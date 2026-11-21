// TPC-mask placement lab for Hopper (H200): per-launch TPC masks
// (tpc_smctrl.h, the libsmctrl callback mechanism) plus an SM filter (each
// CTA reads %smid and only CTAs on chosen SMs work), so that a kernel can be
// placed on chosen SMs. Subcommands:
//
//   tpc_lab probe [--spin-us S] [--out FILE]
//       For every mask bit b in 0..127: launch with only bit b disabled and
//       record which SMs are missing (= the SMs of TPC bit b); then launch
//       with only bit b enabled and check that every CTA ran on those SMs.
//       Writes "bit <b> sms <s...>" lines to FILE.
//   tpc_lab cluster --cluster C (--tpcs LIST | --sms LIST --map FILE)
//                   --nclusters N [--timeout-s T]
//       Cluster launch (one CTA per SM: maximum dynamic shared memory),
//       prints the SMs of every cluster. A launch that cannot be placed is
//       reported as TIMEOUT (process exits 124).
//   tpc_lab run --map FILE [MEM options] [CMP options] [--reps R]
//     MEM (memory-bound job), one of
//       --mem ld  --mem-sms LIST [--per-sm P] [--mb-per-sm X]
//           plain-load stream: P CTAs of 1024 threads per SM, uint4 loads,
//           each CTA sums its own slice (checksum verified)
//       --mem tma --roles "c[:h,h..];c[:h..];..." [--cluster C]
//                 [--self 0|1] [--consume 0|1] [--depth D] [--nw W]
//                 [--mb-per-consumer X] [--extra-sms LIST]
//           cluster TMA stream (borrow_tma.cuh design, roles by %smid):
//           consumer SM c owns a ring of 16 KB slots; its own warp 0
//           (if --self 1) and every helper SM h listed after it issue
//           cp.async.bulk.shared::cluster into c's ring. The mask is the
//           TPCs of all listed SMs (+ --extra-sms); CTAs on unlisted SMs
//           of those TPCs are idle. A helper whose consumer is not in its
//           cluster idles (reported as fewer issuers).
//     CMP (compute-bound job, optional)
//       --cmp-sms LIST [--cmp-per-sm P]
//           FP32 FMA loop (corun.cu muladd: 4 chains, 1024 threads per CTA)
//           on those SMs until the host stops it; per-chunk timestamps give
//           its rate inside the MEM window; a CMP-alone pass of the same
//           length gives the solo rate.
//
// Buffer: --gib G (default 24) of uint32 a[i] = i. Timing: %globaltimer.
#include "tpc_smctrl.h"
#include <cuda_runtime.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

#define MAXSM 256

// ---------------------------------------------------------------- device helpers
__device__ __forceinline__ uint32_t sm_id() { uint32_t r; asm volatile("mov.u32 %0, %%smid;" : "=r"(r)); return r; }
__device__ __forceinline__ unsigned long long gtimer() {
  unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t;
}
__device__ __forceinline__ uint32_t smem_u32(const void* p) { return (uint32_t)__cvta_generic_to_shared(p); }
__device__ __forceinline__ uint32_t cluster_rank() { uint32_t r; asm volatile("mov.u32 %0, %%cluster_ctarank;" : "=r"(r)); return r; }
__device__ __forceinline__ uint32_t cluster_size() { uint32_t r; asm volatile("mov.u32 %0, %%cluster_nctarank;" : "=r"(r)); return r; }
__device__ __forceinline__ void cluster_sync_all() {
  __syncwarp();
  asm volatile("barrier.cluster.arrive.release.aligned;\n\tbarrier.cluster.wait.acquire.aligned;" ::: "memory");
}
__device__ __forceinline__ uint32_t mapa(uint32_t a, uint32_t rank) {
  uint32_t r; asm volatile("mapa.shared::cluster.u32 %0, %1, %2;" : "=r"(r) : "r"(a), "r"(rank)); return r;
}
__device__ __forceinline__ uint32_t ld_cluster_u32(uint32_t ca) {
  uint32_t v; asm volatile("ld.shared::cluster.u32 %0, [%1];" : "=r"(v) : "r"(ca) : "memory"); return v;
}
__device__ __forceinline__ void mbar_init(uint32_t bar, uint32_t count) {
  asm volatile("mbarrier.init.shared::cta.b64 [%0], %1;" :: "r"(bar), "r"(count) : "memory");
}
__device__ __forceinline__ void fence_mbar_init() { asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory"); }
__device__ __forceinline__ void mbar_arrive_expect_tx(uint32_t bar, uint32_t tx) {
  asm volatile("mbarrier.arrive.expect_tx.shared::cta.b64 _, [%0], %1;" :: "r"(bar), "r"(tx) : "memory");
}
__device__ __forceinline__ void mbar_arrive_local(uint32_t bar) {
  asm volatile("mbarrier.arrive.shared::cta.b64 _, [%0];" :: "r"(bar) : "memory");
}
__device__ __forceinline__ void mbar_arrive_cluster(uint32_t cbar) {
  asm volatile("mbarrier.arrive.shared::cluster.b64 _, [%0];" :: "r"(cbar) : "memory");
}
__device__ __forceinline__ void mbar_wait(uint32_t bar, uint32_t parity, int spin = 0) {
  uint32_t ok = 0;
  if (spin > 1) {  // test_wait with __nanosleep(spin) between polls
    for (;;) {
      asm volatile("{\n\t.reg .pred p;\n\tmbarrier.test_wait.parity.shared::cta.b64 p, [%1], %2;\n\t"
                   "selp.u32 %0, 1, 0, p;\n\t}" : "=r"(ok) : "r"(bar), "r"(parity) : "memory");
      if (ok) return;
      __nanosleep(spin);
    }
  }
  if (spin)
    while (!ok)
      asm volatile("{\n\t.reg .pred p;\n\tmbarrier.test_wait.parity.shared::cta.b64 p, [%1], %2;\n\t"
                   "selp.u32 %0, 1, 0, p;\n\t}" : "=r"(ok) : "r"(bar), "r"(parity) : "memory");
  else
    while (!ok)
      asm volatile("{\n\t.reg .pred p;\n\tmbarrier.try_wait.parity.shared::cta.b64 p, [%1], %2;\n\t"
                   "selp.u32 %0, 1, 0, p;\n\t}" : "=r"(ok) : "r"(bar), "r"(parity) : "memory");
}
__device__ __forceinline__ void bulk_g2s(uint32_t cdst, const void* gsrc, uint32_t bytes, uint32_t cbar) {
  asm volatile("cp.async.bulk.shared::cluster.global.mbarrier::complete_tx::bytes [%0], [%1], %2, [%3];"
               :: "r"(cdst), "l"(gsrc), "r"(bytes), "r"(cbar) : "memory");
}
__device__ __forceinline__ void named_bar(int id, int threads) { asm volatile("bar.sync %0, %1;" :: "r"(id), "r"(threads) : "memory"); }

__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) a[i] = (uint32_t)i;
}

// ---------------------------------------------------------------- probe kernels
__global__ void k_smid(int* out, long long spin_ns) {
  if (threadIdx.x == 0) {
    out[blockIdx.x] = (int)sm_id();
    const unsigned long long t0 = gtimer();
    while ((long long)(gtimer() - t0) < spin_ns) {}
  }
  __syncthreads();
}
__global__ void k_cl_smid(int* out, long long spin_ns) {
  if (threadIdx.x == 0) {
    out[blockIdx.x] = (int)sm_id();
    const unsigned long long t0 = gtimer();
    while ((long long)(gtimer() - t0) < spin_ns) {}
  }
  cluster_sync_all();
}

// ---------------------------------------------------------------- CMP: FMA loop
struct CmpParams {
  const unsigned char* allow;  // [MAXSM]
  int per_sm;
  int* claim;                  // [MAXSM]
  int* slot_ctr;
  int* slot_sm;                // [maxslots]
  unsigned long long* ts;      // [maxslots * maxch]: start at [0], chunk ends at [1..]
  int* nch;                    // [maxslots]
  int maxch, fixed, ilp;
  unsigned iters;
  volatile int* stop;          // mapped host memory
  float* out;
};
__global__ void __launch_bounds__(1024) k_fma(CmpParams p) {
  __shared__ int s_slot, s_stop;
  if (threadIdx.x == 0) {
    const int sm = (int)sm_id();
    int slot = -1;
    if (p.allow[sm] && atomicAdd(&p.claim[sm], 1) < p.per_sm) { slot = atomicAdd(p.slot_ctr, 1); p.slot_sm[slot] = sm; }
    s_slot = slot; s_stop = 0;
  }
  __syncthreads();
  const int slot = s_slot;
  if (slot < 0) return;
  unsigned long long* ts = p.ts + (size_t)slot * p.maxch;
  if (threadIdx.x == 0) ts[0] = gtimer();
  float a0 = threadIdx.x, a1 = a0 + 1, a2 = a0 + 2, a3 = a0 + 3;
  const float mul = 0.999999f, add = 1e-7f;
  int c = 1;
  for (; c < p.maxch; ++c) {
    if (p.ilp == 4) {
      for (unsigned i = 0; i < p.iters; i += 4) {
        a0 = a0 * mul + add; a1 = a1 * mul + add; a2 = a2 * mul + add; a3 = a3 * mul + add;
      }
    } else if (p.ilp == 2) {  // same FMA count, 2 independent chains
      for (unsigned i = 0; i < p.iters; i += 4) {
        a0 = a0 * mul + add; a1 = a1 * mul + add; a0 = a0 * mul + add; a1 = a1 * mul + add;
      }
    } else {                  // 1 chain: every FMA waits for the previous one
      for (unsigned i = 0; i < p.iters; i += 4) {
        a0 = a0 * mul + add; a0 = a0 * mul + add; a0 = a0 * mul + add; a0 = a0 * mul + add;
      }
    }
    __syncthreads();
    if (threadIdx.x == 0) { ts[c] = gtimer(); s_stop = p.fixed ? (c >= p.fixed) : *p.stop; }
    __syncthreads();
    if (s_stop) { ++c; break; }
  }
  if (threadIdx.x == 0) p.nch[slot] = c;  // entries 0..c-1 valid
  if (a0 + a1 + a2 + a3 == 1234.5f) p.out[slot] = a0;
}

// ---------------------------------------------------------------- MEM: plain-load stream
struct LdParams {
  const uint4* src;
  const unsigned char* allow;
  int per_sm;
  int* claim;
  int* ctr;
  size_t slice_vec;              // uint4 per slice
  unsigned* sums;                // per slice
  unsigned long long* times;     // per slice t0, t1
  int* slice_sm;
};
__global__ void __launch_bounds__(1024, 2) k_ld(LdParams p) {
  __shared__ int s_o;
  if (threadIdx.x == 0) {
    const int sm = (int)sm_id();
    int o = -1;
    if (p.allow[sm] && atomicAdd(&p.claim[sm], 1) < p.per_sm) { o = atomicAdd(p.ctr, 1); p.slice_sm[o] = sm; }
    s_o = o;
  }
  __syncthreads();
  const int o = s_o;
  if (o < 0) return;
  const unsigned long long t0 = gtimer();
  const uint4* a = p.src + (size_t)o * p.slice_vec + threadIdx.x;
  const size_t n = p.slice_vec / 1024;
  uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const uint4 v0 = a[(i + 0) * 1024], v1 = a[(i + 1) * 1024], v2 = a[(i + 2) * 1024], v3 = a[(i + 3) * 1024];
    s0 += v0.x + v0.y + v0.z + v0.w; s1 += v1.x + v1.y + v1.z + v1.w;
    s2 += v2.x + v2.y + v2.z + v2.w; s3 += v3.x + v3.y + v3.z + v3.w;
  }
  for (; i < n; ++i) { const uint4 v = a[i * 1024]; s0 += v.x + v.y + v.z + v.w; }
  uint32_t s = s0 + s1 + s2 + s3;
  for (int k = 16; k > 0; k >>= 1) s += __shfl_xor_sync(0xffffffffu, s, k);
  if ((threadIdx.x & 31) == 0) atomicAdd(&p.sums[o], s);
  __syncthreads();
  if (threadIdx.x == 0) { p.times[2 * o] = t0; p.times[2 * o + 1] = gtimer(); }
}

__global__ void __launch_bounds__(1024, 2) k_ld32(LdParams p) {
  __shared__ int s_o;
  if (threadIdx.x == 0) {
    const int sm = (int)sm_id();
    int o = -1;
    if (p.allow[sm] && atomicAdd(&p.claim[sm], 1) < p.per_sm) { o = atomicAdd(p.ctr, 1); p.slice_sm[o] = sm; }
    s_o = o;
  }
  __syncthreads();
  const int o = s_o;
  if (o < 0) return;
  const unsigned long long t0 = gtimer();
  const uint32_t* a = reinterpret_cast<const uint32_t*>(p.src) + (size_t)o * p.slice_vec * 4 + threadIdx.x;
  const size_t n = p.slice_vec * 4 / 1024;
  uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  for (size_t i = 0; i + 4 <= n; i += 4) {
    s0 += a[(i + 0) * 1024]; s1 += a[(i + 1) * 1024]; s2 += a[(i + 2) * 1024]; s3 += a[(i + 3) * 1024];
  }
  uint32_t s = s0 + s1 + s2 + s3;
  for (int k = 16; k > 0; k >>= 1) s += __shfl_xor_sync(0xffffffffu, s, k);
  if ((threadIdx.x & 31) == 0) atomicAdd(&p.sums[o], s);
  __syncthreads();
  if (threadIdx.x == 0) { p.times[2 * o] = t0; p.times[2 * o + 1] = gtimer(); }
}

// ---------------------------------------------------------------- MEM: role-based cluster TMA stream
struct RoleParams {
  const uint32_t* src;
  uint64_t window_tiles, cons_stride;
  uint32_t T, tile_bytes;
  int D, self_issue, consume, nw, nlocal, spin, fused;
  const short* role;   // [MAXSM]: >=0 consumer ordinal, -2 helper, -1 idle
  const short* owner;  // [MAXSM]: helper -> consumer SM
  unsigned* sums; unsigned* errs; unsigned long long* times; int* hcount; unsigned* tdone;
  int* cta_sm;         // per CTA
};
#define RB_BAR 2048  // [0] smid, [16..] info; full[64] at 512, empty[64] at 1024
#define RB_MAXH 16
__global__ void k_role(RoleParams p) {
  extern __shared__ __align__(1024) unsigned char smem[];
  int* info = reinterpret_cast<int*>(smem + 16);  // role, crank, issuer, H, cons, hr[RB_MAXH]
  const uint32_t rank = cluster_rank(), C = cluster_size();
  const int warp = threadIdx.x / 32, lane = threadIdx.x % 32;
  const uint32_t base0 = smem_u32(smem), full0 = base0 + 512, empty0 = base0 + 1024, slot0 = base0 + RB_BAR;
  const int my = (int)sm_id();
  if (threadIdx.x == 0) { *reinterpret_cast<uint32_t*>(smem) = (uint32_t)my; p.cta_sm[blockIdx.x] = my; }
  cluster_sync_all();
  if (threadIdx.x == 0) {
    int sm_of[16];
    for (uint32_t r = 0; r < C && r < 16; ++r) sm_of[r] = (int)ld_cluster_u32(mapa(base0, r));
    int role = p.role[my], crank = -1, issuer = -1, H = 0, cons = -1, csm = -1;
    if (role >= 0) { csm = my; crank = (int)rank; cons = role; }
    else if (role == -2) {
      csm = p.owner[my];
      for (uint32_t r = 0; r < C && r < 16; ++r) if (sm_of[r] == csm) crank = (int)r;
      if (crank < 0) role = -1; else cons = p.role[csm];
    }
    int nh = 0;
    if (role != -1) {
      H = p.self_issue;
      for (uint32_t r = 0; r < C && r < 16; ++r)
        if (p.role[sm_of[r]] == -2 && p.owner[sm_of[r]] == csm && nh < RB_MAXH) {
          if ((int)r == (int)rank) issuer = p.self_issue + nh;
          info[5 + nh] = (int)r; ++nh;
        }
      H += nh;
      if (role >= 0 && p.self_issue) issuer = 0;
      if (role >= 0) issuer = p.self_issue ? 0 : -1;
      if (p.nlocal) { if (role >= 0) H = p.nlocal; else role = -1; }
      if (H * p.D > 64 || H == 0) role = -1;
    }
    info[0] = role; info[1] = crank; info[2] = issuer; info[3] = H; info[4] = cons;
    if (role >= 0) {
      for (int s = 0; s < H * p.D; ++s) { mbar_init(full0 + 8 * s, 1); mbar_arrive_expect_tx(full0 + 8 * s, p.tile_bytes); }
      if (p.nlocal) for (int j = 0; j < H * p.D; ++j) mbar_init(empty0 + 8 * j, 1);
      else if (p.self_issue) for (int j = 0; j < p.D; ++j) mbar_init(empty0 + 8 * j, 1);
      p.hcount[cons] = H;
    } else if (role == -2) {
      for (int j = 0; j < p.D; ++j) mbar_init(empty0 + 8 * j, 1);
    }
    fence_mbar_init();
  }
  __syncthreads();
  const int role = info[0], crank = info[1], H = info[3], cons = info[4];
  int issuer = info[2];
  uint32_t ebase = empty0;
  bool issuing = issuer >= 0 && ((role >= 0 && warp == 0 && lane == 0) || (role == -2 && threadIdx.x == 0));
  if (p.nlocal) {
    issuing = role >= 0 && warp > p.nw && warp <= p.nw + p.nlocal && lane == 0;
    issuer = warp - p.nw - 1;
    ebase = empty0 + 8 * issuer * p.D;
  }
  cluster_sync_all();
  if (p.fused && role >= 0) {
    // tmacopy.cu structure: one thread issues D transfers, waits for the
    // oldest, checks its first and last word, re-issues the slot
    if (threadIdx.x == 0) {
      const uint32_t T = p.T, te = p.tile_bytes / 4, D = p.D;
      uint64_t gi = ((uint64_t)cons * p.cons_stride) % p.window_tiles, gc = gi;
      const unsigned long long t0 = gtimer();
      uint32_t acc = 0, err = 0;
      for (uint32_t n = 0; n < D && n < T; ++n) {
        bulk_g2s(slot0 + n * p.tile_bytes, p.src + gi * te, p.tile_bytes, full0 + 8 * n);
        if (++gi >= p.window_tiles) gi = 0;
      }
      uint32_t sl = 0, ph = 0;
      for (uint32_t n = 0; n < T; ++n) {
        mbar_wait(full0 + 8 * sl, ph, p.spin);
        const uint32_t* t = reinterpret_cast<const uint32_t*>(smem + RB_BAR + (size_t)sl * p.tile_bytes);
        const uint32_t first = (uint32_t)(gc * te), a = t[0], b = t[te - 1];
        acc += a + b; err += (a != first) + (b != first + te - 1);
        if (++gc >= p.window_tiles) gc = 0;
        if (n + D < T) {
          mbar_arrive_expect_tx(full0 + 8 * sl, p.tile_bytes);
          bulk_g2s(slot0 + sl * p.tile_bytes, p.src + gi * te, p.tile_bytes, full0 + 8 * sl);
          if (++gi >= p.window_tiles) gi = 0;
        }
        if (++sl == D) { sl = 0; ph ^= 1; }
      }
      p.sums[cons] = acc; p.errs[cons] = err; p.tdone[cons] = T;
      p.times[2 * cons] = t0; p.times[2 * cons + 1] = gtimer();
    }
  } else if (role != -1) {
    const int D = p.D;
    const uint32_t T = p.T - p.T % (uint32_t)H, te = p.tile_bytes / 4;
    const uint64_t base = (uint64_t)cons * p.cons_stride;
    if (issuing) {
      const bool local = role >= 0;
      const uint32_t rfull = local ? full0 : mapa(full0, crank), rslot = local ? slot0 : mapa(slot0, crank);
      uint64_t g = (base + issuer) % p.window_tiles;
      uint32_t j = 0, ph = 0;
      for (uint32_t k = 0; k < T / H; ++k) {
        if (k >= (uint32_t)D) mbar_wait(ebase + 8 * j, ph, p.spin);
        const uint32_t s = issuer * D + j;
        bulk_g2s(rslot + s * p.tile_bytes, p.src + g * te, p.tile_bytes, rfull + 8 * s);
        g += H; while (g >= p.window_tiles) g -= p.window_tiles;
        if (++j == (uint32_t)D) { j = 0; if (k >= (uint32_t)D) ph ^= 1; }
      }
    }
    if (role >= 0 && warp >= 1 && warp <= p.nw) {
      const int w = warp - 1;
      unsigned long long t0 = 0;
      if (w == 0 && lane == 0) t0 = gtimer();
      uint32_t acc = 0, err = 0;
      uint64_t g = (base + w) % p.window_tiles;
      for (uint32_t n = w; n < T; n += p.nw, g += p.nw) {
        while (g >= p.window_tiles) g -= p.window_tiles;
        const uint32_t i = n % H, k = n / H, j = k % D, s = i * D + j, m = k / D;
        mbar_wait(full0 + 8 * s, m & 1, p.spin);
        const uint32_t first = (uint32_t)(g * te);
        const unsigned char* t = smem + RB_BAR + (size_t)s * p.tile_bytes;
        if (p.consume) {
          const uint4* v4 = reinterpret_cast<const uint4*>(t);
          const uint32_t nv = p.tile_bytes / 16;
#pragma unroll 8
          for (uint32_t q = lane; q < nv; q += 32) {
            const uint4 v = v4[q];
            acc += v.x + v.y + v.z + v.w;
            if (q == 0 && v.x != first) ++err;
          }
        } else if (lane == 0) {
          const uint32_t a = reinterpret_cast<const uint32_t*>(t)[0], b = reinterpret_cast<const uint32_t*>(t)[te - 1];
          acc += a + b; err += (a != first) + (b != first + te - 1);
        }
        __syncwarp();
        if (lane == 0) {
          mbar_arrive_expect_tx(full0 + 8 * s, p.tile_bytes);
          if (p.nlocal) mbar_arrive_local(empty0 + 8 * (i * D + j));
          else if (p.self_issue && i == 0) mbar_arrive_local(empty0 + 8 * j);
          else mbar_arrive_cluster(mapa(empty0 + 8 * j, (uint32_t)info[5 + i - p.self_issue]));
        }
      }
      for (int o = 16; o > 0; o >>= 1) { acc += __shfl_xor_sync(0xffffffffu, acc, o); err += __shfl_xor_sync(0xffffffffu, err, o); }
      if (lane == 0) { atomicAdd(&p.sums[cons], acc); atomicAdd(&p.errs[cons], err); }
      named_bar(1, p.nw * 32);
      if (w == 0 && lane == 0) { p.times[2 * cons] = t0; p.times[2 * cons + 1] = gtimer(); p.tdone[cons] = T; }
    }
  }
  cluster_sync_all();
}

// ---------------------------------------------------------------- host helpers
static std::vector<int> parse_list(const std::string& s) {
  std::vector<int> v;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string t = s.substr(i, j - i);
    if (!t.empty()) {
      size_t d = t.find('-');
      if (d != std::string::npos) { int a = atoi(t.substr(0, d).c_str()), b = atoi(t.substr(d + 1).c_str()); for (int k = a; k <= b; ++k) v.push_back(k); }
      else v.push_back(atoi(t.c_str()));
    }
    i = j + 1;
  }
  return v;
}
static int g_sm2bit[MAXSM];
static void load_map(const std::string& f) {
  for (int i = 0; i < MAXSM; ++i) g_sm2bit[i] = -1;
  FILE* fp = fopen(f.c_str(), "r");
  if (!fp) { fprintf(stderr, "cannot open map %s\n", f.c_str()); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, fp)) {
    int b; char* q = line;
    if (sscanf(q, "bit %d sms", &b) != 1) continue;
    q = strstr(q, "sms") + 3;
    int s, n;
    while (sscanf(q, "%d%n", &s, &n) == 1) { if (s >= 0 && s < MAXSM) g_sm2bit[s] = b; q += n; }
  }
  fclose(fp);
}
static u128 mask_of_sms(const std::vector<int>& sms) {
  u128 m = 0;
  for (int s : sms) { if (g_sm2bit[s] < 0) { fprintf(stderr, "SM %d not in map\n", s); exit(1); } m |= u128_bit(g_sm2bit[s]); }
  return m;
}
static double g_timeout_s = 30;
static void wait_stream(cudaStream_t st, const char* what) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    cudaError_t e = cudaStreamQuery(st);
    if (e == cudaSuccess) return;
    if (e != cudaErrorNotReady) { fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(e)); exit(1); }
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > g_timeout_s) {
      printf("TIMEOUT waiting for %s after %.0f s\n", what, g_timeout_s); fflush(stdout); _exit(124);
    }
    usleep(100);
  }
}
static double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static double vmin(const std::vector<double>& v) { return v.empty() ? 0 : *std::min_element(v.begin(), v.end()); }
static double vmax(const std::vector<double>& v) { return v.empty() ? 0 : *std::max_element(v.begin(), v.end()); }

// ---------------------------------------------------------------- probe
static int cmd_probe(int argc, char** argv) {
  int spin_us = 50; std::string out = "";
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--spin-us")) spin_us = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--out")) out = argv[++i];
  }
  cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, 0));
  int drv = 0, rt = 0; cudaDriverGetVersion(&drv); cudaRuntimeGetVersion(&rt);
  const int nsm = prop.multiProcessorCount, maxsmem = (int)prop.sharedMemPerBlockOptin;
  CK(cudaFuncSetAttribute(k_smid, cudaFuncAttributeMaxDynamicSharedMemorySize, maxsmem));
  const int grid = 3 * nsm;
  int* d; CK(cudaMallocManaged(&d, grid * sizeof(int)));
  auto run = [&](u128 en, int g) {
    for (int i = 0; i < g; ++i) d[i] = -1;
    tpc_next_enable(en);
    k_smid<<<g, 1024, maxsmem>>>(d, spin_us * 1000ll);
    CK(cudaGetLastError());
    wait_stream(0, "probe kernel");
    std::set<int> s(d, d + g);
    return s;
  };
  std::set<int> all = run(0, grid);
  printf("gpu=%s sms=%d driver_api=%d runtime=%d unmasked_distinct_sms=%zu\n", prop.name, nsm, drv, rt, all.size());
  printf("callbacks=%ld tmd_version=0x%02x\n", tpcm::g_callbacks, tpcm::g_last_tmd_ver);
  FILE* fo = out.empty() ? nullptr : fopen(out.c_str(), "w");
  int nalive = 0, bad = 0;
  std::set<int> covered;
  for (int b = 0; b < 128; ++b) {
    u128 en = ~u128_bit(b);
    std::set<int> s = run(en, grid);
    std::vector<int> miss;
    for (int x : all) if (!s.count(x)) miss.push_back(x);
    for (int x : s) if (!all.count(x)) printf("  bit %d: unexpected SM %d\n", b, x);
    if (miss.empty()) continue;
    ++nalive;
    // single-bit enable: every CTA must be on the missing SMs
    std::set<int> one = run(u128_bit(b), 8);
    bool ok = true;
    for (int x : one) if (std::find(miss.begin(), miss.end(), x) == miss.end()) ok = false;
    if (!ok) ++bad;
    printf("bit %3d sms", b);
    for (int x : miss) { printf(" %d", x); covered.insert(x); }
    printf("   single_bit_run_sms");
    for (int x : one) printf(" %d", x);
    printf("%s\n", ok ? "" : "  MISMATCH");
    if (fo) { fprintf(fo, "bit %d sms", b); for (int x : miss) fprintf(fo, " %d", x); fprintf(fo, "\n"); }
  }
  printf("summary: alive_bits=%d covered_sms=%zu of %zu single_bit_mismatches=%d callbacks=%ld applied=%ld tmd_version=0x%02x\n",
         nalive, covered.size(), all.size(), bad, tpcm::g_callbacks, tpcm::g_applied, tpcm::g_last_tmd_ver);
  if (fo) fclose(fo);
  return 0;
}

// ---------------------------------------------------------------- cluster placement
static int cmd_cluster(int argc, char** argv) {
  int C = 2, ncl = 1, spin_us = 200; std::string tpcs, sms, map;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--cluster")) C = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--nclusters")) ncl = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--tpcs")) tpcs = argv[++i];
    else if (!strcmp(argv[i], "--sms")) sms = argv[++i];
    else if (!strcmp(argv[i], "--map")) map = argv[++i];
    else if (!strcmp(argv[i], "--timeout-s")) g_timeout_s = atof(argv[++i]);
    else if (!strcmp(argv[i], "--spin-us")) spin_us = atoi(argv[++i]);
  }
  u128 en = 0;
  if (!tpcs.empty()) for (int b : parse_list(tpcs)) en |= u128_bit(b);
  if (!sms.empty()) { load_map(map); en |= mask_of_sms(parse_list(sms)); }
  cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, 0));
  const int maxsmem = (int)prop.sharedMemPerBlockOptin;
  CK(cudaFuncSetAttribute(k_cl_smid, cudaFuncAttributeMaxDynamicSharedMemorySize, maxsmem));
  CK(cudaFuncSetAttribute(k_cl_smid, cudaFuncAttributeNonPortableClusterSizeAllowed, 1));
  int* d; CK(cudaMallocManaged(&d, ncl * C * sizeof(int)));
  for (int i = 0; i < ncl * C; ++i) d[i] = -1;
  cudaLaunchConfig_t cfg = {}; cudaLaunchAttribute at[1];
  at[0].id = cudaLaunchAttributeClusterDimension; at[0].val.clusterDim.x = C; at[0].val.clusterDim.y = 1; at[0].val.clusterDim.z = 1;
  cfg.gridDim = dim3(ncl * C); cfg.blockDim = dim3(32); cfg.dynamicSmemBytes = maxsmem; cfg.attrs = at; cfg.numAttrs = 1;
  char hx[40]; u128_hex(en, hx);
  printf("cluster=%d nclusters=%d enabled_tpcs=%s popc=%d\n", C, ncl, en ? hx : "all", u128_popc(en));
  fflush(stdout);
  tpc_next_enable(en);
  cudaError_t e = cudaLaunchKernelEx(&cfg, k_cl_smid, d, spin_us * 1000ll);
  if (e != cudaSuccess) { printf("LAUNCH_ERROR %s\n", cudaGetErrorString(e)); return 2; }
  wait_stream(0, "cluster kernel");
  std::set<int> distinct(d, d + ncl * C);
  for (int c = 0; c < ncl; ++c) {
    std::vector<int> v(d + c * C, d + (c + 1) * C); std::sort(v.begin(), v.end());
    printf("  cluster %d sms:", c); for (int x : v) printf(" %d", x); printf("\n");
  }
  printf("OK distinct_sms=%zu ctas=%d\n", distinct.size(), ncl * C);
  return 0;
}

// ---------------------------------------------------------------- run (MEM + CMP)
// Host arrays with device mirrors (plain cudaMalloc + explicit copies). No
// managed memory: page faults on managed memory touched by one kernel while
// the other runs stalled the first kernel by hundreds of ms.
static std::map<void*, std::pair<void*, size_t>> g_mir;
template <class T> static void mir_alloc(T*& h, size_t bytes) {
  h = (T*)calloc(1, bytes); void* d; CK(cudaMalloc(&d, bytes)); CK(cudaMemset(d, 0, bytes)); g_mir[(void*)h] = {d, bytes};
}
template <class T> static T* dv(T* h) { return (T*)g_mir.at((void*)h).first; }
template <class T> static void up(T* h, cudaStream_t st) {
  auto& m = g_mir.at((void*)h); CK(cudaMemcpyAsync(m.first, (const void*)h, m.second, cudaMemcpyHostToDevice, st));
}
template <class T> static void down(T* h) { auto& m = g_mir.at((void*)h); CK(cudaMemcpy((void*)h, m.first, m.second, cudaMemcpyDeviceToHost)); }
static int cmd_run(int argc, char** argv) {
  std::string map, mem = "none", mem_sms, roles, extra, cmp_sms, tag = "-";
  int per_sm = 2, C = 1, self = 1, consume = 1, D = 4, nw = 4, reps = 3, cmp_per_sm = 1, tile_kb = 16, verbose = 0, nlocal = 0, spin = 0, cmp_threads = 1024, cmp_fixed = 0, fused = 0, ld_carveout = -1, cmp_carveout = 100, cmp_ilp = 4, mem_first = 0, fused_warps = 4;
  double mb_per_sm = 1024, mbpc = 1024, gib = 24, cmp_solo_ms = 0, window_mb = 0;
  unsigned cmp_iters = 65536;  // ~260 us per chunk; 8192 (33 us) polled the mapped stop flag so often that it stalled other SMs' TMA
  for (int i = 2; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--map")) map = argv[++i];
    else if (is("--mem")) mem = argv[++i];
    else if (is("--mem-sms")) mem_sms = argv[++i];
    else if (is("--per-sm")) per_sm = atoi(argv[++i]);
    else if (is("--mb-per-sm")) mb_per_sm = atof(argv[++i]);
    else if (is("--roles")) roles = argv[++i];
    else if (is("--extra-sms")) extra = argv[++i];
    else if (is("--cluster")) C = atoi(argv[++i]);
    else if (is("--self")) self = atoi(argv[++i]);
    else if (is("--consume")) consume = atoi(argv[++i]);
    else if (is("--depth")) D = atoi(argv[++i]);
    else if (is("--nw")) nw = atoi(argv[++i]);
    else if (is("--tile-kb")) tile_kb = atoi(argv[++i]);
    else if (is("--mb-per-consumer")) mbpc = atof(argv[++i]);
    else if (is("--cmp-sms")) cmp_sms = argv[++i];
    else if (is("--local-issuers")) nlocal = atoi(argv[++i]);
    else if (is("--spin")) spin = atoi(argv[++i]);
    else if (is("--fused")) fused = atoi(argv[++i]);
    else if (is("--ld-carveout")) ld_carveout = atoi(argv[++i]);
    else if (is("--cmp-carveout")) cmp_carveout = atoi(argv[++i]);
    else if (is("--cmp-ilp")) cmp_ilp = atoi(argv[++i]);
    else if (is("--mem-first")) mem_first = atoi(argv[++i]);
    else if (is("--fused-warps")) fused_warps = atoi(argv[++i]);
    else if (is("--cmp-threads")) cmp_threads = atoi(argv[++i]);
    else if (is("--cmp-fixed")) cmp_fixed = atoi(argv[++i]);
    else if (is("--cmp-per-sm")) cmp_per_sm = atoi(argv[++i]);
    else if (is("--cmp-iters")) cmp_iters = (unsigned)atoi(argv[++i]);
    else if (is("--cmp-solo-ms")) cmp_solo_ms = atof(argv[++i]);
    else if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--window-mb")) window_mb = atof(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--timeout-s")) g_timeout_s = atof(argv[++i]);
    else if (is("--tag")) tag = argv[++i];
    else if (is("--verbose")) verbose = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  load_map(map);
  if (fused) { consume = 0; self = 1; }
  cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, 0));
  const int maxsmem = (int)prop.sharedMemPerBlockOptin;
  // 2 KB below the per-block maximum, so that one k_role CTA fits beside a
  // k_fma / k_ld CTA (each CTA also holds 1 KB of reserved shared memory);
  // still above half the SM, so at most one k_role CTA per SM
  const int role_smem = maxsmem - 2048;
  // all kernels prefer the maximum shared-memory carveout, so that an SM
  // running k_fma is already configured for a k_role CTA
  if (cmp_carveout >= 0) CK(cudaFuncSetAttribute(k_fma, cudaFuncAttributePreferredSharedMemoryCarveout, cmp_carveout));
  // plain-load kernels keep the default carveout unless --ld-carveout is given:
  // the maximum shared carveout shrinks L1 and cut plain loads to ~62 GB/s per SM
  if (ld_carveout >= 0) {
    CK(cudaFuncSetAttribute(k_ld, cudaFuncAttributePreferredSharedMemoryCarveout, ld_carveout));
    CK(cudaFuncSetAttribute(k_ld32, cudaFuncAttributePreferredSharedMemoryCarveout, ld_carveout));
  }
  CK(cudaFuncSetAttribute(k_role, cudaFuncAttributePreferredSharedMemoryCarveout, 100));
  const size_t elems = (size_t)(gib * (1ull << 30)) / 4;
  uint32_t* buf; CK(cudaMalloc(&buf, elems * 4));
  init_buf<<<prop.multiProcessorCount * 4, 1024>>>(buf, elems);
  CK(cudaDeviceSynchronize());
  cudaStream_t sA, sB; CK(cudaStreamCreateWithFlags(&sA, cudaStreamNonBlocking)); CK(cudaStreamCreateWithFlags(&sB, cudaStreamNonBlocking));

  // ---- CMP setup
  const bool have_cmp = !cmp_sms.empty();
  std::vector<int> cmpv = parse_list(cmp_sms);
  u128 cmp_mask = have_cmp ? mask_of_sms(cmpv) : 0;
  const int maxch = 16384, maxslots = MAXSM * 2;
  CmpParams cp = {};
  unsigned char* cmp_allow; mir_alloc(cmp_allow, MAXSM);
  for (int s : cmpv) cmp_allow[s] = 1;
  int *cclaim, *cslot, *cslot_sm, *cnch; unsigned long long* cts; volatile int* hstop; int* dstop; float* cout_;
  mir_alloc(cclaim, MAXSM * 4); mir_alloc(cslot, 4); mir_alloc(cslot_sm, maxslots * 4);
  mir_alloc(cnch, maxslots * 4); CK(cudaMalloc(&cts, (size_t)maxslots * maxch * 8)); CK(cudaMalloc(&cout_, maxslots * 4));
  CK(cudaHostAlloc((void**)&hstop, 4, cudaHostAllocMapped)); CK(cudaHostGetDevicePointer((void**)&dstop, (void*)hstop, 0));
  cp.allow = dv(cmp_allow); cp.per_sm = cmp_per_sm; cp.claim = dv(cclaim); cp.slot_ctr = dv(cslot); cp.slot_sm = dv(cslot_sm); cp.ts = cts;
  cp.nch = dv(cnch); cp.maxch = maxch; cp.fixed = cmp_fixed; cp.ilp = cmp_ilp; cp.iters = cmp_iters; cp.stop = dstop; cp.out = cout_;
  std::vector<unsigned long long> hts;
  int cmp_grid = 0;
  {
    std::set<int> t; for (int s : cmpv) { t.insert(g_sm2bit[s]); }
    cmp_grid = (int)t.size() * 2 * 2 * cmp_per_sm;  // 2 SMs per TPC, 2x excess
  }
  auto cmp_launch = [&]() {
    memset(cclaim, 0, MAXSM * 4); *cslot = 0; *hstop = 0;
    up(cmp_allow, sA); up(cclaim, sA); up(cslot, sA);
    tpc_next_enable(cmp_mask);
    k_fma<<<cmp_grid, cmp_threads, 0, sA>>>(cp);
    CK(cudaGetLastError());
  };
  auto cmp_finish = [&](int& nslots) {
    *hstop = 1; __sync_synchronize();
    wait_stream(sA, "cmp kernel");
    down(cslot); down(cslot_sm); down(cnch);
    nslots = *cslot;
    hts.resize((size_t)nslots * maxch);
    CK(cudaMemcpy(hts.data(), cts, (size_t)nslots * maxch * 8, cudaMemcpyDeviceToHost));
  };
  // FMA rate (GFLOP/s) of all slots inside [w0, w1]
  auto cmp_rate = [&](int nslots, unsigned long long w0, unsigned long long w1, unsigned long long* first_end, unsigned long long* last_end) {
    double chunks = 0; unsigned long long fe = ~0ull, le = 0;
    for (int s = 0; s < nslots; ++s) {
      const unsigned long long* t = hts.data() + (size_t)s * maxch;
      const int n = cnch[s];
      fe = std::min(fe, t[0]); le = std::max(le, t[n - 1]);
      for (int c = 1; c < n; ++c) {
        // fraction of chunk c (t[c-1], t[c]] inside the window
        const double a = (double)std::max(t[c - 1], w0), b = (double)std::min(t[c], w1);
        if (b > a) chunks += (b - a) / (double)(t[c] - t[c - 1]);
      }
    }
    if (first_end) *first_end = fe;
    if (last_end) *last_end = le;
    return chunks * cmp_iters * (double)cmp_threads * 2.0 / (double)(w1 - w0);  // FLOP per ns = GFLOP/s
  };
  std::set<int> cmp_cov;

  // ---- MEM setup
  const bool is_ld = mem == "ld" || mem == "ld32", is_tma = mem == "tma";
  std::vector<int> memv = parse_list(mem_sms);
  u128 mem_mask = 0;
  int ncons = 0, mem_grid = 0;
  // ld
  LdParams lp = {};
  unsigned char* ld_allow; mir_alloc(ld_allow, MAXSM);
  int *ld_claim, *ld_ctr, *ld_sm; unsigned* ld_sums; unsigned long long* ld_times;
  size_t slice_vec = 0;
  int nslices = 0;
  // tma
  RoleParams rp = {};
  short *role, *owner; mir_alloc(role, MAXSM * 2); mir_alloc(owner, MAXSM * 2);
  for (int i = 0; i < MAXSM; ++i) { role[i] = -1; owner[i] = -1; }
  std::vector<int> cons_sm;
  unsigned *r_sums, *r_errs, *r_tdone; unsigned long long* r_times; int *r_h, *r_ctasm;
  const uint32_t tile_bytes = tile_kb * 1024;
  cudaLaunchConfig_t cfg = {}; cudaLaunchAttribute at[1];
  std::set<int> role_sms;
  if (is_ld) {
    for (int s : memv) ld_allow[s] = 1;
    mem_mask = mask_of_sms(memv);
    nslices = (int)memv.size() * per_sm;
    slice_vec = ((size_t)(mb_per_sm * (1 << 20)) / per_sm / 16) & ~(size_t)4095;
    if ((size_t)nslices * slice_vec * 16 > elems * 4) slice_vec = ((elems * 4 / 16) / nslices) & ~(size_t)4095;
    mir_alloc(ld_claim, MAXSM * 4); mir_alloc(ld_ctr, 4); mir_alloc(ld_sm, nslices * 4);
    mir_alloc(ld_sums, nslices * 4); mir_alloc(ld_times, nslices * 16);
    lp.src = (const uint4*)buf; lp.allow = dv(ld_allow); lp.per_sm = per_sm; lp.claim = dv(ld_claim); lp.ctr = dv(ld_ctr); lp.slice_vec = slice_vec;
    lp.sums = dv(ld_sums); lp.times = dv(ld_times); lp.slice_sm = dv(ld_sm);
    std::set<int> t; for (int s : memv) t.insert(g_sm2bit[s]);
    mem_grid = (int)t.size() * 2 * per_sm * 2;
  } else if (is_tma) {
    // roles "c:h,h;c:h"
    size_t i = 0;
    while (i < roles.size()) {
      size_t j = roles.find(';', i); if (j == std::string::npos) j = roles.size();
      std::string g = roles.substr(i, j - i);
      if (!g.empty()) {
        size_t k = g.find(':');
        const int c = atoi(g.substr(0, k).c_str());
        role[c] = (short)ncons++; cons_sm.push_back(c); role_sms.insert(c);
        if (k != std::string::npos) for (int h : parse_list(g.substr(k + 1))) { role[h] = -2; owner[h] = (short)c; role_sms.insert(h); }
      }
      i = j + 1;
    }
    std::vector<int> all(role_sms.begin(), role_sms.end());
    for (int s : parse_list(extra)) all.push_back(s);
    mem_mask = mask_of_sms(all);
    mem_grid = u128_popc(mem_mask) * 2;  // one CTA per SM of the masked TPCs
    if (mem_grid % C) { fprintf(stderr, "grid %d (2 x %d TPCs) not a multiple of cluster %d\n", mem_grid, u128_popc(mem_mask), C); return 1; }
    const uint64_t buf_tiles = elems * 4 / tile_bytes;
    uint64_t T = std::min<uint64_t>((uint64_t)(mbpc * (1 << 20)) / tile_bytes, buf_tiles / std::max(1, ncons));
    rp.src = buf; rp.window_tiles = buf_tiles; rp.cons_stride = T; rp.T = (uint32_t)T; rp.tile_bytes = tile_bytes;
    if (window_mb > 0) {  // every consumer re-reads one window (L2-resident after the warm-up rep)
      rp.window_tiles = std::max<uint64_t>(1, (uint64_t)(window_mb * (1 << 20)) / tile_bytes);
      rp.cons_stride = rp.window_tiles / std::max(1, ncons);
      rp.T = (uint32_t)((uint64_t)(mbpc * (1 << 20)) / tile_bytes);
    }
    rp.D = D; rp.self_issue = nlocal ? 0 : self; rp.consume = consume; rp.role = dv(role); rp.owner = dv(owner); rp.nlocal = nlocal; rp.spin = spin; rp.fused = fused;
    mir_alloc(r_sums, ncons * 4); mir_alloc(r_errs, ncons * 4); mir_alloc(r_tdone, ncons * 4);
    mir_alloc(r_times, ncons * 16); mir_alloc(r_h, ncons * 4); mir_alloc(r_ctasm, mem_grid * 4);
    rp.sums = dv(r_sums); rp.errs = dv(r_errs); rp.times = dv(r_times); rp.hcount = dv(r_h); rp.tdone = dv(r_tdone); rp.cta_sm = dv(r_ctasm);
    CK(cudaFuncSetAttribute(k_role, cudaFuncAttributeMaxDynamicSharedMemorySize, role_smem));
    CK(cudaFuncSetAttribute(k_role, cudaFuncAttributeNonPortableClusterSizeAllowed, 1));
    at[0].id = cudaLaunchAttributeClusterDimension; at[0].val.clusterDim.x = C; at[0].val.clusterDim.y = 1; at[0].val.clusterDim.z = 1;
    cfg.gridDim = dim3(mem_grid); cfg.blockDim = dim3(32 * (1 + nw)); cfg.dynamicSmemBytes = role_smem; cfg.attrs = at; cfg.numAttrs = 1;
    cfg.stream = sB;
    // largest issuer count decides whether the ring fits
    std::map<int, int> hc; for (int s = 0; s < MAXSM; ++s) if (role[s] == -2) hc[owner[s]]++;
    int hmax = self; for (auto& kv : hc) hmax = std::max(hmax, self + kv.second);
    if (nlocal) hmax = nlocal;
    // every ring slot must always go to the same consumer warp: nw | H*D
    { const int hd = hmax * D; int w = std::min(nw, hd); while (hd % w) --w; nw = w; }
    rp.nw = nw;
    cfg.blockDim = dim3(fused ? 32 * fused_warps : 32 * (1 + nw + nlocal));  // fused: --fused-warps warps, only thread 0 works
    if ((size_t)RB_BAR + (size_t)hmax * D * tile_bytes > (size_t)role_smem || hmax * D > 64) {
      printf("SKIP ring of %d issuers x depth %d does not fit\n", hmax, D); return 0;
    }
  }
  auto mem_launch = [&]() {
    if (is_ld) {
      memset(ld_claim, 0, MAXSM * 4); *ld_ctr = 0; memset(ld_sums, 0, nslices * 4);
      for (int i = 0; i < nslices; ++i) ld_sm[i] = -1;
      up(ld_allow, sB); up(ld_claim, sB); up(ld_ctr, sB); up(ld_sm, sB); up(ld_sums, sB);
      tpc_next_enable(mem_mask);
      if (mem == "ld32") k_ld32<<<mem_grid, 1024, 0, sB>>>(lp); else k_ld<<<mem_grid, 1024, 0, sB>>>(lp);
      CK(cudaGetLastError());
    } else if (is_tma) {
      memset(r_sums, 0, ncons * 4); memset(r_errs, 0, ncons * 4); memset(r_tdone, 0, ncons * 4); memset(r_h, 0, ncons * 4);
      memset(r_times, 0, ncons * 16);
      up(role, sB); up(owner, sB); up(r_sums, sB); up(r_errs, sB); up(r_tdone, sB); up(r_h, sB); up(r_times, sB);
      tpc_next_enable(mem_mask);
      cudaError_t e = cudaLaunchKernelEx(&cfg, k_role, rp);
      if (e != cudaSuccess) { printf("LAUNCH_ERROR %s\n", cudaGetErrorString(e)); exit(2); }
    }
  };

  // ---- reps
  std::vector<double> agg, percons_med, permin, permax, cmp_co, cmp_solo, win_ms;
  int bad = 0, hmin = 999, hmaxr = 0, nslots_seen = 0;
  std::set<int> mem_cov;
  std::vector<double> last_per;
  for (int r = 0; r <= reps; ++r) {
    int nslots = 0;
    const bool mf = mem_first && have_cmp && (is_ld || is_tma);
    if (have_cmp && !mf) { cmp_launch(); usleep(3000); }
    unsigned long long w0 = 0, w1 = 0;
    if (is_ld || is_tma) {
      mem_launch();
      if (mf) { usleep(100); cmp_launch(); }
      wait_stream(sB, "mem kernel");
      if (is_ld) { down(ld_sm); down(ld_sums); down(ld_times); }
      else { down(r_sums); down(r_errs); down(r_tdone); down(r_times); down(r_h); down(r_ctasm); }
      std::vector<double> per;
      unsigned long long tmin = ~0ull, tmax = 0; double bytes = 0;
      if (is_ld) {
        for (int o = 0; o < nslices; ++o) {
          if (ld_sm[o] < 0) { ++bad; continue; }
          mem_cov.insert(ld_sm[o]);
          const unsigned __int128 W = (unsigned __int128)slice_vec * 4, a0 = (unsigned __int128)o * W;
          const uint32_t expect = (uint32_t)(W * a0 + W * (W - 1) / 2);
          if (ld_sums[o] != expect) ++bad;
          tmin = std::min(tmin, ld_times[2 * o]); tmax = std::max(tmax, ld_times[2 * o + 1]);
          bytes += (double)slice_vec * 16;
        }
        // per SM: bytes of its slices over its span
        std::map<int, std::pair<double, std::pair<unsigned long long, unsigned long long>>> m;
        for (int o = 0; o < nslices; ++o) if (ld_sm[o] >= 0) {
          auto& e = m[ld_sm[o]];
          if (e.first == 0) e.second = {ld_times[2 * o], ld_times[2 * o + 1]};
          e.first += (double)slice_vec * 16;
          e.second.first = std::min(e.second.first, ld_times[2 * o]); e.second.second = std::max(e.second.second, ld_times[2 * o + 1]);
        }
        for (auto& kv : m) per.push_back(kv.second.first / (double)(kv.second.second.second - kv.second.second.first));
      } else {
        const uint32_t te = tile_bytes / 4;
        const uint64_t inner = (uint64_t)te * (te - 1) / 2;
        for (int c = 0; c < ncons; ++c) {
          const uint32_t T = r_tdone[c];
          if (T == 0) { ++bad; continue; }
          hmin = std::min(hmin, r_h[c]); hmaxr = std::max(hmaxr, r_h[c]);
          uint32_t s = 0;
          for (uint32_t n = 0; n < T; ++n) {
            const uint64_t g = ((uint64_t)c * rp.cons_stride + n) % rp.window_tiles;
            const uint32_t first = (uint32_t)(g * te);
            s += consume ? (uint32_t)(first * te + inner) : first + first + te - 1;
          }
          if (s != r_sums[c] || r_errs[c]) ++bad;
          tmin = std::min(tmin, r_times[2 * c]); tmax = std::max(tmax, r_times[2 * c + 1]);
          bytes += (double)T * tile_bytes;
          per.push_back((double)T * tile_bytes / (double)(r_times[2 * c + 1] - r_times[2 * c]));
        }
        if (r == 0) for (int i = 0; i < mem_grid; ++i) mem_cov.insert(r_ctasm[i]);
      }
      w0 = tmin; w1 = tmax;
      if (r > 0) {
        agg.push_back(bytes / (double)(tmax - tmin)); percons_med.push_back(median(per));
        permin.push_back(vmin(per)); permax.push_back(vmax(per)); win_ms.push_back((tmax - tmin) / 1e6);
        last_per = per;
      }
    } else {
      usleep((useconds_t)(cmp_solo_ms > 0 ? cmp_solo_ms * 1000 : 50000));
    }
    if (have_cmp) {
      cmp_finish(nslots);
      nslots_seen = nslots;
      if (r == 0) { cmp_cov.clear(); for (int s = 0; s < nslots; ++s) cmp_cov.insert(cslot_sm[s]); }
      unsigned long long fe, le;
      if (w1 == 0) { cmp_rate(nslots, 0, 1, &fe, &le); w0 = fe + 5000000ull; w1 = le - 2000000ull; }
      if (r > 0) {
        // with --mem-first the MEM window starts before CMP: count CMP only from its first CTA start
        unsigned long long cs, ce; cmp_rate(nslots, 0, 1, &cs, &ce);
        cmp_co.push_back(cmp_rate(nslots, std::max(w0, cs), w1, nullptr, nullptr));
      }
      // CMP alone for the same window length
      if (r > 0 && (is_ld || is_tma)) {
        const double len_ms = (w1 - w0) / 1e6;
        cmp_launch();
        usleep((useconds_t)(len_ms * 1000 + 8000));
        cmp_finish(nslots);
        cmp_rate(nslots, 0, 1, &fe, &le);
        const unsigned long long s0 = fe + 5000000ull;
        cmp_solo.push_back(cmp_rate(nslots, s0, std::min(le - 1000000ull, s0 + (unsigned long long)(len_ms * 1e6)), nullptr, nullptr));
      }
    }
  }
  char hm[40], hc[40]; u128_hex(mem_mask, hm); u128_hex(cmp_mask, hc);
  printf("tag=%s mem=%s", tag.c_str(), mem.c_str());
  if (is_ld) printf(" mem_sms=%zu per_sm=%d ld_carveout=%d slices=%d mb_per_slice=%.1f", memv.size(), per_sm, ld_carveout, nslices, slice_vec * 16.0 / (1 << 20));
  if (is_tma) printf(" fused=%d fused_warps=%d consumers=%d cluster=%d self=%d local_issuers=%d consume=%d depth=%d tile_kb=%d nw=%d spin=%d issuers_min=%d issuers_max=%d mb_per_consumer=%.1f",
                     fused, fused ? fused_warps : 0, ncons, C, self, nlocal, consume, D, tile_kb, nw, spin, hmin, hmaxr, rp.T * (double)tile_bytes / (1 << 20));
  printf(" mem_tpc_mask=%s mem_tpcs=%d mem_cta_sms=%zu role_smem_kb=%d", hm, u128_popc(mem_mask), mem_cov.size(), is_tma ? role_smem / 1024 : 0);
  if (have_cmp) printf(" mem_first=%d cmp_ilp=%d cmp_carveout=%d cmp_sms=%zu cmp_threads=%d cmp_per_sm=%d cmp_cov_sms=%zu cmp_slots=%d cmp_tpcs=%d", mem_first, cmp_ilp, cmp_carveout, cmpv.size(), cmp_threads, cmp_per_sm, cmp_cov.size(), nslots_seen, u128_popc(cmp_mask));
  printf(" reps=%d buf_gib=%.0f window_mb=%.0f check=%s bad=%d", reps, gib, window_mb, bad ? "FAIL" : "ok", bad);
  if (is_ld || is_tma)
    printf(" mem_gbs=%.1f mem_gbs_range=%.1f-%.1f per_unit_med=%.1f slowest=%.1f fastest=%.1f window_ms=%.2f",
           median(agg), vmin(agg), vmax(agg), median(percons_med), median(permin), median(permax), median(win_ms));
  if (have_cmp) {
    printf(" cmp_gflops=%.0f cmp_gflops_range=%.0f-%.0f", median(cmp_co), vmin(cmp_co), vmax(cmp_co));
    if (!cmp_solo.empty()) printf(" cmp_solo_gflops=%.0f cmp_ratio=%.3f", median(cmp_solo), median(cmp_co) / median(cmp_solo));
  }
  printf("\n");
  if (verbose && !last_per.empty()) {
    printf("  per_unit_gbs:");
    if (is_tma) for (int c = 0; c < ncons; ++c) printf(" sm%d=%.1f(h%d)", cons_sm[c], (double)r_tdone[c] * tile_bytes / (double)(r_times[2 * c + 1] - r_times[2 * c]), r_h[c]);
    else { std::set<int> u; for (int o = 0; o < nslices; ++o) u.insert(ld_sm[o]); for (double x : last_per) printf(" %.1f", x); printf("\n  ld_sms:"); for (int x : u) printf(" %d", x); }
    printf("\n");
  }
  if (verbose && is_tma) {
    printf("  cta_sms:"); for (int i = 0; i < mem_grid; ++i) printf(" %d", r_ctasm[i]); printf("\n");
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: tpc_lab probe|cluster|run ...\n"); return 1; }
  if (!strcmp(argv[1], "probe")) return cmd_probe(argc, argv);
  if (!strcmp(argv[1], "cluster")) return cmd_cluster(argc, argv);
  if (!strcmp(argv[1], "run")) return cmd_run(argc, argv);
  fprintf(stderr, "unknown subcommand %s\n", argv[1]);
  return 1;
}
