// L2 / memory-side structure probes for the H200 (uses tpc_smctrl.h masks).
//
//   l2s_lab lat  --map F --sm S [--warm none|self|SM] [--flush 1] [--off0 B]
//                [--stride B] [--count N] [--out FILE] [--gib G]
//       one thread on SM S times single ld.global.cg loads (clock64 before
//       the load, after a shared-memory store that depends on the value) at
//       off0 + i*stride. Before: optional L2 flush (all SMs stream 1 GiB of a
//       separate region), optional warm (one 1024-thread CTA on the warm SM
//       reads every 32 B sector of the probed lines with ld.global.cg).
//       Writes uint16 latencies to FILE; prints summary + effective clock.
//   l2s_lab bw   --map F --groups "SMLIST@LISTFILE;SMLIST@LISTFILE" [--block B]
//                [--per-sm P] [--passes N] [--shared 0|1] [--write 0|1] [--gib G]
//       each SM of group g runs P CTAs of 1024 threads; the CTAs of a group
//       split the group's block list (uint32 block indices, block = B bytes)
//       into disjoint contiguous parts (--shared 1: every CTA reads the whole
//       list), read (ld.global.cg 16 B) or write (st.global.cg) N passes.
//       LISTFILE "all:<n>" = blocks 0..n-1; "stride:<first>:<step>:<n>".
//   l2s_lab mix  --map F --sm S [--tma I --tile-kb T --depth D] [--ld L]
//                [--smr R] [--list LISTFILE --block B] [--dur-ms M]
//                [--second-ld L2] (a second kernel, loads only, same SM)
//       Part 2: one CTA on SM S with I TMA issuer warps (fused: issue, wait,
//       check, re-issue), L load warps (16 B loads, 8 in flight per lane) and
//       R shared-memory reader warps (own region, never written by TMA), all
//       running for M ms; per-component GB/s.
//   Buffer: a[i] = i (uint32 words); checks: lat compares each loaded word,
//   bw reads sum every word, mix checks TMA first/last words and one word
//   per load.
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
#include <unistd.h>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)
#define MAXSM 256
#define MAXG 16

__device__ __forceinline__ uint32_t sm_id() { uint32_t r; asm volatile("mov.u32 %0, %%smid;" : "=r"(r)); return r; }
__device__ __forceinline__ unsigned long long gtimer() { unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t; }
__device__ __forceinline__ long long clk() { long long c; asm volatile("mov.u64 %0, %%clock64;" : "=l"(c) :: "memory"); return c; }
__device__ __forceinline__ uint32_t smem_u32(const void* p) { return (uint32_t)__cvta_generic_to_shared(p); }
__device__ __forceinline__ uint32_t ldcg_u32(const void* p) {
  uint32_t v; asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(v) : "l"(p) : "memory"); return v;
}
__device__ __forceinline__ void mbar_init(uint32_t bar, uint32_t count) { asm volatile("mbarrier.init.shared::cta.b64 [%0], %1;" :: "r"(bar), "r"(count) : "memory"); }
__device__ __forceinline__ void mbar_arrive_expect_tx(uint32_t bar, uint32_t tx) {
  asm volatile("mbarrier.arrive.expect_tx.shared::cta.b64 _, [%0], %1;" :: "r"(bar), "r"(tx) : "memory");
}
__device__ __forceinline__ void mbar_wait(uint32_t bar, uint32_t parity) {
  uint32_t ok = 0;
  for (;;) {  // test_wait + 100 ns backoff (a tight poll loop cut TMA to ~140 GB/s in tpc_lab)
    asm volatile("{\n\t.reg .pred p;\n\tmbarrier.test_wait.parity.shared::cta.b64 p, [%1], %2;\n\t"
                 "selp.u32 %0, 1, 0, p;\n\t}" : "=r"(ok) : "r"(bar), "r"(parity) : "memory");
    if (ok) return;
    __nanosleep(100);
  }
}
__device__ __forceinline__ void bulk_g2s(uint32_t dst, const void* src, uint32_t bytes, uint32_t bar) {
  asm volatile("cp.async.bulk.shared::cluster.global.mbarrier::complete_tx::bytes [%0], [%1], %2, [%3];"
               :: "r"(dst), "l"(src), "r"(bytes), "r"(bar) : "memory");
}

__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) a[i] = (uint32_t)i;
}
__global__ void k_flush(const uint4* a, size_t n, unsigned* out) {
  uint32_t s = 0;
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) { uint4 v = __ldcg(a + i); s += v.x ^ v.w; }
  if (s == 0x9e3779b9u) out[0] = s;
}

// ------------------------------------------------------------------ latency
__global__ void k_touch(const char* buf, uint64_t off0, uint64_t stride, uint64_t n, int target) {
  if ((int)sm_id() != target) return;
  uint32_t s = 0;
  const uint64_t per = stride / 32 ? stride / 32 : 1;  // sectors per probed line
  for (uint64_t k = threadIdx.x; k < n * per; k += blockDim.x) {
    const uint64_t i = k / per, j = k % per;
    s += ldcg_u32(buf + off0 + i * stride + j * 32);
  }
  if (s == 0x12345u) ((uint32_t*)buf)[0] = s;
}
__global__ void k_lat(const char* buf, uint64_t off0, uint64_t stride, uint64_t n, int target, unsigned short* out,
                      unsigned* errs, unsigned long long* t, long long* c) {
  __shared__ uint32_t sink[32];
  if ((int)sm_id() != target || threadIdx.x != 0) return;
  const uint32_t sa = smem_u32(sink);
  uint32_t e = 0;
  const unsigned long long g0 = gtimer(); const long long k0 = clk();
  // true dependency: a[w] = w, so the loaded word gives the next address
  // (next = buf + 4*v + stride); the clock read after it must wait for v
  const char* p = buf + off0;
  for (uint64_t i = 0; i < n; ++i) {
    long long c0, c1; uint32_t v;
    asm volatile("mov.u64 %0, %%clock64;" : "=l"(c0) :: "memory");
    asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(v) : "l"(p) : "memory");
    const char* nx = buf + (uint64_t)v * 4 + stride;
    asm volatile("{\n\t.reg .u64 q;\n\tmov.u64 q, %1;\n\tmov.u64 %0, %%clock64;\n\t}" : "=l"(c1) : "l"(nx) : "memory");
    const long long d = c1 - c0;
    out[i] = (unsigned short)(d > 65535 ? 65535 : d);
    e += v != (uint32_t)((off0 + i * stride) / 4);
    p = nx;
  }
  t[0] = gtimer() - g0; c[0] = clk() - k0; errs[0] = e;
}

// ------------------------------------------------------------------ bandwidth over block lists
struct BwP {
  const char* buf;
  const uint32_t* list;
  int gstart[MAXG], gcount[MAXG], gunits[MAXG], gcontig[MAXG]; uint32_t gfirst[MAXG];
  const signed char* group;  // [MAXSM]
  int per_sm, B, passes, shared, write, ldmode;
  int* claim; int* gctr;
  unsigned* sums; unsigned long long* t; long long* cyc; int* unit_sm; int unit_base[MAXG];
};
template <int U, int MINB>
__global__ void __launch_bounds__(1024, MINB) k_bw(BwP p) {
  __shared__ int s_u, s_g;
  if (threadIdx.x == 0) {
    const int sm = (int)sm_id(), g = p.group[sm];
    int u = -1;
    if (g >= 0 && atomicAdd(&p.claim[sm], 1) < p.per_sm) { u = atomicAdd(&p.gctr[g], 1); if (u >= p.gunits[g]) u = -1; }
    if (u >= 0) p.unit_sm[p.unit_base[g] + u] = sm;
    s_u = u; s_g = g;
  }
  __syncthreads();
  const int u = s_u, g = s_g;
  if (u < 0) return;
  const int gu = p.unit_base[g] + u;
  const int bpr = 1;
  const int per0 = p.shared ? p.gcount[g] : p.gcount[g] / p.gunits[g], per = per0 / bpr * bpr;
  const uint32_t* L = p.list + p.gstart[g] + (p.shared ? 0 : u * per0);
  const bool contig = p.gcontig[g];
  const uint32_t cfirst = p.gfirst[g] + (p.shared ? 0 : u * per0);
  // thread-linear pattern of tpc_lab k_ld: element e = t + (i+u)*1024 (16 B each),
  // block = e >> vsh, offset = (e & vmask)*16; one list lookup per element
  const int vsh = __ffs(p.B / 16) - 1;
  const uint32_t vmask = (1u << vsh) - 1;
  const uint32_t nvec = (uint32_t)per << vsh;
  const uint64_t Bsz = (uint64_t)p.B;
  const unsigned long long t0 = gtimer(); const long long c0 = clk();
  uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  for (int pass = 0; pass < p.passes; ++pass) {
    uint32_t e = threadIdx.x;
    for (; e + (U - 1) * 1024 < nvec; e += U * 1024) {
      const uint4* a[U];
#pragma unroll
      for (int k = 0; k < U; ++k) {
        const uint32_t ee = e + k * 1024, blk = ee >> vsh;
        a[k] = reinterpret_cast<const uint4*>(p.buf + (uint64_t)(contig ? cfirst + blk : __ldg(L + blk)) * Bsz + (ee & vmask) * 16u);
      }
      if (p.write) {
#pragma unroll
        for (int k = 0; k < U; ++k) {
          const uint32_t w = (uint32_t)(((const char*)a[k] - p.buf) / 4);
          __stcg(const_cast<uint4*>(a[k]), make_uint4(w, w + 1, w + 2, w + 3));
        }
      } else {
        uint4 v[U];
#pragma unroll
        if (p.ldmode) {
#pragma unroll
          for (int k = 0; k < U; ++k) v[k] = *a[k];
        } else {
#pragma unroll
          for (int k = 0; k < U; ++k) v[k] = __ldcg(a[k]);
        }
#pragma unroll
        for (int k = 0; k < U; k += 2) { s0 += v[k].x + v[k].y + v[k].z + v[k].w; s1 += v[k + 1].x + v[k + 1].y + v[k + 1].z + v[k + 1].w; }
      }
    }
    for (; e < nvec; e += 1024) {
      const uint32_t blk = e >> vsh;
      const uint4* a = reinterpret_cast<const uint4*>(p.buf + (uint64_t)(contig ? cfirst + blk : __ldg(L + blk)) * Bsz + (e & vmask) * 16u);
      if (p.write) { const uint32_t w = (uint32_t)(((const char*)a - p.buf) / 4); __stcg(const_cast<uint4*>(a), make_uint4(w, w + 1, w + 2, w + 3)); }
      else { const uint4 v = __ldcg(a); s0 += v.x + v.y + v.z + v.w; }
    }
  }
  uint32_t s = s0 + s1 + s2 + s3;
  for (int k = 16; k > 0; k >>= 1) s += __shfl_xor_sync(0xffffffffu, s, k);
  if ((threadIdx.x & 31) == 0) atomicAdd(&p.sums[gu], s);
  __syncthreads();
  if (threadIdx.x == 0) { p.t[2 * gu] = t0; p.t[2 * gu + 1] = gtimer(); p.cyc[gu] = clk() - c0; }
}

// ------------------------------------------------------------------ Part 2 mix kernel
struct MixP {
  const char* buf; const uint32_t* list; int nlist; int B;
  int target, tma, depth, tile, ld, smr;
  unsigned long long dur_ns;
  unsigned long long* bytes;  // [3]: tma, ld, smr
  unsigned long long* tt;     // [6]: start/end per component
  unsigned* errs; long long* cyc; unsigned long long* gns; int* claim;
};
__global__ void k_mix(MixP p) {
  extern __shared__ __align__(1024) unsigned char smem[];
  __shared__ unsigned long long s_end;
  __shared__ int s_ok;
  if (threadIdx.x == 0) s_ok = (int)sm_id() == p.target && atomicAdd(p.claim, 1) == 0;  // one working CTA on the SM
  __syncthreads();
  if (!s_ok) return;
  const int warp = threadIdx.x / 32, lane = threadIdx.x % 32;
  const uint32_t base = smem_u32(smem), bars = base, slots = base + 1024;
  // TMA slots: [1024, 1024 + tma*depth*tile); smem-reader region after that
  const uint32_t ring = (uint32_t)p.tma * p.depth * p.tile;
  if (threadIdx.x == 0) {
    for (int s = 0; s < p.tma * p.depth; ++s) mbar_init(bars + 8 * s, 1);
    asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
    s_end = gtimer() + p.dur_ns;
  }
  __syncthreads();
  const unsigned long long tend = s_end;
  // list split: TMA uses entries [0, nlist/2), loads [nlist/2, nlist) (disjoint data)
  const int half = p.tma && p.ld ? p.nlist / 2 : p.nlist;
  if (warp < p.tma) {
    if (lane == 0) {
      const int i = warp, D = p.depth, per = half / p.tma;
      const uint32_t te = p.tile / 4;
      int gi = 0, gc = 0;
      unsigned long long nb = 0; unsigned e = 0;
      const unsigned long long t0 = gtimer();
      auto src = [&](int k) { return p.buf + (uint64_t)p.list[i * per + (k % per)] * p.B; };
      for (int n = 0; n < D; ++n) {
        const uint32_t b = bars + 8 * (i * D + n);
        mbar_arrive_expect_tx(b, p.tile);
        bulk_g2s(slots + (uint32_t)(i * D + n) * p.tile, src(gi++), p.tile, b);
      }
      int sl = 0; uint32_t ph = 0, inflight = D;
      while (inflight) {
        const uint32_t b = bars + 8 * (i * D + sl);
        mbar_wait(b, ph);
        const uint32_t* t = reinterpret_cast<const uint32_t*>(smem + 1024 + (size_t)(i * D + sl) * p.tile);
        const uint32_t first = (uint32_t)(((uint64_t)p.list[i * per + (gc % per)] * p.B) / 4);
        e += (t[0] != first) + (t[te - 1] != first + te - 1);
        ++gc; nb += p.tile; --inflight;
        if (gtimer() < tend) {
          mbar_arrive_expect_tx(b, p.tile);
          bulk_g2s(slots + (uint32_t)(i * D + sl) * p.tile, src(gi++), p.tile, b);
          ++inflight;
        }
        if (++sl == D) { sl = 0; ph ^= 1; }
      }
      atomicAdd(&p.bytes[0], nb); atomicAdd(&p.errs[0], e);
      atomicMin(&p.tt[0], t0); atomicMax(&p.tt[1], gtimer());
    }
  } else if (warp < p.tma + p.ld) {
    const int w = warp - p.tma;
    const int lbase = p.nlist - half;  // loads use the other half (or all)
    const uint64_t vpb = p.B / 16, nvec = (uint64_t)half * vpb;
    uint64_t e = (uint64_t)w * 32 + lane;
    const uint64_t step = (uint64_t)p.ld * 32;
    uint32_t s = 0; unsigned err = 0; unsigned long long nb = 0;
    const unsigned long long t0 = gtimer(); const long long c0 = clk();
    while (gtimer() < tend) {
      uint4 v[8]; const uint4* a[8];
#pragma unroll
      for (int k = 0; k < 8; ++k) {
        const uint64_t ee = (e + k * step) % nvec;
        a[k] = reinterpret_cast<const uint4*>(p.buf + (uint64_t)p.list[lbase + ee / vpb] * p.B + (ee % vpb) * 16);
      }
#pragma unroll
      for (int k = 0; k < 8; ++k) v[k] = __ldcg(a[k]);
#pragma unroll
      for (int k = 0; k < 8; ++k) { s += v[k].y; err += v[k].x != (uint32_t)(((const char*)a[k] - p.buf) / 4); }
      e = (e + 8 * step) % nvec; nb += 8 * 16;
    }
    if (s == 0x31415u) p.errs[2] = s;
    atomicAdd(&p.bytes[1], nb); atomicAdd(&p.errs[1], err);
    atomicMin(&p.tt[2], t0); atomicMax(&p.tt[3], gtimer());
    if (w == 0 && lane == 0) { p.cyc[0] = clk() - c0; p.gns[0] = gtimer() - t0; }
  } else if (warp < p.tma + p.ld + p.smr) {
    const int w = warp - p.tma - p.ld;
    const uint4* r = reinterpret_cast<const uint4*>(smem + 1024 + ring) + w * 1024;  // 16 KB per reader warp
    uint32_t s = 0; unsigned long long nb = 0;
    const unsigned long long t0 = gtimer();
    while (gtimer() < tend) {
#pragma unroll
      for (int k = 0; k < 32; ++k) { const uint4 v = r[(lane + k * 32) & 1023]; s += v.x ^ v.w; }
      nb += 32 * 16;
    }
    if (s == 0x27182u) p.errs[2] = s;
    atomicAdd(&p.bytes[2], nb);
    atomicMin(&p.tt[4], t0); atomicMax(&p.tt[5], gtimer());
  }
}


// ------------------------------------------------------------------ fixed-window multi-SM stream (E-1/E-2)
struct TwP {
  const char* buf; const uint32_t* list; int B;
  int gstart[MAXG], gcount[MAXG], gunits[MAXG], unit_base[MAXG];
  const signed char* group; int* claim; int* gctr;
  int tma, depth, tile, ld, smr, shared, write;
  unsigned long long dur_ns; unsigned long long* start;  // start[0]: common start time (set by the first CTA)
  unsigned long long* ub; long long* ucyc; int* usm; unsigned* errs; unsigned long long* usmr;
};
__global__ void k_tw(TwP p) {
  extern __shared__ __align__(1024) unsigned char smem[];
  __shared__ int s_u, s_g; __shared__ unsigned long long s_t0;
  if (threadIdx.x == 0) {
    const int sm = (int)sm_id(), g = p.group[sm];
    int u = -1;
    if (g >= 0 && atomicAdd(&p.claim[sm], 1) == 0) { u = atomicAdd(&p.gctr[g], 1); if (u >= p.gunits[g]) u = -1; }
    s_u = u; s_g = g;
    if (u >= 0) {
      p.usm[p.unit_base[g] + u] = sm;
      // common window start: the first unit sets now + 300 us, everyone waits for it
      unsigned long long st = atomicCAS(p.start, 0ull, gtimer() + 300000ull);
      if (st == 0) st = *((volatile unsigned long long*)p.start);
      s_t0 = st;
      const uint32_t bars = smem_u32(smem);
      for (int s = 0; s < p.tma * p.depth; ++s) mbar_init(bars + 8 * s, 1);
      asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
    }
  }
  __syncthreads();
  const int u = s_u, g = s_g;
  if (u < 0) return;
  const int gu = p.unit_base[g] + u;
  const unsigned long long t0 = s_t0, tend = t0 + p.dur_ns;
  while (gtimer() < t0) {}
  const int warp = threadIdx.x / 32, lane = threadIdx.x % 32;
  const int per = p.gcount[g] / p.gunits[g];
  const uint32_t* L = p.list + p.gstart[g] + (p.shared ? 0 : u * per);
  const int half = p.tma && p.ld ? per / 2 : per;
  const uint32_t bars = smem_u32(smem), slots = bars + 1024;
  const long long c0 = clk();
  __shared__ uint32_t s_first[64];
  if (warp < p.tma) {
    if (lane == 0) {
      const int i = warp, D = p.depth, tper = half / p.tma;
      const uint32_t te = p.tile / 4;
      const uint32_t* Li = L + i * tper;
      int gi = 0; unsigned long long nb = 0; unsigned e = 0;
      uint32_t nxt = __ldg(Li);  // block index of the next tile to issue (prefetched)
      auto issue = [&](int slot) {
        const uint32_t b = bars + 8 * (i * D + slot);
        s_first[i * D + slot] = (uint32_t)(((uint64_t)nxt * p.B) / 4);
        mbar_arrive_expect_tx(b, p.tile);
        bulk_g2s(slots + (uint32_t)(i * D + slot) * p.tile, p.buf + (uint64_t)nxt * p.B, p.tile, b);
        if (++gi == tper) gi = 0;
        nxt = __ldg(Li + gi);
      };
      for (int n = 0; n < D; ++n) issue(n);
      int sl = 0; uint32_t ph = 0, inflight = D;
      while (inflight) {
        const uint32_t b = bars + 8 * (i * D + sl);
        mbar_wait(b, ph);
        const unsigned long long now = gtimer();
        const uint32_t* t = reinterpret_cast<const uint32_t*>(smem + 1024 + (size_t)(i * D + sl) * p.tile);
        const uint32_t first = s_first[i * D + sl];
        e += (t[0] != first) + (t[te - 1] != first + te - 1);
        --inflight;
        if (now <= tend) nb += p.tile;
        if (now < tend) { issue(sl); ++inflight; }
        if (++sl == D) { sl = 0; ph ^= 1; }
      }
      atomicAdd(&p.ub[2 * gu], nb); atomicAdd(&p.errs[0], e);
    }
  } else if (warp < p.tma + p.ld) {
    // each warp streams whole blocks: 8 rows (8 x 512 B) per iteration, block
    // index looked up once per block and prefetched one block ahead
    const int w = warp - p.tma;
    const uint32_t* LL = L + (per - half);
    const uint32_t nblk = (uint32_t)half, rows = (uint32_t)p.B / 512, step = (uint32_t)p.ld;
    uint32_t bi = (uint32_t)w % nblk, r0 = 0;
    uint32_t cur = __ldg(LL + bi), nbi = bi + step; while (nbi >= nblk) nbi -= nblk;
    uint32_t nxt = __ldg(LL + nbi);
    uint32_t s = 0; unsigned err = 0; unsigned long long nb = 0;
    for (;;) {
      const uint4* base = reinterpret_cast<const uint4*>(p.buf + (uint64_t)cur * p.B + r0 * 512u) + lane;
      uint4 v[8];
      const uint32_t w0 = (uint32_t)(((uint64_t)cur * p.B + r0 * 512u) / 4) + lane * 4;
      if (p.write) {
#pragma unroll
        for (int k = 0; k < 8; ++k) { const uint32_t ww = w0 + k * 128; __stcg(const_cast<uint4*>(base + k * 32), make_uint4(ww, ww + 1, ww + 2, ww + 3)); v[k] = make_uint4(ww, 0, 0, 0); }
      } else {
#pragma unroll
        for (int k = 0; k < 8; ++k) v[k] = __ldcg(base + k * 32);
      }
      r0 += 8;
      if (r0 >= rows) { r0 = 0; cur = nxt; bi = nbi; nbi = bi + step; while (nbi >= nblk) nbi -= nblk; nxt = __ldg(LL + nbi); }
#pragma unroll
      for (int k = 0; k < 8; ++k) { s += v[k].y; err += v[k].x != w0 + k * 128; }
      const unsigned long long now = gtimer();
      if (now > tend) break;
      nb += 8 * 512;
    }
    if (s == 0x31415u) p.errs[2] = s;
    if (lane == 0) atomicAdd(&p.ub[2 * gu + 1], nb);
    if (err) atomicAdd(&p.errs[1], err);
  } else if (warp < p.tma + p.ld + p.smr) {
    // shared-memory readers: own 16 KB region after the TMA ring (never written by TMA)
    const int w = warp - p.tma - p.ld;
    const uint4* r = reinterpret_cast<const uint4*>(smem + 1024 + (size_t)p.tma * p.depth * p.tile) + w * 1024;
    uint32_t s = 0, it = 0; unsigned long long nb = 0;
    for (;;) {
#pragma unroll
      for (int k = 0; k < 32; ++k) { const uint4 v = r[(lane + k * 32 + it) & 1023]; s += v.x ^ v.w; }
      it = (it + 32 + (uint32_t)p.write) & 1023;  // offset moves every iteration (runtime param), loads cannot be hoisted
      if (gtimer() > tend) break;
      nb += 32 * 512;
    }
    if (s == 0x27182u) p.errs[2] = s;
    if (lane == 0) atomicAdd(&p.usmr[gu], nb);
  }
  __syncthreads();
  if (threadIdx.x == 0) p.ucyc[gu] = clk() - c0;
}

// ------------------------------------------------------------------ host helpers
static std::vector<int> parse_list(const std::string& s) {
  std::vector<int> v; size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string t = s.substr(i, j - i);
    if (!t.empty()) { size_t d = t.find('-');
      if (d != std::string::npos) for (int k = atoi(t.substr(0, d).c_str()); k <= atoi(t.substr(d + 1).c_str()); ++k) v.push_back(k);
      else v.push_back(atoi(t.c_str())); }
    i = j + 1;
  }
  return v;
}
static int g_sm2bit[MAXSM];
static void load_map(const std::string& f) {
  for (int i = 0; i < MAXSM; ++i) g_sm2bit[i] = -1;
  FILE* fp = fopen(f.c_str(), "r"); if (!fp) { fprintf(stderr, "cannot open map %s\n", f.c_str()); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, fp)) {
    int b; if (sscanf(line, "bit %d sms", &b) != 1) continue;
    char* q = strstr(line, "sms") + 3; int s, n;
    while (sscanf(q, "%d%n", &s, &n) == 1) { if (s >= 0 && s < MAXSM) g_sm2bit[s] = b; q += n; }
  }
  fclose(fp);
}
static u128 mask_of_sms(const std::vector<int>& sms) {
  u128 m = 0;
  for (int s : sms) { if (g_sm2bit[s] < 0) { fprintf(stderr, "SM %d not in map\n", s); exit(1); } m |= u128_bit(g_sm2bit[s]); }
  return m;
}
static double g_timeout = 60;
static void wait_dev(const char* what) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    cudaError_t e = cudaStreamQuery(0);
    if (e == cudaSuccess) return;
    if (e != cudaErrorNotReady) { fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(e)); exit(1); }
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > g_timeout) { printf("TIMEOUT %s\n", what); fflush(stdout); _exit(124); }
    usleep(50);
  }
}
static double median(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
struct Args {
  std::map<std::string, std::string> kv;
  Args(int argc, char** argv) { for (int i = 2; i + 1 < argc; i += 2) { if (strncmp(argv[i], "--", 2)) { fprintf(stderr, "bad arg %s\n", argv[i]); exit(1); } kv[argv[i] + 2] = argv[i + 1]; } }
  std::string s(const char* k, const char* d = "") { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
  double f(const char* k, double d) { auto it = kv.find(k); return it == kv.end() ? d : atof(it->second.c_str()); }
  long long i(const char* k, long long d) { auto it = kv.find(k); return it == kv.end() ? d : atoll(it->second.c_str()); }
};
static uint32_t* g_buf = nullptr; static size_t g_elems = 0;
static void alloc_buf(double gib) {
  g_elems = (size_t)(gib * (1ull << 30)) / 4;
  CK(cudaMalloc(&g_buf, g_elems * 4));
  init_buf<<<132 * 4, 1024>>>(g_buf, g_elems); CK(cudaDeviceSynchronize());
  printf("buf=%p gib=%.2f align_2MB=%s\n", (void*)g_buf, gib, ((uintptr_t)g_buf % (2u << 20)) ? "no" : "yes");
}
static void flush_l2() {
  // stream the last 1 GiB of the buffer from all SMs (no mask)
  const size_t n = (1ull << 30) / 16, off = g_elems * 4 - (1ull << 30);
  unsigned* o; CK(cudaMalloc(&o, 4));
  tpc_next_enable(0);
  k_flush<<<132 * 2, 1024>>>(reinterpret_cast<const uint4*>((const char*)g_buf + off), n, o);
  CK(cudaDeviceSynchronize()); CK(cudaFree(o));
}

// ------------------------------------------------------------------ lat
static int cmd_lat(Args& a) {
  load_map(a.s("map"));
  alloc_buf(a.f("gib", 8));
  const int sm = (int)a.i("sm", 0);
  const std::string warm = a.s("warm", "none");
  const uint64_t off0 = a.i("off0", 0), stride = a.i("stride", 128), n = a.i("count", 16384);
  if (off0 + n * stride > g_elems * 4 - (1ull << 30)) { fprintf(stderr, "probe range overlaps the flush region\n"); return 1; }
  unsigned short* d_out; unsigned* d_err; unsigned long long* d_t; long long* d_c;
  CK(cudaMalloc(&d_out, n * 2)); CK(cudaMalloc(&d_err, 4)); CK(cudaMalloc(&d_t, 8)); CK(cudaMalloc(&d_c, 8));
  const int maxsmem = 200 * 1024;
  CK(cudaFuncSetAttribute(k_touch, cudaFuncAttributeMaxDynamicSharedMemorySize, maxsmem));
  CK(cudaFuncSetAttribute(k_lat, cudaFuncAttributeMaxDynamicSharedMemorySize, maxsmem));
  if (a.i("flush", 1)) flush_l2();
  int wsm = -1;
  if (warm == "self") wsm = sm; else if (warm != "none") wsm = atoi(warm.c_str());
  if (wsm >= 0) {
    tpc_next_enable(mask_of_sms({wsm}));
    k_touch<<<2, 1024, maxsmem>>>((const char*)g_buf, off0, stride, n, wsm);
    wait_dev("touch");
  }
  tpc_next_enable(mask_of_sms({sm}));
  k_lat<<<2, 32, maxsmem>>>((const char*)g_buf, off0, stride, n, sm, d_out, d_err, d_t, d_c);
  wait_dev("lat");
  std::vector<unsigned short> h(n); unsigned err; unsigned long long t; long long c;
  CK(cudaMemcpy(h.data(), d_out, n * 2, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&err, d_err, 4, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(&t, d_t, 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&c, d_c, 8, cudaMemcpyDeviceToHost));
  std::string out = a.s("out");
  if (!out.empty()) { FILE* f = fopen(out.c_str(), "wb"); fwrite(h.data(), 2, n, f); fclose(f); }
  std::vector<double> v(h.begin(), h.end()); std::sort(v.begin(), v.end());
  printf("lat sm=%d warm=%s off0=%llu stride=%llu count=%llu errs=%u p5=%.0f p25=%.0f p50=%.0f p75=%.0f p95=%.0f clock_mhz=%.0f out=%s\n",
         sm, warm.c_str(), (unsigned long long)off0, (unsigned long long)stride, (unsigned long long)n, err,
         v[n * 5 / 100], v[n / 4], v[n / 2], v[n * 3 / 4], v[n * 95 / 100], 1000.0 * c / t, out.c_str());
  return 0;
}

// ------------------------------------------------------------------ bw
static std::vector<uint32_t> read_list(const std::string& spec) {
  std::vector<uint32_t> v;
  if (!spec.compare(0, 4, "all:")) { uint32_t n = (uint32_t)atoll(spec.c_str() + 4); v.resize(n); for (uint32_t i = 0; i < n; ++i) v[i] = i; return v; }
  if (!spec.compare(0, 7, "stride:")) {
    unsigned long long f, st, n; sscanf(spec.c_str() + 7, "%llu:%llu:%llu", &f, &st, &n);
    for (unsigned long long i = 0; i < n; ++i) v.push_back((uint32_t)(f + i * st)); return v;
  }
  FILE* fp = fopen(spec.c_str(), "rb"); if (!fp) { fprintf(stderr, "cannot open list %s\n", spec.c_str()); exit(1); }
  fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
  v.resize(sz / 4); if (fread(v.data(), 4, v.size(), fp) != v.size()) exit(1); fclose(fp);
  return v;
}
static int cmd_bw(Args& a) {
  load_map(a.s("map"));
  alloc_buf(a.f("gib", 24));
  const int B = (int)a.i("block", 4096), per_sm = (int)a.i("per-sm", 2), passes = (int)a.i("passes", 1);
  const int shared = (int)a.i("shared", 0), write = (int)a.i("write", 0), reps = (int)a.i("reps", 3), ldmode = (int)a.i("ldmode", 0), unroll = (int)a.i("unroll", 8);
  g_timeout = a.f("timeout-s", 60);
  // groups "sms@list;sms@list"
  std::string gs = a.s("groups");
  std::vector<std::vector<int>> gsms; std::vector<std::vector<uint32_t>> glist;
  size_t i = 0;
  while (i < gs.size()) {
    size_t j = gs.find(';', i); if (j == std::string::npos) j = gs.size();
    std::string g = gs.substr(i, j - i); size_t at = g.find('@');
    if (at == std::string::npos) { fprintf(stderr, "group needs sms@list\n"); return 1; }
    gsms.push_back(parse_list(g.substr(0, at))); glist.push_back(read_list(g.substr(at + 1)));
    i = j + 1;
  }
  const int G = (int)gsms.size();
  BwP p = {};
  std::vector<signed char> grp(MAXSM, -1);
  std::vector<uint32_t> all;
  std::vector<int> allsms;
  int units = 0;
  for (int g = 0; g < G; ++g) {
    for (int s : gsms[g]) { grp[s] = (signed char)g; allsms.push_back(s); }
    p.gstart[g] = (int)all.size(); p.gcount[g] = (int)glist[g].size(); p.gunits[g] = (int)gsms[g].size() * per_sm;
    p.unit_base[g] = units; units += p.gunits[g];
    { bool c = true; for (size_t k = 1; k < glist[g].size() && c; ++k) c = glist[g][k] == glist[g][k - 1] + 1;
      p.gcontig[g] = c && !glist[g].empty() && !a.i("nocontig", 0); p.gfirst[g] = glist[g].empty() ? 0 : glist[g][0]; }
    for (uint32_t x : glist[g]) { if ((uint64_t)x * B + B > g_elems * 4) { fprintf(stderr, "block %u beyond buffer\n", x); return 1; } all.push_back(x); }
    if (!shared && p.gcount[g] < p.gunits[g]) { fprintf(stderr, "group %d: fewer blocks than units\n", g); return 1; }
  }
  uint32_t* d_list; CK(cudaMalloc(&d_list, all.size() * 4)); CK(cudaMemcpy(d_list, all.data(), all.size() * 4, cudaMemcpyHostToDevice));
  signed char* d_grp; CK(cudaMalloc(&d_grp, MAXSM)); CK(cudaMemcpy(d_grp, grp.data(), MAXSM, cudaMemcpyHostToDevice));
  int *d_claim, *d_gctr, *d_usm; unsigned* d_sums; unsigned long long* d_t; long long* d_cyc;
  CK(cudaMalloc(&d_claim, MAXSM * 4)); CK(cudaMalloc(&d_gctr, MAXG * 4)); CK(cudaMalloc(&d_usm, units * 4));
  CK(cudaMalloc(&d_sums, units * 4)); CK(cudaMalloc(&d_t, units * 16)); CK(cudaMalloc(&d_cyc, units * 8));
  p.buf = (const char*)g_buf; p.list = d_list; p.group = d_grp; p.per_sm = per_sm; p.B = B; p.passes = passes;
  p.shared = shared; p.write = write; p.ldmode = ldmode; p.claim = d_claim; p.gctr = d_gctr; p.sums = d_sums; p.t = d_t; p.cyc = d_cyc; p.unit_sm = d_usm;
  const u128 mask = mask_of_sms(allsms);
  const int grid = u128_popc(mask) * 2 * per_sm * 2;
  // expected sums per unit (reads)
  std::vector<uint32_t> expect(units, 0);
  std::vector<double> ubytes(units, 0);
  for (int g = 0; g < G; ++g) {
    const int bpr = 1;
    const int per = (shared ? p.gcount[g] : p.gcount[g] / p.gunits[g]) / bpr * bpr;
    for (int u = 0; u < p.gunits[g]; ++u) {
      uint32_t s = 0;
      const uint64_t W = B / 4;
      for (int k = 0; k < per; ++k) {
        const uint64_t w0 = (uint64_t)all[p.gstart[g] + (shared ? 0 : u * ((shared ? p.gcount[g] : p.gcount[g] / p.gunits[g]))) + k] * W;
        s += (uint32_t)(W * w0 + W * (W - 1) / 2);
      }
      expect[p.unit_base[g] + u] = s * (uint32_t)passes;
      ubytes[p.unit_base[g] + u] = (double)per * B * passes;  // rounded per (multiple of blocks per row)
    }
  }
  std::vector<double> agg, mhz; std::vector<std::vector<double>> gagg(G);
  int bad = 0; std::set<int> used;
  std::vector<unsigned> hs(units); std::vector<unsigned long long> ht(2 * units); std::vector<long long> hc(units); std::vector<int> husm(units);
  for (int r = 0; r <= reps; ++r) {
    CK(cudaMemset(d_claim, 0, MAXSM * 4)); CK(cudaMemset(d_gctr, 0, MAXG * 4)); CK(cudaMemset(d_sums, 0, units * 4));
    CK(cudaMemset(d_usm, 0xff, units * 4));
    tpc_next_enable(mask);
    if (unroll == 8) k_bw<8, 1><<<grid, 1024>>>(p); else k_bw<4, 2><<<grid, 1024>>>(p);
    CK(cudaGetLastError());
    wait_dev("bw");
    CK(cudaMemcpy(hs.data(), d_sums, units * 4, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(ht.data(), d_t, units * 16, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(hc.data(), d_cyc, units * 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(husm.data(), d_usm, units * 4, cudaMemcpyDeviceToHost));
    if (r == 0) continue;
    unsigned long long t0 = ~0ull, t1 = 0; double bytes = 0, cyc = 0, ns = 0;
    std::vector<unsigned long long> g0(G, ~0ull), g1(G, 0); std::vector<double> gb(G, 0);
    for (int g = 0; g < G; ++g)
      for (int u = 0; u < p.gunits[g]; ++u) {
        const int x = p.unit_base[g] + u;
        if (husm[x] < 0) { ++bad; continue; }
        used.insert(husm[x]);
        if (!write && hs[x] != expect[x]) ++bad;
        t0 = std::min(t0, ht[2 * x]); t1 = std::max(t1, ht[2 * x + 1]); bytes += ubytes[x];
        g0[g] = std::min(g0[g], ht[2 * x]); g1[g] = std::max(g1[g], ht[2 * x + 1]); gb[g] += ubytes[x];
        cyc += hc[x]; ns += ht[2 * x + 1] - ht[2 * x];
      }
    agg.push_back(bytes / (t1 - t0)); mhz.push_back(1000.0 * cyc / ns);
    for (int g = 0; g < G; ++g) gagg[g].push_back(gb[g] / (g1[g] - g0[g]));
  }
  const double m = median(agg), clkm = median(mhz);
  printf("bw unroll=%d groups=%d sms=%zu units=%d block=%d per_sm=%d passes=%d shared=%d write=%d ldmode=%d reps=%d check=%s bad=%d used_sms=%zu "
         "gbs=%.1f gbs_range=%.1f-%.1f clock_mhz=%.0f bytes_per_clk_chip=%.1f per_sm_gbs=%.1f",
         unroll, G, allsms.size(), units, B, per_sm, passes, shared, write, ldmode, reps, bad ? "FAIL" : "ok", bad, used.size(), m,
         *std::min_element(agg.begin(), agg.end()), *std::max_element(agg.begin(), agg.end()), clkm, m * 1e3 / clkm, m / allsms.size());
  for (int g = 0; g < G; ++g) printf(" g%d_gbs=%.1f", g, median(gagg[g]));
  printf("\n");
  return 0;
}

// ------------------------------------------------------------------ mix
static int cmd_mix(Args& a) {
  load_map(a.s("map"));
  alloc_buf(a.f("gib", 24));
  const int sm = (int)a.i("sm", 0), tma = (int)a.i("tma", 0), D = (int)a.i("depth", 6), tile = (int)a.i("tile-kb", 32) * 1024;
  const int ld = (int)a.i("ld", 0), smr = (int)a.i("smr", 0), B = (int)a.i("block", tile ? tile : 4096);
  const int ld2 = (int)a.i("second-ld", 0), carve = (int)a.i("carveout", -1), reps = (int)a.i("reps", 3);
  const double dur = a.f("dur-ms", 20);
  if (tma && B != tile) { fprintf(stderr, "with TMA the block size must equal the tile size\n"); return 1; }
  std::vector<uint32_t> list = read_list(a.s("list", ("all:" + std::to_string((unsigned long long)(g_elems * 4 / B) - (1ull << 30) / B)).c_str()));
  uint32_t* d_list; CK(cudaMalloc(&d_list, list.size() * 4)); CK(cudaMemcpy(d_list, list.data(), list.size() * 4, cudaMemcpyHostToDevice));
  const size_t smem = (tma || smr) ? 1024 + (size_t)tma * D * tile + (size_t)smr * 16384 : 0;
  CK(cudaFuncSetAttribute(k_mix, cudaFuncAttributeMaxDynamicSharedMemorySize, 224 * 1024));
  if (carve >= 0) CK(cudaFuncSetAttribute(k_mix, cudaFuncAttributePreferredSharedMemoryCarveout, carve));
  if (smem > 224 * 1024) { printf("SKIP smem %zu\n", smem); return 0; }
  auto mk = [&](MixP& p, int t_, int l_, int s_) {
    p = {}; p.buf = (const char*)g_buf; p.list = d_list; p.nlist = (int)list.size(); p.B = B; p.target = sm;
    p.tma = t_; p.depth = D; p.tile = tile; p.ld = l_; p.smr = s_; p.dur_ns = (unsigned long long)(dur * 1e6);
    CK(cudaMalloc(&p.bytes, 24)); CK(cudaMalloc(&p.tt, 48)); CK(cudaMalloc(&p.errs, 12)); CK(cudaMalloc(&p.cyc, 8)); CK(cudaMalloc(&p.gns, 8)); CK(cudaMalloc(&p.claim, 4));
  };
  MixP p1, p2; mk(p1, tma, ld, smr); if (ld2) mk(p2, 0, ld2, 0);
  cudaStream_t s1, s2; CK(cudaStreamCreateWithFlags(&s1, cudaStreamNonBlocking)); CK(cudaStreamCreateWithFlags(&s2, cudaStreamNonBlocking));
  const u128 mask = mask_of_sms({sm});
  std::vector<double> r[4]; double mhz = 0; unsigned errs[3] = {0, 0, 0};
  for (int rep = 0; rep <= reps; ++rep) {
    for (MixP* p : {&p1, &p2}) {
      if (p == &p2 && !ld2) continue;
      std::vector<unsigned long long> init = {~0ull, 0, ~0ull, 0, ~0ull, 0};
      CK(cudaMemset(p->bytes, 0, 24)); CK(cudaMemset(p->errs, 0, 12)); CK(cudaMemset(p->claim, 0, 4)); CK(cudaMemcpy(p->tt, init.data(), 48, cudaMemcpyHostToDevice));
    }
    const int thr1 = 32 * (tma + ld + smr);
    tpc_next_enable(mask);
    k_mix<<<2, thr1, smem, s1>>>(p1);  // smem > half the SM when TMA: one per SM
    if (ld2) { tpc_next_enable(mask); k_mix<<<8, 32 * ld2, 0, s2>>>(p2); }
    CK(cudaStreamSynchronize(s1)); CK(cudaStreamSynchronize(s2));
    CK(cudaGetLastError());
    if (rep == 0) continue;
    unsigned long long b[3], tt[6]; unsigned e[3]; long long cyc; unsigned long long gns;
    CK(cudaMemcpy(b, p1.bytes, 24, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(tt, p1.tt, 48, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(e, p1.errs, 12, cudaMemcpyDeviceToHost));
    for (int k = 0; k < 2; ++k) errs[k] += e[k];
    for (int k = 0; k < 3; ++k) r[k].push_back(b[k] ? (double)b[k] / (tt[2 * k + 1] - tt[2 * k]) : 0);
    if (ld) { CK(cudaMemcpy(&cyc, p1.cyc, 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&gns, p1.gns, 8, cudaMemcpyDeviceToHost)); mhz = 1000.0 * cyc / gns; }
    if (ld2) {
      CK(cudaMemcpy(b, p2.bytes, 24, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(tt, p2.tt, 48, cudaMemcpyDeviceToHost));
      CK(cudaMemcpy(e, p2.errs, 12, cudaMemcpyDeviceToHost)); errs[1] += e[1];
      r[3].push_back((double)b[1] / (tt[3] - tt[2]));
      if (!ld) { CK(cudaMemcpy(&cyc, p2.cyc, 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(&gns, p2.gns, 8, cudaMemcpyDeviceToHost)); mhz = 1000.0 * cyc / gns; }
    }
  }
  const double t = median(r[0]), l = median(r[1]), s = median(r[2]), l2 = median(r[3]);
  printf("mix sm=%d tma=%d depth=%d tile_kb=%d ld=%d smr=%d second_ld=%d block=%d list=%zu carveout=%d dur_ms=%.0f reps=%d "
         "tma_errs=%u ld_errs=%u check=%s tma_gbs=%.1f ld_gbs=%.1f smr_gbs=%.1f ld2_gbs=%.1f gmem_sum_gbs=%.1f clock_mhz=%.0f\n",
         sm, tma, D, tile / 1024, ld, smr, ld2, B, list.size(), carve, dur, reps, errs[0], errs[1], (errs[0] || errs[1]) ? "FAIL" : "ok",
         t, l, s, l2, t + l + l2, mhz);
  return 0;
}


// ------------------------------------------------------------------ tw (fixed window, many SMs)
static int cmd_tw(Args& a) {
  load_map(a.s("map"));
  alloc_buf(a.f("gib", 24));
  const int tma = (int)a.i("tma", 0), D = (int)a.i("depth", 6), tile = (int)a.i("tile-kb", 32) * 1024, ld = (int)a.i("ld", 0);
  const int B = (int)a.i("block", tma ? tile : 4096);
  const double dur = a.f("dur-ms", 1000), mbpu = a.f("mb-per-unit", 64);
  const int reps = (int)a.i("reps", 1), carve = (int)a.i("carveout", -1), smr = (int)a.i("smr", 0), shared = (int)a.i("shared", 0);
  const int write = (int)a.i("write", 0), ld2 = (int)a.i("second-ld", 0);
  g_timeout = a.f("timeout-s", 120);
  if (tma && B != tile) { fprintf(stderr, "TMA needs block == tile\n"); return 1; }
  // groups "sms@list;sms@list"; list "all" = disjoint contiguous portions of mb-per-unit each
  std::string gs = a.s("groups");
  std::vector<std::vector<int>> gsms; std::vector<std::string> gl;
  size_t i = 0;
  while (i < gs.size()) {
    size_t j = gs.find(';', i); if (j == std::string::npos) j = gs.size();
    std::string g = gs.substr(i, j - i); size_t at = g.find('@');
    gsms.push_back(parse_list(g.substr(0, at))); gl.push_back(at == std::string::npos ? "all" : g.substr(at + 1));
    i = j + 1;
  }
  const int G = (int)gsms.size();
  TwP p = {};
  std::vector<signed char> grp(MAXSM, -1); std::vector<uint32_t> all; std::vector<int> allsms;
  int units = 0; uint32_t next_contig = 0;
  const uint32_t per_unit_blocks = std::max<uint32_t>(2, (uint32_t)(mbpu * (1 << 20) / B));
  for (int g = 0; g < G; ++g) {
    for (int s : gsms[g]) { grp[s] = (signed char)g; allsms.push_back(s); }
    p.gunits[g] = (int)gsms[g].size(); p.unit_base[g] = units; units += p.gunits[g];
    p.gstart[g] = (int)all.size();
    if (gl[g] == "all") {
      for (uint32_t k = 0; k < per_unit_blocks * (shared ? 1 : p.gunits[g]); ++k) all.push_back(next_contig++);
      if (shared) for (uint32_t k = 0; k < per_unit_blocks * (p.gunits[g] - 1); ++k) all.push_back(all[p.gstart[g] + k % per_unit_blocks]);
    } else {
      std::vector<uint32_t> L = read_list(gl[g]);
      // the group's units take disjoint portions of mb-per-unit each from the list
      const size_t need = (size_t)per_unit_blocks * p.gunits[g];
      if (L.size() < need) { fprintf(stderr, "list %s too short (%zu < %zu)\n", gl[g].c_str(), L.size(), need); return 1; }
      all.insert(all.end(), L.begin(), L.begin() + need);
    }
    p.gcount[g] = (int)all.size() - p.gstart[g];
  }
  if ((uint64_t)next_contig * B > g_elems * 4) { fprintf(stderr, "contiguous portions exceed the buffer\n"); return 1; }
  uint32_t* d_list; CK(cudaMalloc(&d_list, all.size() * 4)); CK(cudaMemcpy(d_list, all.data(), all.size() * 4, cudaMemcpyHostToDevice));
  signed char* d_grp; CK(cudaMalloc(&d_grp, MAXSM)); CK(cudaMemcpy(d_grp, grp.data(), MAXSM, cudaMemcpyHostToDevice));
  int *d_claim, *d_gctr, *d_usm; unsigned long long *d_ub, *d_start; long long* d_cyc; unsigned* d_err;
  CK(cudaMalloc(&d_claim, MAXSM * 4)); CK(cudaMalloc(&d_gctr, MAXG * 4)); CK(cudaMalloc(&d_usm, units * 4));
  CK(cudaMalloc(&d_ub, units * 16)); CK(cudaMalloc(&d_start, 8)); CK(cudaMalloc(&d_cyc, units * 8)); CK(cudaMalloc(&d_err, 12));
  p.buf = (const char*)g_buf; p.list = d_list; p.B = B; p.group = d_grp; p.claim = d_claim; p.gctr = d_gctr;
  p.tma = tma; p.depth = D; p.tile = tile; p.ld = ld; p.dur_ns = (unsigned long long)(dur * 1e6); p.start = d_start;
  p.ub = d_ub; p.ucyc = d_cyc; p.usm = d_usm; p.errs = d_err; p.smr = smr; p.shared = shared; p.write = write;
  unsigned long long* d_smr; CK(cudaMalloc(&d_smr, units * 8)); p.usmr = d_smr;
  // optional second kernel on the same SMs: loads only, same window start
  TwP p2 = p; p2.tma = 0; p2.smr = 0; p2.ld = ld2;
  int *d_claim2, *d_gctr2, *d_usm2; unsigned long long *d_ub2, *d_smr2; long long* d_cyc2; unsigned* d_err2;
  CK(cudaMalloc(&d_claim2, MAXSM * 4)); CK(cudaMalloc(&d_gctr2, MAXG * 4)); CK(cudaMalloc(&d_usm2, units * 4)); CK(cudaMalloc(&d_ub2, units * 16));
  CK(cudaMalloc(&d_smr2, units * 8)); CK(cudaMalloc(&d_cyc2, units * 8)); CK(cudaMalloc(&d_err2, 12));
  p2.claim = d_claim2; p2.gctr = d_gctr2; p2.usm = d_usm2; p2.ub = d_ub2; p2.usmr = d_smr2; p2.ucyc = d_cyc2; p2.errs = d_err2;
  cudaStream_t st1, st2; CK(cudaStreamCreateWithFlags(&st1, cudaStreamNonBlocking)); CK(cudaStreamCreateWithFlags(&st2, cudaStreamNonBlocking));
  const size_t smem = (tma || smr) ? 1024 + (size_t)tma * D * tile + (size_t)smr * 16384 : 0;
  if (smem > 224 * 1024) { printf("SKIP smem %zu\n", smem); return 0; }
  CK(cudaFuncSetAttribute(k_tw, cudaFuncAttributeMaxDynamicSharedMemorySize, 224 * 1024));
  if (carve >= 0) CK(cudaFuncSetAttribute(k_tw, cudaFuncAttributePreferredSharedMemoryCarveout, carve));
  const u128 mask = mask_of_sms(allsms);
  const int threads = 32 * (tma + ld + smr);
  // one working CTA per SM; with no TMA (small smem) launch 4 per TPC SM so every SM gets one
  const int grid = u128_popc(mask) * 2 * ((tma || smr) ? 1 : 4);
  std::vector<unsigned long long> hub(2 * units); std::vector<long long> hc(units); std::vector<int> husm(units); unsigned herr[3];
  for (int r = 0; r < reps; ++r) {
    CK(cudaMemset(d_claim, 0, MAXSM * 4)); CK(cudaMemset(d_gctr, 0, MAXG * 4)); CK(cudaMemset(d_ub, 0, units * 16));
    CK(cudaMemset(d_start, 0, 8)); CK(cudaMemset(d_err, 0, 12)); CK(cudaMemset(d_usm, 0xff, units * 4)); CK(cudaMemset(d_smr, 0, units * 8));
    CK(cudaMemset(d_claim2, 0, MAXSM * 4)); CK(cudaMemset(d_gctr2, 0, MAXG * 4)); CK(cudaMemset(d_ub2, 0, units * 16)); CK(cudaMemset(d_err2, 0, 12));
    CK(cudaDeviceSynchronize());
    tpc_next_enable(mask);
    k_tw<<<grid, threads, smem, st1>>>(p);
    if (ld2) { tpc_next_enable(mask); k_tw<<<u128_popc(mask) * 2 * 4, 32 * ld2, 0, st2>>>(p2); }
    CK(cudaGetLastError()); CK(cudaStreamSynchronize(st1)); CK(cudaStreamSynchronize(st2));
    CK(cudaMemcpy(hub.data(), d_ub, units * 16, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(hc.data(), d_cyc, units * 8, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(husm.data(), d_usm, units * 4, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(herr, d_err, 12, cudaMemcpyDeviceToHost));
    std::vector<unsigned long long> hub2(2 * units, 0), hsm(units, 0);
    CK(cudaMemcpy(hsm.data(), d_smr, units * 8, cudaMemcpyDeviceToHost));
    if (ld2) CK(cudaMemcpy(hub2.data(), d_ub2, units * 16, cudaMemcpyDeviceToHost));
    double tot = 0, tt = 0, tl = 0, mhz = 0, tl2 = 0, tsm = 0; int miss = 0;
    std::vector<double> gsum(G, 0);
    printf("tw rep=%d tma=%d depth=%d tile_kb=%d ld=%d block=%d units=%d dur_ms=%.0f mb_per_unit=%.1f carveout=%d", r, tma, D, tile / 1024, ld, B, units, dur,
           per_unit_blocks * (double)B / (1 << 20), carve);
    std::string per;
    for (int g = 0; g < G; ++g)
      for (int k = 0; k < p.gunits[g]; ++k) {
        const int x = p.unit_base[g] + k;
        if (husm[x] < 0) { ++miss; continue; }
        const double rt = hub[2 * x] / (dur * 1e6), rl = hub[2 * x + 1] / (dur * 1e6), r2 = hub2[2 * x + 1] / (dur * 1e6);
        tt += rt; tl += rl; tl2 += r2; tsm += hsm[x] / (dur * 1e6); tot += rt + rl + r2; gsum[g] += rt + rl + r2; mhz += hc[x] / (dur * 1e3);
        char b[64]; snprintf(b, sizeof b, " %d:%.1f", husm[x], rt + rl + r2); per += b;
      }
    printf(" smr=%d shared=%d write=%d second_ld=%d missing_units=%d tma_errs=%u ld_errs=%u check=%s total_gbs=%.1f tma_gbs=%.1f ld_gbs=%.1f ld2_gbs=%.1f smr_gbs=%.1f clock_mhz_avg=%.0f",
           smr, shared, write, ld2, miss, herr[0], herr[1], (miss || herr[0] || (herr[1] && !write)) ? "FAIL" : "ok", tot, tt, tl, tl2, tsm, mhz / std::max(1, units - miss));
    for (int g = 0; g < G; ++g) printf(" g%d_gbs=%.1f", g, gsum[g]);
    printf("\n  per_sm_gbs:%s\n", per.c_str());
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: l2s_lab lat|bw|mix ...\n"); return 1; }
  Args a(argc, argv);
  if (!strcmp(argv[1], "lat")) return cmd_lat(a);
  if (!strcmp(argv[1], "bw")) return cmd_bw(a);
  if (!strcmp(argv[1], "mix")) return cmd_mix(a);
  if (!strcmp(argv[1], "tw")) return cmd_tw(a);
  return 1;
}
