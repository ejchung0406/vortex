// Experiment A (X0): go/no-go for SM sharing on a real H200.
//
// A lane-sized decode kernel (1 warp per SM, <= 32 registers, a ring of
// D x T KB of shared memory filled by cp.async.bulk, consumer = ldmatrix +
// mma.sync.m16n8k16 GEMV over M = 1/4/16 tokens) co-runs with a lender:
//   cublas_{bf16,fp8}_{N}      cuBLASLt matmul M=4096 K=3840 N (3840 / 15360)
//   cutlass_{bf16,fp8}[m1]_{N} CUTLASS Hopper warp-specialised cooperative
//                              persistent GEMM, m1 = one fewer stage
//   ew                         y = gelu_tanh(a*b + c), BF16, 4096 x 15360
//   fma2 / fma4                FP32 FMA loop, ILP 2 / 4, 1024 threads per SM
// Lender specs may end in "@sms=<list>": TPC mask + SM-count target.
//
// The run follows a schedule file shared by every process of the run, so the
// same binary does "two streams in one process" (--roles lane,lender) and
// "two MPS clients" (one process --roles lane, the other --roles lender,
// same --t0). Each slot line:
//   slot <k> lane <spec|-> lender <spec|-> first <lane|lender> trial <i> tag <t>
// Times inside a slot (ms from its start S = t0 + k * slot_ms):
//   first role active [0, 2600], second role [400, 2200],
//   a role alone [0, 2200]; measurement window [550, 2050] (1.5 s)
// Lane spec: M=<1|4|16>:r=<GB/s per SM, 0 = unpaced>:t=<tile KB>:d=<depth>
//            [:spin=<ns>][:sms=<list>]
// All rates are computed inside the window from %globaltimer stamps (lane:
// every 64 tiles per CTA; FMA: every chunk per CTA) or from CUDA events after
// every lender launch, mapped to %globaltimer through a stamp kernel.
#include "gng_common.h"
#include "gng_cutlass_api.h"
#include <cublasLt.h>
#include <cuda_bf16.h>
#include <cuda_fp8.h>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>

// ------------------------------------------------------------------ data generators (host and device)
__host__ __device__ __forceinline__ uint32_t mix32(uint64_t x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return (uint32_t)x;
}
// lane weights: value of row r (0..15), column kk (0..15) of kstep q of slice sl: multiples of 1/8 in [-7/8, 7/8]
__host__ __device__ __forceinline__ float wval(uint32_t sl, uint64_t q, int r, int kk) {
  return (float)((int)(mix32(((uint64_t)sl << 40) ^ (q * 256 + r * 16 + kk)) % 15u) - 7) * 0.125f;
}
// lane activations X[n][k], k < 1024: multiples of 1/4 in [-1, 1]; zero for n >= M
__host__ __device__ __forceinline__ float xval(int n, int k, int M) {
  return n < M ? (float)((int)(mix32(0x9e3779b9ull + (uint64_t)n * 4096 + k) % 9u) - 4) * 0.25f : 0.f;
}
// GEMM operands: multiples of 1/4 in [-0.5, 0.5] (exact in BF16 and e4m3)
__host__ __device__ __forceinline__ float gval(uint32_t which, uint64_t i) {
  return (float)((int)(mix32(((uint64_t)which << 48) ^ i) % 5u) - 2) * 0.25f;
}
__host__ __device__ __forceinline__ float eval_(uint32_t which, uint64_t i) {
  return (float)((int)(mix32(((uint64_t)(which + 7) << 48) ^ i) % 17u) - 8) * 0.125f;
}

// lane weight layout: a slice is a sequence of 512-byte ksteps; kstep q holds
// four 8x8 BF16 blocks [rows 0-7 k 0-7][rows 8-15 k 0-7][rows 0-7 k 8-15][rows 8-15 k 8-15]
// so that ldmatrix.x4 with thread t reading 16 bytes at t*16 returns the mma A fragment.
__global__ void k_wgen(__nv_bfloat16* w, uint64_t slice_bytes, int nslices) {
  const uint64_t chunks = slice_bytes / 16 * nslices;
  for (uint64_t c = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x; c < chunks; c += (uint64_t)gridDim.x * blockDim.x) {
    const uint32_t sl = (uint32_t)(c / (slice_bytes / 16));
    const uint64_t cl = c % (slice_bytes / 16);
    const uint64_t q = cl / 32;
    const int mat = (int)((cl % 32) / 8), rr = (int)(cl % 8);
    const int r = rr + 8 * (mat & 1), kk0 = 8 * (mat >> 1);
    __align__(16) __nv_bfloat16 v[8];
    for (int j = 0; j < 8; ++j) v[j] = __float2bfloat16(wval(sl, q, r, kk0 + j));
    reinterpret_cast<uint4*>(w)[c] = *reinterpret_cast<uint4*>(v);
  }
}
template <class T> __global__ void k_ggen(T* p, uint64_t n, uint32_t which) {
  for (uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x; i < n; i += (uint64_t)gridDim.x * blockDim.x) p[i] = T(gval(which, i));
}
__global__ void k_egen(__nv_bfloat16* p, uint64_t n, uint32_t which) {
  for (uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x; i < n; i += (uint64_t)gridDim.x * blockDim.x) p[i] = __float2bfloat16(eval_(which, i));
}

// ------------------------------------------------------------------ lane kernel
struct LaneP {
  const uint8_t* w; const uint2* xf; float* out;
  uint64_t slice_bytes; uint32_t tile_bytes; int ksteps, D, G, spin, max_stamps, consume;
  double ns_per_tile; unsigned long long deadline;
  const unsigned char* allow; int* claim; int* ctr; int* worker_sm;
  unsigned long long *t_start, *t_end, *stamps; unsigned long long* tiles_done;
};
__device__ __forceinline__ uint32_t smem_u32(const void* p) { return (uint32_t)__cvta_generic_to_shared(p); }
__device__ __forceinline__ void mbar_init(uint32_t bar, uint32_t c) { asm volatile("mbarrier.init.shared::cta.b64 [%0], %1;" :: "r"(bar), "r"(c) : "memory"); }
__device__ __forceinline__ void mbar_expect(uint32_t bar, uint32_t tx) {
  asm volatile("mbarrier.arrive.expect_tx.shared::cta.b64 _, [%0], %1;" :: "r"(bar), "r"(tx) : "memory");
}
__device__ __forceinline__ void mbar_wait(uint32_t bar, uint32_t par, int spin) {
  uint32_t ok = 0;
  if (spin > 0) {
    for (;;) {
      asm volatile("{\n\t.reg .pred p;\n\tmbarrier.test_wait.parity.shared::cta.b64 p, [%1], %2;\n\tselp.u32 %0, 1, 0, p;\n\t}"
                   : "=r"(ok) : "r"(bar), "r"(par) : "memory");
      if (ok) return;
      __nanosleep(spin);
    }
  }
  asm volatile("{\n.reg .pred P1;\nWAIT_%=:\n"
               "mbarrier.try_wait.parity.shared::cta.b64 P1, [%0], %1;\n"
               "@!P1 bra WAIT_%=;\n}" :: "r"(bar), "r"(par) : "memory");
  (void)ok;
}
__device__ __forceinline__ void bulk_g2s(uint32_t dst, const void* src, uint32_t bytes, uint32_t bar) {
  asm volatile("cp.async.bulk.shared::cluster.global.mbarrier::complete_tx::bytes [%0], [%1], %2, [%3];"
               :: "r"(dst), "l"(src), "r"(bytes), "r"(bar) : "memory");
}
__device__ __forceinline__ void mma_bf16(float* d, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t b0, uint32_t b1) {
  asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
               : "+f"(d[0]), "+f"(d[1]), "+f"(d[2]), "+f"(d[3]) : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
}
template <int G, int KS>
__global__ void __maxnreg__(32) k_lane(const __grid_constant__ LaneP p) {
  extern __shared__ __align__(1024) uint8_t ring[];
  __shared__ __align__(8) uint64_t bars[8];
  __shared__ int s_o;
  const int lane = threadIdx.x;
  if (lane == 0) {
    const int sm = (int)gng_smid();
    int o = -1;
    if (p.allow[sm] && atomicAdd(&p.claim[sm], 1) == 0) { o = atomicAdd(p.ctr, 1); p.worker_sm[o] = sm; }
    s_o = o;
    if (o >= 0) {
      for (int i = 0; i < p.D; ++i) mbar_init(smem_u32(&bars[i]), 1);
      asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
    }
  }
  __syncwarp();
  const int o = s_o;
  if (o < 0) return;
  const uint8_t* base = p.w + (uint64_t)o * p.slice_bytes;
  const uint32_t nt = (uint32_t)(p.slice_bytes / p.tile_bytes), nblk = nt / 64, D = (uint32_t)p.D;
  const unsigned long long t0 = gng_gt();
  if (lane == 0) {
    p.t_start[o] = t0;
    for (uint32_t i = 0; i < D; ++i) {
      if (p.ns_per_tile > 0) while (gng_gt() < t0 + (unsigned long long)(i * p.ns_per_tile)) __nanosleep(100);
      mbar_expect(smem_u32(&bars[i]), p.tile_bytes);
      bulk_g2s(smem_u32(ring + (size_t)i * p.tile_bytes), base + (uint64_t)(i % nt) * p.tile_bytes, p.tile_bytes, smem_u32(&bars[i]));
    }
  }
  // G n8 groups; for G == 1 two accumulator sets (even / odd k-steps) so that two mma chains are in flight
  constexpr int NA = G == 1 ? 2 : 2;
  float acc[NA][4];
#pragma unroll
  for (int g = 0; g < NA; ++g) acc[g][0] = acc[g][1] = acc[g][2] = acc[g][3] = 0.f;
  int ns = 0;
  uint32_t n = 0, slot = 0, phase = 0;
  const uint32_t ring0 = smem_u32(ring), bar0 = smem_u32(&bars[0]);
  for (;; ++n) {
    mbar_wait(bar0 + 8 * slot, phase, p.spin);
    const uint32_t t = ring0 + slot * p.tile_bytes + lane * 16;
    const uint2* xf = p.xf + lane;
#pragma unroll 4
    for (int s = 0; s < (p.consume ? KS : 0); ++s) {
      uint32_t a0, a1, a2, a3;
      asm volatile("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%0,%1,%2,%3}, [%4];"
                   : "=r"(a0), "=r"(a1), "=r"(a2), "=r"(a3) : "r"(t + s * 512));
      if constexpr (G == 1) {
        const uint2 b = __ldg(xf + s * 32);
        mma_bf16(acc[s & 1], a0, a1, a2, a3, b.x, b.y);
      } else {
        const uint2 b0 = __ldg(xf + s * 32), b1 = __ldg(xf + (64 + s) * 32);
        mma_bf16(acc[0], a0, a1, a2, a3, b0.x, b0.y);
        mma_bf16(acc[1], a0, a1, a2, a3, b1.x, b1.y);
      }
    }
    if ((n & 63) == 63) {
      const uint32_t blk = (n >> 6) % nblk;
      float* out = p.out + ((uint64_t)o * nblk + blk) * 256 + (lane / 4) * 16 + (lane % 4) * 2;
      if constexpr (G == 1) {
        out[0] = acc[0][0] + acc[1][0]; out[1] = acc[0][1] + acc[1][1]; out[128] = acc[0][2] + acc[1][2]; out[129] = acc[0][3] + acc[1][3];
      } else {
#pragma unroll
        for (int g = 0; g < 2; ++g) { out[g * 8] = acc[g][0]; out[g * 8 + 1] = acc[g][1]; out[128 + g * 8] = acc[g][2]; out[128 + g * 8 + 1] = acc[g][3]; }
      }
#pragma unroll
      for (int g = 0; g < NA; ++g) acc[g][0] = acc[g][1] = acc[g][2] = acc[g][3] = 0.f;
      if (lane == 0 && ns < p.max_stamps) p.stamps[(uint64_t)o * p.max_stamps + ns] = gng_gt();
      ++ns;
    }
    __syncwarp();
    int stop = 0;
    if (lane == 0) {
      if (gng_gt() >= p.deadline) stop = 1;
      else {
        const uint32_t m = n + D;
        if (p.ns_per_tile > 0) while (gng_gt() < t0 + (unsigned long long)((double)m * p.ns_per_tile)) __nanosleep(100);
        mbar_expect(bar0 + 8 * slot, p.tile_bytes);
        bulk_g2s(ring0 + slot * p.tile_bytes, base + (uint64_t)(m % nt) * p.tile_bytes, p.tile_bytes, bar0 + 8 * slot);
      }
    }
    stop = __shfl_sync(0xffffffffu, stop, 0);
    if (stop) {
      for (uint32_t j = 1; j < D; ++j) { const uint32_t q = slot + j; mbar_wait(bar0 + 8 * (q % D), phase ^ (q >= D ? 1u : 0u), p.spin); }
      break;
    }
    if (++slot == D) { slot = 0; phase ^= 1; }
  }
  if (lane == 0) { p.t_end[o] = gng_gt(); p.tiles_done[o] = n + 1; }
}

// ------------------------------------------------------------------ FMA lender (persistent, deadline)
struct FmaP {
  const unsigned char* allow; int* claim; int* ctr; int* worker_sm;
  unsigned long long* ts; int* nch; int maxch, ilp; unsigned iters; unsigned long long deadline; float* out;
};
__global__ void __launch_bounds__(1024) k_fma(const __grid_constant__ FmaP p) {
  __shared__ int s_o, s_stop;
  if (threadIdx.x == 0) {
    const int sm = (int)gng_smid();
    int o = -1;
    if (p.allow[sm] && atomicAdd(&p.claim[sm], 1) == 0) { o = atomicAdd(p.ctr, 1); p.worker_sm[o] = sm; }
    s_o = o; s_stop = 0;
  }
  __syncthreads();
  const int o = s_o;
  if (o < 0) return;
  unsigned long long* ts = p.ts + (size_t)o * p.maxch;
  if (threadIdx.x == 0) ts[0] = gng_gt();
  float a0 = threadIdx.x, a1 = a0 + 1, a2 = a0 + 2, a3 = a0 + 3;
  const float mul = 0.999999f, add = 1e-7f;
  int c = 1;
  for (; c < p.maxch; ++c) {
    if (p.ilp == 4) {
      for (unsigned i = 0; i < p.iters; i += 4) { a0 = a0 * mul + add; a1 = a1 * mul + add; a2 = a2 * mul + add; a3 = a3 * mul + add; }
    } else {
      for (unsigned i = 0; i < p.iters; i += 4) { a0 = a0 * mul + add; a1 = a1 * mul + add; a0 = a0 * mul + add; a1 = a1 * mul + add; }
    }
    __syncthreads();
    if (threadIdx.x == 0) { ts[c] = gng_gt(); s_stop = gng_gt() >= p.deadline; }
    __syncthreads();
    if (s_stop) { ++c; break; }
  }
  if (threadIdx.x == 0) p.nch[o] = c;
  if (a0 + a1 + a2 + a3 == 1234.5f) p.out[o] = a0;
}

// ------------------------------------------------------------------ elementwise lender
__device__ __forceinline__ float gelu_tanh(float x) { return 0.5f * x * (1.f + tanhf(0.7978845608f * (x + 0.044715f * x * x * x))); }
__host__ float gelu_tanh_h(float x) { return 0.5f * x * (1.f + tanhf(0.7978845608f * (x + 0.044715f * x * x * x))); }
__global__ void __launch_bounds__(256) k_ew(const uint4* a, const uint4* b, const uint4* c, uint4* y, size_t n8) {
  const size_t i = (blockIdx.x * (size_t)blockDim.x + threadIdx.x) * 2;
#pragma unroll
  for (int j = 0; j < 2; ++j) {
    if (i + j >= n8) return;
    uint4 va = a[i + j], vb = b[i + j], vc = c[i + j], vy;
    const __nv_bfloat162* pa = reinterpret_cast<const __nv_bfloat162*>(&va);
    const __nv_bfloat162* pb = reinterpret_cast<const __nv_bfloat162*>(&vb);
    const __nv_bfloat162* pc = reinterpret_cast<const __nv_bfloat162*>(&vc);
    __nv_bfloat162* py = reinterpret_cast<__nv_bfloat162*>(&vy);
#pragma unroll
    for (int k = 0; k < 4; ++k) {
      const float2 fa = __bfloat1622float2(pa[k]), fb = __bfloat1622float2(pb[k]), fc = __bfloat1622float2(pc[k]);
      py[k] = __floats2bfloat162_rn(gelu_tanh(fa.x * fb.x + fc.x), gelu_tanh(fa.y * fb.y + fc.y));
    }
    y[i + j] = vy;
  }
}

// ------------------------------------------------------------------ host side
static int g_nsm = 132;
static Nvml g_nv;
static const int GM = 4096, GK = 3840;
static const size_t EWN = (size_t)4096 * 15360;

struct Spec { std::map<std::string, std::string> kv; std::string name; };
static Spec parse_spec(const std::string& s) {
  Spec r;
  std::string body = s, opts;
  size_t at = s.find('@');
  if (at != std::string::npos) { body = s.substr(0, at); opts = s.substr(at + 1); }
  r.name = body;
  auto addkv = [&](const std::string& str) {
    size_t i = 0;
    while (i < str.size()) {
      size_t j = str.find(':', i); if (j == std::string::npos) j = str.size();
      std::string t = str.substr(i, j - i);
      size_t e = t.find('=');
      if (e != std::string::npos) r.kv[t.substr(0, e)] = t.substr(e + 1);
      i = j + 1;
    }
  };
  addkv(body); addkv(opts);
  return r;
}
static std::string get(const Spec& s, const char* k, const char* d) { auto it = s.kv.find(k); return it == s.kv.end() ? d : it->second; }
static std::string bitmap_hex(const std::set<int>& sms) {
  uint64_t w[4] = {0, 0, 0, 0};
  for (int s : sms) if (s >= 0 && s < 256) w[s / 64] |= 1ull << (s % 64);
  char b[80]; snprintf(b, sizeof b, "%016llx%016llx%016llx", (unsigned long long)w[2], (unsigned long long)w[1], (unsigned long long)w[0]);
  return b;
}
// work done in [w0, w1] by a unit whose cumulative work is given at time points (monotone)
static double work_in(const std::vector<std::pair<uint64_t, double>>& pts, uint64_t w0, uint64_t w1) {
  auto at = [&](uint64_t t) -> double {
    if (pts.empty() || t <= pts.front().first) return pts.empty() ? 0 : pts.front().second;
    if (t >= pts.back().first) return pts.back().second;
    auto it = std::upper_bound(pts.begin(), pts.end(), std::make_pair(t, -1e300),
                               [](const std::pair<uint64_t, double>& a, const std::pair<uint64_t, double>& b) { return a.first < b.first; });
    const auto& hi = *it; const auto& lo = *(it - 1);
    return lo.second + (hi.second - lo.second) * (double)(t - lo.first) / (double)(hi.first - lo.first);
  };
  return at(w1) - at(w0);
}

// ---------------- lane
struct Lane {
  uint8_t* w = nullptr; uint2* xf[17] = {}; float* out = nullptr;
  uint64_t slice_bytes = 128ull << 20; int nslices = 0;
  unsigned char* allow; int *claim, *ctr, *wsm; unsigned long long *ts0, *ts1, *st, *td;
  int max_stamps = 4096;
  cudaStream_t s;
  void setup(int nsl) {
    nslices = nsl;
    CK(cudaMalloc(&w, slice_bytes * nslices));
    k_wgen<<<g_nsm * 8, 1024>>>((__nv_bfloat16*)w, slice_bytes, nslices);
    for (int M : {1, 4, 16}) {
      const int G = M > 8 ? 2 : 1;
      std::vector<uint2> h((size_t)G * 64 * 32);
      for (int g = 0; g < G; ++g) for (int s = 0; s < 64; ++s) for (int l = 0; l < 32; ++l) {
        const int nn = g * 8 + l / 4, k0 = s * 16 + (l % 4) * 2;
        __nv_bfloat16 v[4] = {__float2bfloat16(xval(nn, k0, M)), __float2bfloat16(xval(nn, k0 + 1, M)),
                              __float2bfloat16(xval(nn, k0 + 8, M)), __float2bfloat16(xval(nn, k0 + 9, M))};
        memcpy(&h[(g * 64 + s) * 32 + l], v, 8);
      }
      CK(cudaMalloc(&xf[M], h.size() * 8)); CK(cudaMemcpy(xf[M], h.data(), h.size() * 8, cudaMemcpyHostToDevice));
    }
    const uint64_t nblk_max = slice_bytes / (64 * 16384);
    CK(cudaMalloc(&out, (size_t)nslices * nblk_max * 256 * 4));
    CK(cudaMalloc(&allow, GNG_MAXSM)); CK(cudaMalloc(&claim, GNG_MAXSM * 4)); CK(cudaMalloc(&ctr, 4)); CK(cudaMalloc(&wsm, GNG_MAXSM * 4));
    CK(cudaMalloc(&ts0, GNG_MAXSM * 8)); CK(cudaMalloc(&ts1, GNG_MAXSM * 8)); CK(cudaMalloc(&td, GNG_MAXSM * 8));
    CK(cudaMalloc(&st, (size_t)GNG_MAXSM * max_stamps * 8));
    CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    for (int D : {1}) (void)D;
    auto attr = [](auto k) {
      CK(cudaFuncSetAttribute(k, cudaFuncAttributeMaxDynamicSharedMemorySize, 220 * 1024));
      CK(cudaFuncSetAttribute(k, cudaFuncAttributePreferredSharedMemoryCarveout, 100));
    };
    attr(k_lane<1, 32>); attr(k_lane<2, 32>); attr(k_lane<1, 64>); attr(k_lane<2, 64>);
    CK(cudaDeviceSynchronize());
    cudaFuncAttributes fa; CK(cudaFuncGetAttributes(&fa, k_lane<2, 32>));
    printf("lane_kernel regs=%d static_smem=%zu local=%zu (G=2)\n", fa.numRegs, fa.sharedSizeBytes, fa.localSizeBytes);
    CK(cudaFuncGetAttributes(&fa, k_lane<1, 32>));
    printf("lane_kernel regs=%d static_smem=%zu local=%zu (G=1)\n", fa.numRegs, fa.sharedSizeBytes, fa.localSizeBytes);
  }
  // returns result string
  std::string run(const Spec& sp, int64_t start_h, int64_t end_h, int64_t w0h, int64_t w1h, std::set<int>* sms_out) {
    const int M = atoi(get(sp, "M", "16").c_str()), tkb = atoi(get(sp, "t", "16").c_str()), D = atoi(get(sp, "d", "2").c_str());
    const double rate = atof(get(sp, "r", "0").c_str());
    const int spin = atoi(get(sp, "spin", "0").c_str()), consume = atoi(get(sp, "c", "1").c_str());
    const std::string sml = get(sp, "sms", "all");
    std::vector<int> smv; if (sml == "all") { for (int i = 0; i < g_nsm; ++i) smv.push_back(i); } else smv = parse_list(sml);
    std::vector<unsigned char> al(GNG_MAXSM, 0); for (int x : smv) al[x] = 1;
    const u128 mask = sml == "all" ? 0 : mask_of_sms(smv);
    int grid = sml == "all" ? 2 * g_nsm : 4 * u128_popc(mask);
    LaneP p{};
    p.w = w; p.xf = xf[M]; p.out = out; p.slice_bytes = slice_bytes; p.tile_bytes = tkb * 1024; p.ksteps = tkb * 1024 / 512;
    p.D = D; p.G = M > 8 ? 2 : 1; p.spin = spin; p.max_stamps = max_stamps; p.consume = consume;
    p.ns_per_tile = rate > 0 ? p.tile_bytes / rate : 0;
    p.allow = allow; p.claim = claim; p.ctr = ctr; p.worker_sm = wsm; p.t_start = ts0; p.t_end = ts1; p.stamps = st; p.tiles_done = td;
    CK(cudaMemcpyAsync(allow, al.data(), GNG_MAXSM, cudaMemcpyHostToDevice, s));
    CK(cudaMemsetAsync(claim, 0, GNG_MAXSM * 4, s)); CK(cudaMemsetAsync(ctr, 0, 4, s)); CK(cudaMemsetAsync(wsm, 0xff, GNG_MAXSM * 4, s));
    CK(cudaMemsetAsync(st, 0, (size_t)GNG_MAXSM * max_stamps * 8, s));
    CK(cudaStreamSynchronize(s));
    gng_mask(mask);
    const double late = (host_ns() - start_h) / 1e6;
    sleep_until_host(start_h);
    p.deadline = host_to_gt(end_h);
    const size_t smem = (size_t)D * p.tile_bytes;
    if (p.ksteps == 32) { if (p.G == 2) k_lane<2, 32><<<grid, 32, smem, s>>>(p); else k_lane<1, 32><<<grid, 32, smem, s>>>(p); }
    else if (p.ksteps == 64) { if (p.G == 2) k_lane<2, 64><<<grid, 32, smem, s>>>(p); else k_lane<1, 64><<<grid, 32, smem, s>>>(p); }
    else { fprintf(stderr, "lane tile must be 16 or 32 KB\n"); exit(1); }
    CK(cudaGetLastError());
    const int64_t launched = host_ns();
    CK(cudaStreamSynchronize(s));
    gng_mask(0);
    int nw = 0; CK(cudaMemcpy(&nw, ctr, 4, cudaMemcpyDeviceToHost));
    std::vector<int> ws(nw); std::vector<unsigned long long> a(nw), b(nw), tdn(nw), sts((size_t)nw * max_stamps);
    CK(cudaMemcpy(ws.data(), wsm, nw * 4, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(a.data(), ts0, nw * 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(b.data(), ts1, nw * 8, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(tdn.data(), td, nw * 8, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(sts.data(), st, sts.size() * 8, cudaMemcpyDeviceToHost));
    const uint64_t g0 = host_to_gt(w0h), g1 = host_to_gt(w1h);
    double bytes = 0; std::vector<double> per;
    std::set<int> sms; unsigned long long latest_start = 0;
    // 10 ms bins of the chip total, to find the worst stretch
    const int nb = (int)((g1 - g0) / 10000000ull);
    std::vector<double> bins(std::max(nb, 1), 0.0);
    for (int i = 0; i < nw; ++i) {
      sms.insert(ws[i]);
      latest_start = std::max(latest_start, a[i]);
      std::vector<std::pair<uint64_t, double>> pts;
      pts.push_back({a[i], 0});
      const uint64_t nsk = std::min<uint64_t>(tdn[i] / 64, max_stamps);
      for (uint64_t k = 0; k < nsk; ++k) pts.push_back({sts[(size_t)i * max_stamps + k], (double)(k + 1) * 64 * p.tile_bytes});
      pts.push_back({b[i], (double)tdn[i] * p.tile_bytes});
      const double x = work_in(pts, g0, g1);
      bytes += x; per.push_back(x / (double)(g1 - g0));
      for (int k = 0; k < nb; ++k) bins[k] += work_in(pts, g0 + k * 10000000ull, g0 + (k + 1) * 10000000ull) / 1e7;
    }
    std::sort(bins.begin(), bins.end());
    // verification of 2 (worker, block) pairs whose block finished at least once
    int checked = 0, bad = 0;
    const uint64_t nt = slice_bytes / p.tile_bytes, nblk = nt / 64;
    std::vector<float> xh(16 * 1024);
    for (int nn = 0; nn < 16; ++nn) for (int k = 0; k < 1024; ++k) xh[nn * 1024 + k] = xval(nn, k, M);
    const int NN = M > 8 ? 16 : 8;
    for (int c = 0; c < 2 && nw > 0 && consume; ++c) {
      const int i = (int)(mix32(c * 7919ull + nw + host_ns()) % nw);
      const uint64_t done = tdn[i] / 64;
      if (done == 0) continue;
      const uint64_t blk = mix32(c * 104729ull + host_ns()) % std::min<uint64_t>(done, nblk);
      std::vector<float> got(256);
      CK(cudaMemcpy(got.data(), out + ((uint64_t)i * nblk + blk) * 256, 1024, cudaMemcpyDeviceToHost));
      std::vector<double> ref(256, 0.0);
      for (uint64_t tl = blk * 64; tl < blk * 64 + 64; ++tl)
        for (int s2 = 0; s2 < p.ksteps; ++s2) {
          const uint64_t q = tl * p.ksteps + s2;
          for (int r = 0; r < 16; ++r) for (int kk = 0; kk < 16; ++kk) {
            const double wv = wval((uint32_t)i, q, r, kk);
            for (int nn = 0; nn < NN; ++nn) ref[r * 16 + nn] += wv * xh[nn * 1024 + s2 * 16 + kk];
          }
        }
      for (int r = 0; r < 16; ++r) for (int nn = 0; nn < NN; ++nn)
        if (fabs(ref[r * 16 + nn] - got[r * 16 + nn]) > 1e-3 * (1 + fabs(ref[r * 16 + nn]))) ++bad;
      ++checked;
    }
    if (sms_out) *sms_out = sms;
    char buf[1024];
    const double win = (double)(g1 - g0);
    const double flop = bytes / 2 * 2 * M;  // useful FLOPs: 2*M per weight element
    snprintf(buf, sizeof buf,
             "lane_consume=%d lane_M=%d lane_rate_target=%.0f tile_kb=%d depth=%d spin=%d lane_workers=%d lane_sms_bitmap=%s lane_gbs=%.1f lane_gbs_per_sm=%.2f "
             "lane_per_sm_min=%.2f lane_per_sm_max=%.2f lane_bin10ms_p5=%.1f lane_bin10ms_min=%.1f lane_late_ms=%.2f lane_useful_tflops=%.3f lane_check=%s(%d blocks,%d bad) lane_late_start_ms=%.1f",
             consume, M, rate, tkb, D, spin, nw, bitmap_hex(sms).c_str(), bytes / win, nw ? bytes / win / nw : 0,
             per.empty() ? 0 : *std::min_element(per.begin(), per.end()), per.empty() ? 0 : *std::max_element(per.begin(), per.end()),
             bins[bins.size() / 20], bins.front(), nw ? ((double)latest_start - (double)host_to_gt(launched)) / 1e6 : -1, flop / win / 1e3,
             checked == 0 ? "none" : (bad ? "FAIL" : "ok"), checked, bad, late);
    return buf;
  }
};

// ---------------- lenders
struct GemmBufs {
  void* A16; void* B16; void* A8; void* B8; void* D; void* ws; size_t wsz = 64 << 20;
  std::vector<float> hA, hB;  // host copies of the values (same for BF16 and FP8: exact in both)
};
static GemmBufs g_gb;
static cublasLtHandle_t g_lt;
struct CublasG {
  cublasLtMatmulDesc_t op; cublasLtMatrixLayout_t la, lb, lc; cublasLtMatmulAlgo_t algo; bool fp8; int N; int smt;
  std::string kname;
  void init(bool f8, int n, int sm_target) {
    fp8 = f8; N = n; smt = sm_target;
    const cudaDataType_t ab = f8 ? CUDA_R_8F_E4M3 : CUDA_R_16BF;
    cublasLtMatmulDescCreate(&op, CUBLAS_COMPUTE_32F, CUDA_R_32F);
    cublasOperation_t ta = CUBLAS_OP_T, tb = CUBLAS_OP_N;
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSA, &ta, sizeof ta);
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSB, &tb, sizeof tb);
    if (f8) { int8_t fa = 1; cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_FAST_ACCUM, &fa, sizeof fa); }
    if (sm_target > 0) cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET, &sm_target, sizeof sm_target);
    // column-major view: D^T (N x M) = B^T (N x K, stored K x N col-major, op T) * A^T (K x M, = A row-major)
    cublasLtMatrixLayoutCreate(&la, ab, GK, N, GK);
    cublasLtMatrixLayoutCreate(&lb, ab, GK, GM, GK);
    cublasLtMatrixLayoutCreate(&lc, CUDA_R_16BF, N, GM, N);
    cublasLtMatmulPreference_t pref; cublasLtMatmulPreferenceCreate(&pref);
    cublasLtMatmulPreferenceSetAttribute(pref, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &g_gb.wsz, sizeof g_gb.wsz);
    cublasLtMatmulHeuristicResult_t r[1]; int nr = 0;
    if (cublasLtMatmulAlgoGetHeuristic(g_lt, op, la, lb, lc, lc, pref, 1, r, &nr) != CUBLAS_STATUS_SUCCESS || nr == 0) {
      fprintf(stderr, "cublasLt: no algorithm (fp8=%d N=%d)\n", f8, N); exit(1);
    }
    algo = r[0].algo;
  }
  void launch(cudaStream_t s) {
    const float al = 1.f, be = 0.f;
    const void* A = fp8 ? g_gb.A8 : g_gb.A16; const void* B = fp8 ? g_gb.B8 : g_gb.B16;
    cublasStatus_t st = cublasLtMatmul(g_lt, op, &al, B, la, A, lb, &be, g_gb.D, lc, g_gb.D, lc, &algo, g_gb.ws, g_gb.wsz, s);
    if (st != CUBLAS_STATUS_SUCCESS) { fprintf(stderr, "cublasLtMatmul failed %d\n", (int)st); exit(1); }
  }
};
// host reference check of 32 sampled outputs of D (row-major M x N)
static std::string check_gemm(int N) {
  std::vector<__nv_bfloat16> d((size_t)GM * N);
  CK(cudaMemcpy(d.data(), g_gb.D, d.size() * 2, cudaMemcpyDeviceToHost));
  int bad = 0;
  for (int c = 0; c < 32; ++c) {
    const int i = mix32(c * 31ull + N) % GM, j = mix32(c * 37ull + 5 + N) % N;
    double ref = 0;
    for (int k = 0; k < GK; ++k) ref += (double)g_gb.hA[(size_t)i * GK + k] * g_gb.hB[(size_t)j * GK + k];
    const double got = __bfloat162float(d[(size_t)i * N + j]);
    if (fabs(got - ref) > 0.01 * (1 + fabs(ref))) ++bad;
  }
  return bad ? "FAIL" : "ok";
}

struct Lender {
  // persistent FMA
  unsigned char* allow; int *claim, *ctr, *wsm, *nch; unsigned long long* ts; float* fout; int maxch = 16384; unsigned iters = 65536;
  // ew
  __nv_bfloat16 *ea, *eb, *ec, *ey;
  // gemm
  std::map<std::string, std::unique_ptr<gngc::Runner>> cut;
  std::map<std::string, std::unique_ptr<CublasG>> cub;
  int* csmid;  // [8][grid]
  cudaStream_t s;
  std::vector<cudaEvent_t> ev; cudaEvent_t e0; unsigned long long* dstamp;
  void setup() {
    CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    CK(cudaMalloc(&allow, GNG_MAXSM)); CK(cudaMalloc(&claim, GNG_MAXSM * 4)); CK(cudaMalloc(&ctr, 4)); CK(cudaMalloc(&wsm, GNG_MAXSM * 4));
    CK(cudaMalloc(&nch, GNG_MAXSM * 4)); CK(cudaMalloc(&ts, (size_t)GNG_MAXSM * maxch * 8)); CK(cudaMalloc(&fout, GNG_MAXSM * 4));
    CK(cudaFuncSetAttribute(k_fma, cudaFuncAttributePreferredSharedMemoryCarveout, 100));
    CK(cudaFuncSetAttribute(k_ew, cudaFuncAttributePreferredSharedMemoryCarveout, 100));
    for (auto** p : {&ea, &eb, &ec, &ey}) CK(cudaMalloc(p, EWN * 2));
    k_egen<<<g_nsm * 4, 1024>>>(ea, EWN, 1); k_egen<<<g_nsm * 4, 1024>>>(eb, EWN, 2); k_egen<<<g_nsm * 4, 1024>>>(ec, EWN, 3);
    const int NMAX = 15360;
    CK(cudaMalloc(&g_gb.A16, (size_t)GM * GK * 2)); CK(cudaMalloc(&g_gb.B16, (size_t)NMAX * GK * 2));
    CK(cudaMalloc(&g_gb.A8, (size_t)GM * GK)); CK(cudaMalloc(&g_gb.B8, (size_t)NMAX * GK));
    CK(cudaMalloc(&g_gb.D, (size_t)GM * NMAX * 2)); CK(cudaMalloc(&g_gb.ws, g_gb.wsz));
    k_ggen<__nv_bfloat16><<<g_nsm * 4, 1024>>>((__nv_bfloat16*)g_gb.A16, (uint64_t)GM * GK, 1);
    k_ggen<__nv_bfloat16><<<g_nsm * 4, 1024>>>((__nv_bfloat16*)g_gb.B16, (uint64_t)NMAX * GK, 2);
    k_ggen<__nv_fp8_e4m3><<<g_nsm * 4, 1024>>>((__nv_fp8_e4m3*)g_gb.A8, (uint64_t)GM * GK, 1);
    k_ggen<__nv_fp8_e4m3><<<g_nsm * 4, 1024>>>((__nv_fp8_e4m3*)g_gb.B8, (uint64_t)NMAX * GK, 2);
    g_gb.hA.resize((size_t)GM * GK); g_gb.hB.resize((size_t)NMAX * GK);
    for (size_t i = 0; i < g_gb.hA.size(); ++i) g_gb.hA[i] = gval(1, i);
    for (size_t i = 0; i < g_gb.hB.size(); ++i) g_gb.hB[i] = gval(2, i);
    cublasLtCreate(&g_lt);
    CK(cudaMalloc(&csmid, 8 * 1024 * 4));
    ev.resize(60000); for (auto& e : ev) CK(cudaEventCreate(&e)); CK(cudaEventCreate(&e0));
    CK(cudaMalloc(&dstamp, 8));
    CK(cudaDeviceSynchronize());
  }
  gngc::Runner* get_cut(const std::string& kind, int N, int smc) {
    const std::string key = kind + "_" + std::to_string(N) + "_" + std::to_string(smc);
    auto it = cut.find(key);
    if (it != cut.end()) return it->second.get();
    std::unique_ptr<gngc::Runner> r(gngc::make_runner(kind));
    if (!r) { fprintf(stderr, "unknown cutlass kind %s\n", kind.c_str()); exit(1); }
    const bool f8 = kind.rfind("fp8", 0) == 0;
    if (!r->init(f8 ? g_gb.A8 : g_gb.A16, f8 ? g_gb.B8 : g_gb.B16, g_gb.D, GM, N, GK, smc)) exit(1);
    printf("cutlass_kernel kind=%s N=%d sm_count=%d grid=%d threads=%d regs_per_thread=%d smem_bytes=%d stages=%d regs_per_cta=%d\n",
           kind.c_str(), N, smc, r->grid(), r->threads(), r->regs(), r->smem(), r->stages(), r->regs() * r->threads());
    gngc::Runner* p = r.get(); cut[key] = std::move(r); return p;
  }
  CublasG* get_cub(bool f8, int N, int smt) {
    const std::string key = std::to_string(f8) + "_" + std::to_string(N) + "_" + std::to_string(smt);
    auto it = cub.find(key);
    if (it != cub.end()) return it->second.get();
    std::unique_ptr<CublasG> c(new CublasG()); c->init(f8, N, smt);
    CublasG* p = c.get(); cub[key] = std::move(c); return p;
  }
  // prepare (host-side setup outside the timed region)
  void prepare(const Spec& sp) {
    const std::string& n = sp.name;
    const std::string sml = get(sp, "sms", "all");
    const int smc = sml == "all" ? g_nsm : (int)parse_list(sml).size();
    if (n.rfind("cutlass_", 0) == 0) { std::string k = n.substr(8, n.rfind('_') - 8); get_cut(k, atoi(n.substr(n.rfind('_') + 1).c_str()), smc); }
    if (n.rfind("cublas_", 0) == 0) get_cub(n.find("fp8") != std::string::npos, atoi(n.substr(n.rfind('_') + 1).c_str()), sml == "all" ? 0 : smc);
  }
  std::string run(const Spec& sp, int64_t start_h, int64_t end_h, int64_t w0h, int64_t w1h, const std::set<int>* lane_sms) {
    const std::string& n = sp.name;
    const std::string sml = get(sp, "sms", "all");
    std::vector<int> smv; if (sml == "all") { for (int i = 0; i < g_nsm; ++i) smv.push_back(i); } else smv = parse_list(sml);
    const u128 mask = sml == "all" ? 0 : mask_of_sms(smv);
    const uint64_t g0 = host_to_gt(w0h), g1 = host_to_gt(w1h);
    char buf[1024];
    if (n == "fma2" || n == "fma4") {
      std::vector<unsigned char> al(GNG_MAXSM, 0); for (int x : smv) al[x] = 1;
      CK(cudaMemcpyAsync(allow, al.data(), GNG_MAXSM, cudaMemcpyHostToDevice, s));
      CK(cudaMemsetAsync(claim, 0, GNG_MAXSM * 4, s)); CK(cudaMemsetAsync(ctr, 0, 4, s)); CK(cudaStreamSynchronize(s));
      FmaP p{allow, claim, ctr, wsm, ts, nch, maxch, n == "fma4" ? 4 : 2, iters, 0, fout};
      gng_mask(mask);
      sleep_until_host(start_h);
      p.deadline = host_to_gt(end_h);
      k_fma<<<sml == "all" ? 2 * g_nsm : 4 * u128_popc(mask), 1024, 0, s>>>(p);
      CK(cudaGetLastError());
      CK(cudaStreamSynchronize(s));
      gng_mask(0);
      int nw = 0; CK(cudaMemcpy(&nw, ctr, 4, cudaMemcpyDeviceToHost));
      std::vector<int> c(nw), ws(nw); CK(cudaMemcpy(c.data(), nch, nw * 4, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(ws.data(), wsm, nw * 4, cudaMemcpyDeviceToHost));
      std::vector<unsigned long long> t((size_t)nw * maxch); CK(cudaMemcpy(t.data(), ts, t.size() * 8, cudaMemcpyDeviceToHost));
      double fl = 0; std::set<int> sms;
      const double per_chunk = (double)iters * 1024 * 2;
      for (int i = 0; i < nw; ++i) {
        sms.insert(ws[i]);
        std::vector<std::pair<uint64_t, double>> pts;
        for (int k = 0; k < c[i]; ++k) pts.push_back({t[(size_t)i * maxch + k], k * per_chunk});
        fl += work_in(pts, g0, g1);
      }
      int shared = 0; if (lane_sms) for (int x : sms) shared += lane_sms->count(x);
      snprintf(buf, sizeof buf, "lender=%s lender_workers=%d lender_sms_bitmap=%s lender_tflops=%.3f lender_rate=%.4f lender_rate_unit=TFLOP/s sms_shared_with_lane=%d lender_check=n/a",
               n.c_str(), nw, bitmap_hex(sms).c_str(), fl / (double)(g1 - g0) / 1e3, fl / (double)(g1 - g0) / 1e3, lane_sms ? shared : -1);
      return buf;
    }
    // launch-loop lenders
    std::function<void()> launch; double work_per = 0; std::string unit = "TFLOP/s"; int N = 0;
    gngc::Runner* cr = nullptr; CublasG* cb = nullptr;
    const int smc = (int)smv.size();
    if (n == "ew") {
      const size_t n8 = EWN / 8; const int blocks = (int)((n8 + 511) / 512);
      launch = [=] { k_ew<<<blocks, 256, 0, s>>>((const uint4*)ea, (const uint4*)eb, (const uint4*)ec, (uint4*)ey, n8); };
      work_per = EWN * 2.0 * 4 / 1e3; unit = "TB/s";  // bytes moved (3 reads + 1 write) / 1e3 -> TB/s when divided by ns
    } else if (n.rfind("cutlass_", 0) == 0) {
      std::string k = n.substr(8, n.rfind('_') - 8); N = atoi(n.substr(n.rfind('_') + 1).c_str());
      cr = get_cut(k, N, smc);
      int li = 0;
      launch = [=]() mutable { if (!cr->launch(s, csmid + (li++ % 8) * 1024)) { fprintf(stderr, "cutlass launch failed\n"); exit(1); } };
      work_per = 2.0 * GM * N * GK / 1e3;
    } else if (n.rfind("cublas_", 0) == 0) {
      N = atoi(n.substr(n.rfind('_') + 1).c_str());
      cb = get_cub(n.find("fp8") != std::string::npos, N, sml == "all" ? 0 : smc);
      launch = [=] { cb->launch(s); };
      work_per = 2.0 * GM * N * GK / 1e3;
    } else { fprintf(stderr, "unknown lender %s\n", n.c_str()); exit(1); }
    if (cr) CK(cudaMemsetAsync(csmid, 0xff, 8 * 1024 * 4, s));
    CK(cudaStreamSynchronize(s));
    gng_mask(mask);
    const double late = (host_ns() - start_h) / 1e6;
    sleep_until_host(start_h);
    gng_k_stamp<<<1, 32, 0, s>>>(dstamp); CK(cudaEventRecord(e0, s));
    size_t i = 0;
    while (host_ns() < end_h && i < ev.size()) {
      launch(); CK(cudaEventRecord(ev[i], s)); ++i;
      if (i >= 4) CK(cudaEventSynchronize(ev[i - 4]));
    }
    CK(cudaStreamSynchronize(s));
    gng_mask(0);
    CK(cudaGetLastError());
    unsigned long long gs; CK(cudaMemcpy(&gs, dstamp, 8, cudaMemcpyDeviceToHost));
    std::vector<std::pair<uint64_t, double>> pts; pts.push_back({gs, 0});
    std::vector<double> dur;
    float prev = 0;
    for (size_t k = 0; k < i; ++k) {
      float ms; CK(cudaEventElapsedTime(&ms, e0, ev[k]));
      pts.push_back({gs + (uint64_t)(ms * 1e6), (double)(k + 1)});
      if (pts.back().first >= g0 && pts.back().first <= g1) dur.push_back(ms - prev);
      prev = ms;
    }
    const double launches = work_in(pts, g0, g1);
    std::string chk = "n/a";
    if (N) chk = check_gemm(N);
    else if (n == "ew") {
      std::vector<__nv_bfloat16> y(EWN); CK(cudaMemcpy(y.data(), ey, EWN * 2, cudaMemcpyDeviceToHost));
      int bad = 0;
      for (int c = 0; c < 64; ++c) {
        const size_t j = mix32(c * 977ull) % EWN;
        const float ref = gelu_tanh_h(eval_(1, j) * eval_(2, j) + eval_(3, j));
        if (fabs(__bfloat162float(y[j]) - ref) > 0.02 * (1 + fabs(ref))) ++bad;
      }
      chk = bad ? "FAIL" : "ok";
    }
    std::string extra;
    if (cr) {
      std::vector<int> sm(8 * 1024); CK(cudaMemcpy(sm.data(), csmid, sm.size() * 4, cudaMemcpyDeviceToHost));
      std::set<int> sms; int filled = 0;
      for (int l = 0; l < 8; ++l) for (int c = 0; c < cr->grid(); ++c) if (sm[l * 1024 + c] >= 0) { sms.insert(sm[l * 1024 + c]); ++filled; }
      int shared = 0; if (lane_sms) for (int x : sms) shared += lane_sms->count(x);
      char b2[256];
      snprintf(b2, sizeof b2, " gemm_grid=%d gemm_sms_last8=%zu gemm_sms_bitmap=%s sms_shared_with_lane=%d", cr->grid(), sms.size(), bitmap_hex(sms).c_str(), lane_sms ? shared : -1);
      extra = b2;
    }
    snprintf(buf, sizeof buf, "lender=%s lender_launches=%zu launches_in_window=%.1f launch_ms_med=%.4f lender_rate=%.4f lender_rate_unit=%s lender_check=%s lender_late_start_ms=%.1f%s",
             n.c_str(), i, launches, median_of(dur), launches * work_per / (double)(g1 - g0), unit.c_str(), chk.c_str(), late, extra.c_str());
    return buf;
  }
};

int main(int argc, char** argv) {
  std::string map, sched, roles = "lane,lender", only_tag;
  int64_t t0 = 0; int slot_ms = 2600, nvml_ms = 10, nslices = 0, first_slot = 0, last_slot = 1 << 30;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--map")) map = argv[++i];
    else if (is("--schedule")) sched = argv[++i];
    else if (is("--roles")) roles = argv[++i];
    else if (is("--t0")) t0 = atoll(argv[++i]);
    else if (is("--slot-ms")) slot_ms = atoi(argv[++i]);
    else if (is("--nvml-ms")) nvml_ms = atoi(argv[++i]);
    else if (is("--slices")) nslices = atoi(argv[++i]);
    else if (is("--first-slot")) first_slot = atoi(argv[++i]);
    else if (is("--last-slot")) last_slot = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  const bool do_lane = roles.find("lane") != std::string::npos, do_lender = roles.find("lender") != std::string::npos;
  load_map(map);
  cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, 0));
  g_nsm = prop.multiProcessorCount;
  char host[128]; gethostname(host, sizeof host);
  printf("host=%s gpu=%s sms=%d roles=%s mps_pipe=%s pid=%d\n", host, prop.name, g_nsm, roles.c_str(), getenv("CUDA_MPS_PIPE_DIRECTORY") ? getenv("CUDA_MPS_PIPE_DIRECTORY") : "none", getpid());
  struct Slot { int k; std::string lane, lender, first, tag; int trial; };
  std::vector<Slot> slots;
  {
    FILE* f = fopen(sched.c_str(), "r");
    if (!f) { fprintf(stderr, "no schedule %s\n", sched.c_str()); return 1; }
    char l[4096];
    while (fgets(l, sizeof l, f)) {
      std::istringstream is(l); std::string w; Slot s{}; s.k = -1;
      while (is >> w) {
        if (w == "slot") is >> s.k; else if (w == "lane") is >> s.lane; else if (w == "lender") is >> s.lender;
        else if (w == "first") is >> s.first; else if (w == "trial") is >> s.trial; else if (w == "tag") is >> s.tag;
      }
      if (s.k >= first_slot && s.k <= last_slot) slots.push_back(s);
    }
    fclose(f);
  }
  Lane lane; Lender lender;
  if (do_lane) lane.setup(nslices ? nslices : g_nsm);
  if (do_lender) {
    lender.setup();
    for (auto& s : slots) if (s.lender != "-") lender.prepare(parse_spec(s.lender));
  }
  calibrate_gt();
  g_nv.start(nvml_ms);
  printf("setup_done host_ns=%lld t0=%lld margin_ms=%.0f gt_offset=%lld\n", (long long)host_ns(), (long long)t0, (t0 - host_ns()) / 1e6, (long long)g_gt_off);
  fflush(stdout);
  if (t0 < host_ns() + 500000000ll) { printf("WARNING t0 too early; shifting\n"); t0 = host_ns() + 1000000000ll; }
  for (auto& s : slots) {
    const int64_t S = t0 + (int64_t)s.k * slot_ms * 1000000ll;
    const bool hl = s.lane != "-", hd = s.lender != "-";
    const bool both = hl && hd;
    auto times = [&](bool is_lane, int64_t& a, int64_t& b) {
      if (!both) { a = S; b = S + 2000000000ll; return; }
      const bool first = (s.first == "lane") == is_lane;
      if (first) { a = S; b = S + 2300000000ll; } else { a = S + 300000000ll; b = S + 2000000000ll; }
    };
    const int64_t w0 = S + 450000000ll, w1 = S + 1850000000ll;
    std::string rl, rd;
    std::set<int> lsms;
    std::thread tl, td;
    if (hl && do_lane) { int64_t a, b; times(true, a, b); tl = std::thread([&, a, b] { rl = lane.run(parse_spec(s.lane), a, b, w0, w1, &lsms); }); }
    if (hd && do_lender) {
      int64_t a, b; times(false, a, b);
      td = std::thread([&, a, b] { rd = lender.run(parse_spec(s.lender), a, b, w0, w1, nullptr); });
    }
    if (tl.joinable()) tl.join();
    if (td.joinable()) td.join();
    // SMs shared: recompute with the lane's SM set when both ran here
    if (!rd.empty() && !lsms.empty()) {
      auto p = rd.find("sms_shared_with_lane=-1");
      if (p != std::string::npos) {
        // count from bitmap
        auto q = rd.find("_sms_bitmap=");
        if (q != std::string::npos) {
          std::string hx = rd.substr(q + 12, 48); int sh = 0;
          for (int sm : lsms) { const int word = 2 - sm / 64; const uint64_t v = strtoull(hx.substr(word * 16, 16).c_str(), nullptr, 16); if (v >> (sm % 64) & 1) ++sh; }
          rd.replace(p, strlen("sms_shared_with_lane=-1"), "sms_shared_with_lane=" + std::to_string(sh));
        }
      }
    }
    printf("slot=%d tag=%s trial=%d first=%s mode=%s lane_spec=%s lender_spec=%s %s %s win_ms=%.0f %s\n", s.k, s.tag.c_str(), s.trial, s.first.c_str(),
           roles.c_str(), s.lane.c_str(), s.lender.c_str(), rl.c_str(), rd.c_str(), (w1 - w0) / 1e6, g_nv.summary(w0, w1).c_str());
    fflush(stdout);
  }
  g_nv.finish();
  printf("done mask_callbacks=%ld mask_applied=%ld\n", (long)gngm::g_cb, (long)gngm::g_applied);
  return 0;
}
