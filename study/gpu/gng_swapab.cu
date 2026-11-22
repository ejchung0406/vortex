// Experiment B: go/no-go for bypassing shared memory in a decode GEMM on H200.
//
// "Swap-AB" wgmma decode: weights are the 64-row A operand (m64), the 16
// tokens are N (m64n16k16, BF16 in, FP32 accumulate). A row block is 64
// weight rows x K = 1024 (128 KB); each warpgroup owns every NWG-th row
// block of its CTA's slice and writes C = W_blk * X^T (64 x 16 FP32).
// X (16 x 1024 BF16, 32 KB) sits in shared memory in the canonical
// no-swizzle K-major layout and is the wgmma B operand in every variant.
// Modes (--mode):
//   ss        (1) A and B from shared memory; A staged by cp.async.bulk into a
//                 ring of S stages (producer warp), A read by wgmma descriptors
//   rs_smem   (2) A in registers, loaded from the TMA-staged ring with
//                 ld.shared.v4 (data pre-permuted into fragment order)
//   rs_gmem   (3) A in registers, loaded straight from global memory with
//                 16-byte ld.global.nc (fragment order), P k-steps in flight
//   tma_only  control: the ring is filled, consumers release without reading
//   smem_read control: ring + ld.shared.v4 of every byte, no wgmma
//   ld_only   control: (3)'s loads without wgmma
//   cublas    padded-M baseline: cuBLASLt BF16, D[M x N] = X[M x K] W^T,
//             K = 4096, N = 28672 (235 MB of weights), back to back
// Placement: --sms LIST (TPC mask of their TPCs + %smid filter, 1 CTA per SM).
// Each trial runs >= --secs seconds (work sized from a short calibration run);
// rate = bytes of weights / (t_end - t_start) per CTA, %globaltimer.
#include "gng_common.h"
#include <cublasLt.h>
#include <cuda_bf16.h>
#include <set>

__host__ __device__ __forceinline__ uint32_t mix32(uint64_t x) {
  x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33; return (uint32_t)x;
}
// W of global row block gb: row r (0..63), column k (0..1023), multiples of 1/8 in [-7/8, 7/8]
__host__ __device__ __forceinline__ float wv(uint64_t gb, int r, int k) { return (float)((int)(mix32((gb << 20) ^ (r * 1024 + k)) % 15u) - 7) * 0.125f; }
__host__ __device__ __forceinline__ float xv(int n, int k) { return (float)((int)(mix32(0x51ull + n * 4096ull + k) % 9u) - 4) * 0.25f; }

static const int KSTEPS = 64;            // K = 1024
static const int BLK_BYTES = 64 * 1024 * 2;  // 128 KB per row block

// canonical (no swizzle, K-major) offset inside one k-step of a R-row operand:
// core matrix (8 rows x 16 bytes) of row group i, k half j at (i*2 + j)*128
__host__ __device__ __forceinline__ uint32_t canon_off(int r, int kk) { return ((r / 8) * 2 + kk / 8) * 128 + (r % 8) * 16 + (kk % 8) * 2; }
// fragment order (mma A layout per warp of the warpgroup), 2 KB per k-step:
// thread (warp w, lane l) holds 8 BF16 at w*512 + l*16
__host__ __device__ __forceinline__ void frag_rc(int w, int l, int j, int& r, int& kk) {
  r = 16 * w + l / 4 + 8 * ((j >> 1) & 1); kk = (l % 4) * 2 + (j & 1) + 8 * (j >> 2);
}
__global__ void k_gen(__nv_bfloat16* canon, __nv_bfloat16* frag, uint64_t nblocks) {
  const uint64_t chunks = nblocks * KSTEPS * 128;  // 16-byte chunks: 2 KB per k-step
  for (uint64_t c = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x; c < chunks; c += (uint64_t)gridDim.x * blockDim.x) {
    const uint64_t gb = c / (KSTEPS * 128); const int s = (int)(c / 128 % KSTEPS), q = (int)(c % 128);
    __align__(16) __nv_bfloat16 v[8];
    // canon: chunk q of the k-step = core matrix q/8, row q%8
    { const int core = q / 8, rr = q % 8, r = (core / 2) * 8 + rr, kk0 = (core % 2) * 8;
      for (int j = 0; j < 8; ++j) v[j] = __float2bfloat16(wv(gb, r, s * 16 + kk0 + j));
      reinterpret_cast<uint4*>(canon)[c] = *reinterpret_cast<uint4*>(v); }
    { const int w = q / 32, l = q % 32;
      for (int j = 0; j < 8; ++j) { int r, kk; frag_rc(w, l, j, r, kk); v[j] = __float2bfloat16(wv(gb, r, s * 16 + kk)); }
      reinterpret_cast<uint4*>(frag)[c] = *reinterpret_cast<uint4*>(v); }
  }
}

__device__ __forceinline__ uint32_t su32(const void* p) { return (uint32_t)__cvta_generic_to_shared(p); }
__device__ __forceinline__ uint64_t desc(uint32_t saddr, uint32_t lbo, uint32_t sbo) {
  return (uint64_t)((saddr & 0x3FFFF) >> 4) | ((uint64_t)(lbo >> 4) << 16) | ((uint64_t)(sbo >> 4) << 32);  // layout type 0 = no swizzle
}
// CUTLASS's warpgroup_fence_operand: keep accumulator accesses from being moved across wgmma boundaries
__device__ __forceinline__ void fence_acc(float* d) {
#pragma unroll
  for (int i = 0; i < 8; ++i) asm volatile("" : "+f"(d[i]) :: "memory");
}
__device__ __forceinline__ void wg_fence() { asm volatile("wgmma.fence.sync.aligned;" ::: "memory"); }
__device__ __forceinline__ void wg_commit() { asm volatile("wgmma.commit_group.sync.aligned;" ::: "memory"); }
template <int N> __device__ __forceinline__ void wg_wait() { asm volatile("wgmma.wait_group.sync.aligned %0;" :: "n"(N) : "memory"); }
__device__ __forceinline__ void wgmma_ss(float* d, uint64_t da, uint64_t db, int scale) {
  asm volatile("{\n.reg .pred p;\nsetp.ne.b32 p, %10, 0;\n"
               "wgmma.mma_async.sync.aligned.m64n16k16.f32.bf16.bf16 {%0,%1,%2,%3,%4,%5,%6,%7}, %8, %9, p, 1, 1, 0, 0;\n}"
               : "+f"(d[0]), "+f"(d[1]), "+f"(d[2]), "+f"(d[3]), "+f"(d[4]), "+f"(d[5]), "+f"(d[6]), "+f"(d[7])
               : "l"(da), "l"(db), "r"(scale));
}
__device__ __forceinline__ void wgmma_rs(float* d, const uint4& a, uint64_t db, int scale) {
  asm volatile("{\n.reg .pred p;\nsetp.ne.b32 p, %13, 0;\n"
               "wgmma.mma_async.sync.aligned.m64n16k16.f32.bf16.bf16 {%0,%1,%2,%3,%4,%5,%6,%7}, {%8,%9,%10,%11}, %12, p, 1, 1, 0;\n}"
               : "+f"(d[0]), "+f"(d[1]), "+f"(d[2]), "+f"(d[3]), "+f"(d[4]), "+f"(d[5]), "+f"(d[6]), "+f"(d[7])
               : "r"(a.x), "r"(a.y), "r"(a.z), "r"(a.w), "l"(db), "r"(scale));
}
__device__ __forceinline__ void mbar_init(uint32_t b, uint32_t c) { asm volatile("mbarrier.init.shared::cta.b64 [%0], %1;" :: "r"(b), "r"(c) : "memory"); }
__device__ __forceinline__ void mbar_expect(uint32_t b, uint32_t tx) { asm volatile("mbarrier.arrive.expect_tx.shared::cta.b64 _, [%0], %1;" :: "r"(b), "r"(tx) : "memory"); }
__device__ __forceinline__ void mbar_arrive(uint32_t b) { asm volatile("mbarrier.arrive.shared::cta.b64 _, [%0];" :: "r"(b) : "memory"); }
__device__ __forceinline__ void mbar_wait(uint32_t b, uint32_t par) {
  // test_wait + short nanosleep between polls (a tight try_wait loop cost ~30% of TMA rate in tpc_lab)
  uint32_t ok = 0;
  for (;;) {
    asm volatile("{\n.reg .pred p;\nmbarrier.test_wait.parity.shared::cta.b64 p, [%1], %2;\nselp.u32 %0, 1, 0, p;\n}" : "=r"(ok) : "r"(b), "r"(par) : "memory");
    if (ok) return;
    __nanosleep(32);
  }
}
__device__ __forceinline__ void bulk(uint32_t dst, const void* src, uint32_t bytes, uint32_t bar) {
  asm volatile("cp.async.bulk.shared::cluster.global.mbarrier::complete_tx::bytes [%0], [%1], %2, [%3];" :: "r"(dst), "l"(src), "r"(bytes), "r"(bar) : "memory");
}
__device__ __forceinline__ uint4 ldg16(const void* p) {
  uint4 v; asm volatile("ld.global.nc.L1::no_allocate.v4.u32 {%0,%1,%2,%3}, [%4];" : "=r"(v.x), "=r"(v.y), "=r"(v.z), "=r"(v.w) : "l"(p)); return v;
}
__device__ __forceinline__ uint4 lds16(uint32_t a) {
  uint4 v; asm volatile("ld.shared.v4.u32 {%0,%1,%2,%3}, [%4];" : "=r"(v.x), "=r"(v.y), "=r"(v.z), "=r"(v.w) : "r"(a)); return v;
}

enum Mode { SS = 1, RS_SMEM = 2, RS_GMEM = 3, TMA_ONLY = 4, SMEM_READ = 5, LD_ONLY = 6 };
struct P {
  const uint8_t* canon; const uint8_t* frag; float* out;
  uint64_t slice_blocks;  // row blocks per CTA slice
  uint64_t nb;            // row blocks per warpgroup to process
  int mode, nwg, S, stage_bytes;
  const unsigned char* allow; int* claim; int* ctr; int* wsm;
  unsigned long long *t0, *t1; unsigned* sink;
};
#define XBYTES (16 * 1024 * 2)
template <int MODE, int PF>
__global__ void __launch_bounds__(512, 1) k_swap(const __grid_constant__ P p) {
  extern __shared__ __align__(1024) uint8_t sm[];
  uint8_t* xs = sm; uint8_t* ring = sm + XBYTES;
  uint64_t* full = reinterpret_cast<uint64_t*>(ring + (size_t)p.S * p.stage_bytes);
  uint64_t* empty = full + 64;
  __shared__ int s_o;
  const int tid = threadIdx.x, lane = tid % 32;
  // warp-uniform role indices (as CUTLASS does), so wgmma sits on non-divergent paths
  const int wg = __shfl_sync(0xffffffffu, tid / 128, 0), wl = (tid / 32) % 4;
  const int nthr = p.nwg * 128;
  if (tid == 0) {
    const int smid = (int)gng_smid(); int o = -1;
    if (p.allow[smid] && atomicAdd(&p.claim[smid], 1) == 0) { o = atomicAdd(p.ctr, 1); p.wsm[o] = smid; }
    s_o = o;
    for (int i = 0; i < p.S; ++i) { mbar_init(su32(&full[i]), 1); mbar_init(su32(&empty[i]), 4); }
    asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
  }
  __syncthreads();
  const int o = s_o;
  if (o < 0) return;
  // X into shared memory, canonical layout
  for (int e = tid; e < 16 * 1024; e += blockDim.x) {
    const int n = e / 1024, k = e % 1024;
    *reinterpret_cast<__nv_bfloat16*>(xs + (k / 16) * 512 + canon_off(n, k % 16)) = __float2bfloat16(xv(n, k));
  }
  asm volatile("fence.proxy.async.shared::cta;" ::: "memory");
  __syncthreads();
  const unsigned long long t0 = gng_gt();
  const uint64_t sb = p.slice_blocks, base_blk = (uint64_t)o * sb;
  const int spb = BLK_BYTES / p.stage_bytes, kps = p.stage_bytes / 2048;  // stages per block, k-steps per stage
  constexpr bool ring_mode = MODE == SS || MODE == RS_SMEM || MODE == TMA_ONLY || MODE == SMEM_READ;
  unsigned sink = 0;
  if (wg == p.nwg) {  // producer warpgroup: warp 0 issues; warp 1+w waits on the full barriers for consumer warpgroup w
    // setmaxnreg (as in CUTLASS's producer warpgroup) marks this region warpgroup-uniform for ptxas;
    // without it ptxas serialised every wgmma of the consumers (C7518, WARPGROUP.DEPBAR.LE 0 after each HGMMA)
    asm volatile("setmaxnreg.dec.sync.aligned.u32 40;\n" ::: "memory");
    if constexpr (ring_mode) {
      if (wl == 0 && lane == 0) {
        // incremental counters: a 64-bit runtime % or / per stage cost ~600 ns and capped the ring at one stage in flight
        const uint64_t total = p.nb * spb * p.nwg;
        const uint8_t* srcbase = (MODE == SS ? p.canon : p.frag) + base_blk * BLK_BYTES;
        uint32_t slot = 0, ephase = 0, w = 0, jsp = 0;   // jsp = stage within block of warpgroup w
        uint32_t blkw[4] = {0, 1, 2, 3};                 // current block of each warpgroup (< sb)
        for (uint64_t j = 0; j < total; ++j) {
          if (j >= (uint64_t)p.S) mbar_wait(su32(&empty[slot]), ephase);
          const uint8_t* src = srcbase + (uint64_t)blkw[w] * BLK_BYTES + (uint64_t)jsp * p.stage_bytes;
          mbar_expect(su32(&full[slot]), p.stage_bytes);
          bulk(su32(ring + (size_t)slot * p.stage_bytes), src, p.stage_bytes, su32(&full[slot]));
          if (++slot == (uint32_t)p.S) { slot = 0; if (j >= (uint64_t)p.S) ephase ^= 1; }
          if (j + 1 == (uint64_t)p.S) ephase = 0;
          if (++w == (uint32_t)p.nwg) {
            w = 0;
            if (++jsp == (uint32_t)spb) { jsp = 0; for (int q = 0; q < p.nwg; ++q) { blkw[q] += p.nwg; if (blkw[q] >= sb) blkw[q] -= sb; } }
          }
        }
      } else if (wl >= 1 && wl <= p.nwg) {
        // notifier: the consumers never spin on an mbarrier (a data-dependent loop before a wgmma made
        // ptxas serialise every wgmma, C7518); they meet this warp at a named barrier instead
        const int w = wl - 1;
        const uint64_t stages = p.nb * spb;
        uint32_t j = w, slot = w, ph = 0;  // j = js * nwg + w, kept as slot / phase incrementally
        while (slot >= (uint32_t)p.S) { slot -= p.S; ph ^= 1; }
        for (uint64_t js = 0; js < stages; ++js) {
          mbar_wait(su32(&full[slot]), ph);
          asm volatile("bar.sync %0, 160;" :: "r"(1 + w) : "memory");
          slot += p.nwg; while (slot >= (uint32_t)p.S) { slot -= p.S; ph ^= 1; }
        }
        (void)j;
      }
    }
  } else {
    float acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const uint32_t xbase = su32(xs);
    auto write_out = [&](uint64_t blk) {
      float* out = p.out + (base_blk + blk) * 1024;  // 64 x 16 per block
      const int r = 16 * wl + lane / 4;
#pragma unroll
      for (int i = 0; i < 8; ++i) {
        const int col = (i / 4) * 8 + (lane % 4) * 2 + (i % 2), row = r + 8 * ((i / 2) % 2);
        out[row * 16 + col] = acc[i];
      }
    };
    if constexpr (ring_mode) {
      const uint64_t stages = p.nb * spb;
      int pend = -1;  // ring slot whose wgmmas may still be in flight (ss)
      uint32_t cslot = wg, sib = 0, cblk = wg;  // ring slot, stage within block, block (< sb): incremental, no 64-bit % or /
      while (cslot >= (uint32_t)p.S) cslot -= p.S;
      for (uint64_t js = 0; js < stages; ++js) {
        const int slot = (int)cslot;
        asm volatile("bar.sync %0, 160;" :: "r"(1 + wg) : "memory");  // stage js is full (see the notifier)
        const uint32_t st = su32(ring + (size_t)slot * p.stage_bytes);
        const int s0 = (int)sib * kps;  // first k-step of this stage within its block
        if constexpr (MODE == SS) {
          fence_acc(acc); wg_fence();
#pragma unroll
          for (int s = 0; s < 8; ++s)  // 16 KB stages: 8 k-steps, compile-time so that no wgmma sits on a branch
            wgmma_ss(acc, desc(st + s * 2048, 128, 256), desc(xbase + (s0 + s) * 512, 128, 256), (s0 + s) != 0);
          wg_commit();
          wg_wait<1>(); fence_acc(acc);
          if (pend >= 0) { __syncwarp(); if (lane == 0) mbar_arrive(su32(&empty[pend])); }
          pend = slot;
          if (sib == (uint32_t)spb - 1) {
            wg_wait<0>(); fence_acc(acc);
            __syncwarp(); if (lane == 0) mbar_arrive(su32(&empty[pend])); pend = -1;
            write_out(cblk);
          }
        } else if constexpr (MODE == RS_SMEM) {
          // two named register buffers, k-steps in pairs; 16 KB stages (8 k-steps, compile-time, fully unrolled)
          constexpr int KPS = 8;
          uint4 a0 = lds16(st + wl * 512 + lane * 16), a1;
#pragma unroll
          for (int s = 0; s < KPS; s += 2) {
            fence_acc(acc); wg_fence();
            wgmma_rs(acc, a0, desc(xbase + (s0 + s) * 512, 128, 256), (s0 + s) != 0);
            wg_commit();
            wg_wait<1>(); fence_acc(acc);
            a1 = lds16(st + (s + 1) * 2048 + wl * 512 + lane * 16);
            fence_acc(acc); wg_fence();
            wgmma_rs(acc, a1, desc(xbase + (s0 + s + 1) * 512, 128, 256), 1);
            wg_commit();
            wg_wait<1>(); fence_acc(acc);
            if (s + 2 < KPS) a0 = lds16(st + (s + 2) * 2048 + wl * 512 + lane * 16);
          }
          __syncwarp(); if (lane == 0) mbar_arrive(su32(&empty[slot]));
          if (sib == (uint32_t)spb - 1) { wg_wait<0>(); fence_acc(acc); write_out(cblk); }
        } else if constexpr (MODE == SMEM_READ) {
          for (int s = 0; s < kps; ++s) { const uint4 v = lds16(st + s * 2048 + wl * 512 + lane * 16); sink ^= v.x ^ v.y ^ v.z ^ v.w; }
          __syncwarp(); if (lane == 0) mbar_arrive(su32(&empty[slot]));
        } else {  // TMA_ONLY
          __syncwarp(); if (lane == 0) mbar_arrive(su32(&empty[slot]));
        }
        cslot += p.nwg; while (cslot >= (uint32_t)p.S) cslot -= p.S;
        if (++sib == (uint32_t)spb) { sib = 0; cblk += p.nwg; if (cblk >= sb) cblk -= sb; }
      }
    } else {  // RS_GMEM / LD_ONLY: k-step t of this warpgroup = block t / 64, k-step t % 64
      // load cursor kept incrementally (a 64-bit runtime % per load made these loops instruction-bound)
      const uint8_t* fb = p.frag + base_blk * BLK_BYTES + wl * 512 + lane * 16;
      uint32_t lblk = wg, ls = 0, cblk = wg;
      auto next_addr = [&]() {
        const uint8_t* q = fb + (uint64_t)lblk * BLK_BYTES + ls * 2048;
        if (++ls == KSTEPS) { ls = 0; lblk += p.nwg; if (lblk >= sb) lblk -= sb; }
        return q;
      };
      const uint64_t T = p.nb * KSTEPS;
      uint4 a[PF];
#pragma unroll
      for (int j = 0; j < PF; ++j) a[j] = ldg16(next_addr());
      for (uint64_t t0b = 0; t0b < T; t0b += PF) {
#pragma unroll
        for (int j = 0; j < PF; ++j) {
          const uint64_t t = t0b + j; const int s = (int)(t & (KSTEPS - 1));
          if constexpr (MODE == RS_GMEM) {
            fence_acc(acc); wg_fence();
            wgmma_rs(acc, a[j], desc(xbase + s * 512, 128, 256), s != 0);
            wg_commit();
            wg_wait<1>(); fence_acc(acc);
            // wgmma t-1 is done: its registers take k-step t-1+PF
            const int jp = (j + PF - 1) % PF;
            if (t > 0 && t - 1 + PF < T) a[jp] = ldg16(next_addr());
            if (s == KSTEPS - 1) { wg_wait<0>(); fence_acc(acc); write_out(cblk); cblk += p.nwg; if (cblk >= sb) cblk -= sb; }
          } else {
            sink ^= a[j].x ^ a[j].y ^ a[j].z ^ a[j].w;
            if (t + PF < T) a[j] = ldg16(next_addr());
          }
        }
      }
      if constexpr (MODE == RS_GMEM) {
        // the last block's tail (t - 1 + PF >= T loads were skipped; the final wgmma is waited above)
        wg_wait<0>(); fence_acc(acc);
      }
    }
  }
  __syncthreads();
  if (tid == 0) { p.t0[o] = t0; p.t1[o] = gng_gt(); }
  if (sink == 0x9e3779b9u) p.sink[o] = sink;
}

// ------------------------------------------------------------------ host
static Nvml g_nv;
int main(int argc, char** argv) {
  std::string map, sms = "0", mode = "rs_gmem";
  int nwg = 2, S = 8, stage_kb = 16, pf = 8, trials = 5, M = 16;
  double secs = 1.5, slice_mb = 256;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--map")) map = argv[++i]; else if (is("--sms")) sms = argv[++i]; else if (is("--mode")) mode = argv[++i];
    else if (is("--nwg")) nwg = atoi(argv[++i]); else if (is("--stages")) S = atoi(argv[++i]); else if (is("--stage-kb")) stage_kb = atoi(argv[++i]);
    else if (is("--pf")) pf = atoi(argv[++i]); else if (is("--trials")) trials = atoi(argv[++i]); else if (is("--secs")) secs = atof(argv[++i]);
    else if (is("--slice-mb")) slice_mb = atof(argv[++i]); else if (is("--M")) M = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  load_map(map);
  cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, 0));
  char host[128]; gethostname(host, sizeof host);
  std::vector<int> smv = parse_list(sms);
  const u128 mask = mask_of_sms(smv);
  calibrate_gt();
  g_nv.start(10);
  if (mode == "cublas") {
    // padded-M baseline: D[M x N] = X[M x K] W^T, W N x K row-major
    const int K = 4096, N = 28672;
    __nv_bfloat16 *W, *X, *D; void* ws; size_t wsz = 64 << 20;
    CK(cudaMalloc(&W, (size_t)N * K * 2)); CK(cudaMalloc(&X, (size_t)M * K * 2)); CK(cudaMalloc(&D, (size_t)M * N * 2)); CK(cudaMalloc(&ws, wsz));
    std::vector<__nv_bfloat16> hx((size_t)M * K); for (int n = 0; n < M; ++n) for (int k = 0; k < K; ++k) hx[(size_t)n * K + k] = __float2bfloat16(xv(n, k));
    CK(cudaMemcpy(X, hx.data(), hx.size() * 2, cudaMemcpyHostToDevice));
    { std::vector<__nv_bfloat16> hw((size_t)N * K); for (size_t i = 0; i < hw.size(); ++i) hw[i] = __float2bfloat16(wv(i / K / 64, (int)(i / K % 64), (int)(i % K % 1024)));
      CK(cudaMemcpy(W, hw.data(), hw.size() * 2, cudaMemcpyHostToDevice)); }
    cublasLtHandle_t lt; cublasLtCreate(&lt);
    cublasLtMatmulDesc_t op; cublasLtMatmulDescCreate(&op, CUBLAS_COMPUTE_32F, CUDA_R_32F);
    cublasOperation_t ta = CUBLAS_OP_T, tb = CUBLAS_OP_N;
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSA, &ta, sizeof ta); cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSB, &tb, sizeof tb);
    int smt = (int)smv.size(); cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET, &smt, sizeof smt);
    cublasLtMatrixLayout_t la, lb, lc;
    cublasLtMatrixLayoutCreate(&la, CUDA_R_16BF, K, N, K); cublasLtMatrixLayoutCreate(&lb, CUDA_R_16BF, K, M, K); cublasLtMatrixLayoutCreate(&lc, CUDA_R_16BF, N, M, N);
    cublasLtMatmulPreference_t pref; cublasLtMatmulPreferenceCreate(&pref);
    cublasLtMatmulPreferenceSetAttribute(pref, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &wsz, sizeof wsz);
    cublasLtMatmulHeuristicResult_t r[1]; int nr = 0;
    if (cublasLtMatmulAlgoGetHeuristic(lt, op, la, lb, lc, lc, pref, 1, r, &nr) || !nr) { fprintf(stderr, "no algo\n"); return 1; }
    cudaStream_t st; CK(cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking));
    const float al = 1, be = 0;
    gng_mask(mask);
    auto one = [&] { if (cublasLtMatmul(lt, op, &al, W, la, X, lb, &be, D, lc, D, lc, &r[0].algo, ws, wsz, st)) { fprintf(stderr, "matmul failed\n"); exit(1); } };
    for (int i = 0; i < 5; ++i) one();
    CK(cudaStreamSynchronize(st));
    cudaEvent_t e0, e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0, st); for (int i = 0; i < 20; ++i) one(); cudaEventRecord(e1, st); CK(cudaEventSynchronize(e1));
    float ms; cudaEventElapsedTime(&ms, e0, e1); const int iters = std::max(20, (int)(secs * 1000 / (ms / 20)));
    std::vector<double> gbs;
    for (int tr = 0; tr < trials; ++tr) {
      const int64_t h0 = host_ns();
      cudaEventRecord(e0, st); for (int i = 0; i < iters; ++i) one(); cudaEventRecord(e1, st); CK(cudaEventSynchronize(e1));
      const int64_t h1 = host_ns();
      cudaEventElapsedTime(&ms, e0, e1);
      const double g = (double)N * K * 2 * iters / (ms * 1e6) / smv.size();
      gbs.push_back(g);
      printf("host=%s mode=cublas M=%d K=%d N=%d sms=%s n_sms=%zu trial=%d iters=%d ms_per=%.4f gbs_per_sm=%.1f tflops_useful=%.3f %s\n", host, M, K, N, sms.c_str(), smv.size(), tr, iters,
             ms / iters, g, 2.0 * M * N * K * iters / (ms * 1e9), g_nv.summary(h0, h1).c_str());
    }
    gng_mask(0);
    std::vector<__nv_bfloat16> hd((size_t)M * N); CK(cudaMemcpy(hd.data(), D, hd.size() * 2, cudaMemcpyDeviceToHost));
    int bad = 0;
    for (int c = 0; c < 64; ++c) {
      const int i = mix32(c) % M, j = mix32(c + 99) % N; double ref = 0;
      for (int k = 0; k < K; ++k) ref += (double)__bfloat162float(__float2bfloat16(wv((uint64_t)j / 64 * 1, j % 64, k % 1024))) * xv(i, k);
      if (fabs(__bfloat162float(hd[(size_t)i * N + j]) - ref) > 0.02 * (1 + fabs(ref))) ++bad;
    }
    std::sort(gbs.begin(), gbs.end());
    printf("summary mode=cublas M=%d sms=%s n_sms=%zu gbs_per_sm_med=%.1f min=%.1f max=%.1f check=%s\n", M, sms.c_str(), smv.size(), gbs[gbs.size() / 2], gbs.front(), gbs.back(), bad ? "FAIL" : "ok");
    g_nv.finish();
    return 0;
  }
  int md = mode == "ss" ? SS : mode == "rs_smem" ? RS_SMEM : mode == "rs_gmem" ? RS_GMEM : mode == "tma_only" ? TMA_ONLY : mode == "smem_read" ? SMEM_READ : mode == "ld_only" ? LD_ONLY : 0;
  if (!md) { fprintf(stderr, "bad mode\n"); return 1; }
  if ((md == RS_SMEM || md == SS) && stage_kb != 16) { fprintf(stderr, "ss and rs_smem need --stage-kb 16\n"); return 1; }
  const int nsl = (int)smv.size();
  const uint64_t sb = (uint64_t)(slice_mb * (1 << 20)) / BLK_BYTES;
  const uint64_t nblocks = sb * nsl;
  uint8_t *canon, *frag; float* out;
  CK(cudaMalloc(&canon, nblocks * BLK_BYTES)); CK(cudaMalloc(&frag, nblocks * BLK_BYTES)); CK(cudaMalloc(&out, nblocks * 1024 * 4));
  k_gen<<<prop.multiProcessorCount * 4, 1024>>>((__nv_bfloat16*)canon, (__nv_bfloat16*)frag, nblocks);
  CK(cudaDeviceSynchronize());
  unsigned char* allow; int *claim, *ctr, *wsm; unsigned long long *t0, *t1; unsigned* sink;
  CK(cudaMalloc(&allow, GNG_MAXSM)); CK(cudaMalloc(&claim, GNG_MAXSM * 4)); CK(cudaMalloc(&ctr, 4)); CK(cudaMalloc(&wsm, GNG_MAXSM * 4));
  CK(cudaMalloc(&t0, GNG_MAXSM * 8)); CK(cudaMalloc(&t1, GNG_MAXSM * 8)); CK(cudaMalloc(&sink, GNG_MAXSM * 4));
  std::vector<unsigned char> al(GNG_MAXSM, 0); for (int x : smv) al[x] = 1;
  CK(cudaMemcpy(allow, al.data(), GNG_MAXSM, cudaMemcpyHostToDevice));
  const bool ring = md == SS || md == RS_SMEM || md == TMA_ONLY || md == SMEM_READ;
  const size_t smem = XBYTES + (ring ? (size_t)S * stage_kb * 1024 : 0) + 64 * 16 + 64;
  void (*kern)(P) = nullptr;
#define PICK(M) if (md == M) kern = pf == 4 ? k_swap<M, 4> : pf == 16 ? k_swap<M, 16> : k_swap<M, 8>;
  PICK(SS) PICK(RS_SMEM) PICK(RS_GMEM) PICK(TMA_ONLY) PICK(SMEM_READ) PICK(LD_ONLY)
  CK(cudaFuncSetAttribute(kern, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  cudaFuncAttributes fa; CK(cudaFuncGetAttributes(&fa, kern));
  P p{canon, frag, out, sb, 1, md, nwg, ring ? S : 0, stage_kb * 1024, allow, claim, ctr, wsm, t0, t1, sink};
  const int grid = 4 * u128_popc(mask), threads = (nwg + 1) * 128;
  auto run = [&](uint64_t nb, std::vector<double>& per, int& nw, int64_t& h0, int64_t& h1) {
    p.nb = nb;
    CK(cudaMemset(claim, 0, GNG_MAXSM * 4)); CK(cudaMemset(ctr, 0, 4));
    gng_mask(mask);
    h0 = host_ns();
    kern<<<grid, threads, smem>>>(p);
    CK(cudaGetLastError()); CK(cudaDeviceSynchronize());
    h1 = host_ns();
    gng_mask(0);
    CK(cudaMemcpy(&nw, ctr, 4, cudaMemcpyDeviceToHost));
    std::vector<unsigned long long> a(nw), b(nw); CK(cudaMemcpy(a.data(), t0, nw * 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(b.data(), t1, nw * 8, cudaMemcpyDeviceToHost));
    per.clear();
    for (int i = 0; i < nw; ++i) per.push_back((double)nb * nwg * BLK_BYTES / (double)(b[i] - a[i]));
  };
  std::vector<double> per; int nw; int64_t h0, h1;
  run(std::max<uint64_t>(1, 4096 / nwg), per, nw, h0, h1);  // calibration: 512 MB per CTA
  const double est = median_of(per);
  const uint64_t nb = std::max<uint64_t>(16, (uint64_t)(est * secs * 1e9 / ((double)nwg * BLK_BYTES)));
  std::vector<double> meds;
  for (int tr = 0; tr < trials; ++tr) {
    run(nb, per, nw, h0, h1);
    const double m = median_of(per);
    meds.push_back(m);
    printf("host=%s mode=%s sms=%s n_sms=%zu workers=%d nwg=%d stages=%d stage_kb=%d pf=%d trial=%d secs=%.2f gbs_per_sm_med=%.1f gbs_per_sm_min=%.1f gbs_per_sm_max=%.1f "
           "tflops_per_sm=%.3f regs=%d smem=%zu %s\n",
           host, mode.c_str(), sms.c_str(), smv.size(), nw, nwg, ring ? S : 0, ring ? stage_kb : 0, pf, tr, (h1 - h0) / 1e9, m,
           *std::min_element(per.begin(), per.end()), *std::max_element(per.begin(), per.end()),
           (md == SS || md == RS_SMEM || md == RS_GMEM) ? m / 2048 * 32768 / 1e3 : 0.0, fa.numRegs, smem, g_nv.summary(h0, h1).c_str());
    fflush(stdout);
  }
  // correctness: 3 blocks of each worker that were written (every block of the slice when nb >= slice)
  int bad = 0, checked = 0;
  if (md == SS || md == RS_SMEM || md == RS_GMEM) {
    std::vector<int> ws(nw); CK(cudaMemcpy(ws.data(), wsm, nw * 4, cudaMemcpyDeviceToHost));
    const uint64_t written = std::min<uint64_t>(sb, nb * nwg);
    for (int o = 0; o < nw; ++o) for (int c = 0; c < 3; ++c) {
      const uint64_t blk = mix32(o * 31 + c) % written, gb = o * sb + blk;
      std::vector<float> got(1024); CK(cudaMemcpy(got.data(), out + gb * 1024, 4096, cudaMemcpyDeviceToHost));
      for (int r = 0; r < 64; ++r) for (int n = 0; n < 16; ++n) {
        double ref = 0; for (int k = 0; k < 1024; ++k) ref += (double)wv(gb, r, k) * xv(n, k);
        if (fabs(ref - got[r * 16 + n]) > 1e-3 * (1 + fabs(ref))) ++bad;
      }
      ++checked;
    }
  }
  std::sort(meds.begin(), meds.end());
  printf("summary mode=%s sms=%s n_sms=%zu nwg=%d stages=%d stage_kb=%d pf=%d gbs_per_sm_med=%.1f min=%.1f max=%.1f check=%s(%d blocks, %d bad elems)\n", mode.c_str(), sms.c_str(), smv.size(), nwg,
         ring ? S : 0, ring ? stage_kb : 0, pf, meds[meds.size() / 2], meds.front(), meds.back(),
         checked == 0 ? "n/a" : (bad ? "FAIL" : "ok"), checked, bad);
  g_nv.finish();
  return 0;
}
