// Bystander-effect laboratory for the H200 (one process: page classification,
// NVML sampling, many scenarios x trials, interleaved).
//
//   bys_lab check --map F --gpc F              TPC-bit -> SM map check, side check
//   bys_lab run   --map F --gpc F --plan P [--gib 24] [--nvml-ms 10] [--persist 1]
//                 [--nomask 0] [--hist FILE] [--gap-ms 100] [--only NAME]
//
// A plan is a list of scenarios; a scenario is a set of groups of SMs that run
// together in ONE kernel launch for a fixed window (every unit starts at the
// same globaltimer instant; only bytes completed inside the window count).
// Plan syntax (one item per line, '#' comments):
//   S name=<id> [trials=5] [dur=1000] [bin=1000]
//   G name=<id> sms=<list> mode=tma|ld|st|tmast|fma|lat|idle [data=contig|shared|home:A|home:B|gran:P:M1:V1[:M2:V2]]
//     [mb=64] [depth=6] [tile=32] [warps=32] [pace=GB/s per SM] [duty=on_us/period_us]
//     [lat=hit|miss|far] [side=A|B] [warm=<sm>] [latcap=N]
// Every unit (= one SM) of a group gets disjoint data (except data=shared).
// Placement: one TPC mask covering all groups' SMs; inside, each CTA reads
// %smid and takes the unit of that SM (one CTA per SM); the SM of every unit
// is written back and checked.
//
// Buffer layout (gib = 24): [0,8MB) probe zone (lat hit / far lines),
// [8MB,16MB) granule zone (slice-set tests), [16MB, 16GiB) read data,
// [16GiB, 18GiB) write area, [18GiB, 24GiB) cold area (lat miss walks, flush).
// a[i] = i (uint32) everywhere; readers check words.
#include "tpc_smctrl.h"
#include <cuda_runtime.h>
#include <nvml.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)
#define MAXSM 256
#define MAXG 12
#define NB 1024   // latency histogram bins of 8 cycles
#define BINW 8

enum { M_IDLE = 0, M_TMA, M_LD, M_ST, M_TMAST, M_FMA, M_LAT };
enum { L_HIT = 0, L_MISS, L_FAR };

__device__ __forceinline__ uint32_t sm_id() { uint32_t r; asm volatile("mov.u32 %0, %%smid;" : "=r"(r)); return r; }
__device__ __forceinline__ unsigned long long gtimer() { unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t; }
__device__ __forceinline__ long long clk() { long long c; asm volatile("mov.u64 %0, %%clock64;" : "=l"(c) :: "memory"); return c; }
__device__ __forceinline__ uint32_t smem_u32(const void* p) { return (uint32_t)__cvta_generic_to_shared(p); }
__device__ __forceinline__ void mbar_init(uint32_t bar, uint32_t count) { asm volatile("mbarrier.init.shared::cta.b64 [%0], %1;" :: "r"(bar), "r"(count) : "memory"); }
__device__ __forceinline__ void mbar_arrive_expect_tx(uint32_t bar, uint32_t tx) {
  asm volatile("mbarrier.arrive.expect_tx.shared::cta.b64 _, [%0], %1;" :: "r"(bar), "r"(tx) : "memory");
}
__device__ __forceinline__ void mbar_wait(uint32_t bar, uint32_t parity) {
  uint32_t ok = 0;
  for (;;) {
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
__device__ __forceinline__ void bulk_s2g(const void* dst, uint32_t src, uint32_t bytes) {
  asm volatile("cp.async.bulk.global.shared::cta.bulk_group [%0], [%1], %2;" :: "l"(dst), "r"(src), "r"(bytes) : "memory");
  asm volatile("cp.async.bulk.commit_group;" ::: "memory");
}
__device__ __forceinline__ void bulk_wait(int n) {
  switch (n) {
    case 0: asm volatile("cp.async.bulk.wait_group 0;" ::: "memory"); break;
    case 1: asm volatile("cp.async.bulk.wait_group 1;" ::: "memory"); break;
    case 2: asm volatile("cp.async.bulk.wait_group 2;" ::: "memory"); break;
    case 3: asm volatile("cp.async.bulk.wait_group 3;" ::: "memory"); break;
    case 4: asm volatile("cp.async.bulk.wait_group 4;" ::: "memory"); break;
    case 5: asm volatile("cp.async.bulk.wait_group 5;" ::: "memory"); break;
    case 6: asm volatile("cp.async.bulk.wait_group 6;" ::: "memory"); break;
    default: asm volatile("cp.async.bulk.wait_group 7;" ::: "memory"); break;
  }
}

// ------------------------------------------------------------------ simple kernels
__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) a[i] = (uint32_t)i;
}
__global__ void k_flush(const uint4* a, size_t n, unsigned* out) {
  uint32_t s = 0;
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) { uint4 v = __ldcg(a + i); s += v.x ^ v.w; }
  if (s == 0x9e3779b9u) out[0] = s;
}
// one CTA on SM target reads the lines (byte offsets / 128 in lines[]) with ld.global.cg
__global__ void k_touch(const char* buf, const uint32_t* lines, int n, int target, unsigned* claim) {
  __shared__ int ok;
  if (threadIdx.x == 0) ok = (int)sm_id() == target && atomicAdd(claim, 1) == 0;
  __syncthreads();
  if (!ok) return;
  uint32_t s = 0;
  for (int i = threadIdx.x; i < n; i += blockDim.x) { uint32_t v; asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(v) : "l"(buf + (uint64_t)lines[i] * 128) : "memory"); s += v; }
  if (s == 0x12345u) claim[1] = s;
}
// one thread on SM target times dependent loads over lines[] (line index = byte offset / 128)
__global__ void k_lat_list(const char* buf, const uint32_t* lines, int n, int target, unsigned short* out, unsigned* errs, unsigned* claim) {
  if ((int)sm_id() != target || threadIdx.x != 0 || atomicAdd(claim, 1) != 0) return;
  unsigned e = 0;
  uint64_t a_cur = (uint64_t)lines[0] * 128, a_nxt = n > 1 ? (uint64_t)lines[1] * 128 : 0;
  const char* p = buf + a_cur;
  for (int i = 0; i < n; ++i) {
    const uint64_t a_n2 = i + 2 < n ? (uint64_t)__ldg(lines + i + 2) * 128 : 0;
    long long c0, c1; uint32_t v;
    asm volatile("mov.u64 %0, %%clock64;" : "=l"(c0) :: "memory");
    asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(v) : "l"(p) : "memory");
    const uint32_t ex = (uint32_t)(a_cur / 4);
    const char* nx = buf + a_nxt + (v ^ ex);
    asm volatile("{\n\t.reg .u64 q;\n\tmov.u64 q, %1;\n\tmov.u64 %0, %%clock64;\n\t}" : "=l"(c1) : "l"(nx) : "memory");
    const long long d = c1 - c0;
    out[i] = (unsigned short)(d > 65535 ? 65535 : d);
    e += v != ex;
    p = nx; a_cur = a_nxt; a_nxt = a_n2;
  }
  errs[0] = e;
}
__global__ void k_where(int* out) { if (threadIdx.x == 0) out[blockIdx.x] = (int)sm_id(); }

// ------------------------------------------------------------------ main kernel
struct GP {
  int mode, depth, tile, warps, B, lat, side, contig, shared;
  unsigned long long pace_q;            // ns per quantum (TMA: per tile; LD/ST: per 4 KB per warp; TMAST: per tile)
  unsigned long long on_ns, per_ns;     // duty
  unsigned long long latcap;
};
struct KP {
  const char* buf;
  GP g[MAXG];
  const int* sm2unit;                 // [MAXSM]
  const int* ugrp;                    // unit -> group
  const unsigned long long* ubase;    // byte offset of the unit's contiguous region (contig / lat)
  const unsigned long long* ubytes;   // unit data bytes (lat far: number of lines)
  const unsigned long long* uloff;    // offset of the unit's block list in list[]
  const uint32_t* list;
  const uint8_t* home;                // per 4 KB block: 0 = side A, 1 = side B, 2 = unknown
  int* claim; unsigned long long* start; unsigned long long dur_ns;
  unsigned long long* ob; long long* ocyc; unsigned* oerr; int* osm; unsigned long long* ot;
  unsigned long long* tl; int nbins; unsigned long long bin_ns;
  unsigned* hist; unsigned long long* lsum;   // hist [u][2][NB]; lsum [u][4]: sum0, n0, sum1, n1
};

__device__ __forceinline__ void tl_add(const KP& p, int u, unsigned long long now, unsigned long long t0, int& cb, unsigned long long& cbytes, unsigned long long add) {
  int b = (int)((now - t0) / p.bin_ns); if (b >= p.nbins) b = p.nbins - 1;
  if (b != cb) { if (cb >= 0 && cbytes) atomicAdd(&p.tl[(size_t)u * p.nbins + cb], cbytes); cb = b; cbytes = 0; }
  cbytes += add;
}
__device__ __forceinline__ void tl_flush(const KP& p, int u, int cb, unsigned long long cbytes) {
  if (cb >= 0 && cbytes) atomicAdd(&p.tl[(size_t)u * p.nbins + cb], cbytes);
}
// wait until issue k is allowed (pacing and duty); returns false if the window ended
__device__ __forceinline__ bool gate(const GP& g, unsigned long long t0, unsigned long long tend, unsigned long long k) {
  if (!g.pace_q && !g.per_ns) return gtimer() < tend;
  for (;;) {
    const unsigned long long now = gtimer();
    if (now >= tend) return false;
    bool ok = true;
    if (g.pace_q && now < t0 + k * g.pace_q) ok = false;
    if (g.per_ns && (now - t0) % g.per_ns >= g.on_ns) ok = false;
    if (ok) return true;
    __nanosleep(200);
  }
}

__global__ void __launch_bounds__(1024, 1) k_bys(KP p) {
  extern __shared__ __align__(1024) unsigned char smem[];
  __shared__ int s_u; __shared__ unsigned long long s_t0;
  __shared__ unsigned s_hist[2 * NB];
  if (threadIdx.x == 0) {
    const int sm = (int)sm_id();
    int u = p.sm2unit[sm];
    if (u >= 0 && atomicAdd(&p.claim[sm], 1) != 0) u = -1;
    s_u = u;
    if (u >= 0) {
      p.osm[u] = sm;
      unsigned long long st = atomicCAS(p.start, 0ull, gtimer() + 300000ull);
      if (st == 0) st = *((volatile unsigned long long*)p.start);
      s_t0 = st;
    }
  }
  __syncthreads();
  const int u = s_u;
  if (u < 0) return;
  const GP& g = p.g[p.ugrp[u]];
  const unsigned long long t0 = s_t0, tend = t0 + p.dur_ns;
  const int warp = threadIdx.x / 32, lane = threadIdx.x % 32;
  const char* buf = p.buf;
  const unsigned long long base = p.ubase[u], ubytes = p.ubytes[u];
  const uint32_t* L = p.list + p.uloff[u];

  if (g.mode == M_TMA) {
    if (threadIdx.x != 0) return;
    const uint32_t bars = smem_u32(smem), slots = bars + 1024;
    const int D = g.depth; const uint32_t tile = (uint32_t)g.tile, sub = g.contig ? tile : (uint32_t)g.B, nsub = tile / sub;
    for (int s = 0; s < D; ++s) mbar_init(bars + 8 * s, 1);
    asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
    const unsigned long long ntiles = ubytes / tile;
    uint32_t ex0[8], ex1[8];
    unsigned long long k = 0, nb = 0; unsigned e = 0; int cb = -1; unsigned long long cby = 0;
    while (gtimer() < t0) {}
    const long long c0 = clk();
    auto issue = [&](int slot) -> bool {
      if (!gate(g, t0, tend, k)) return false;
      const uint32_t b = bars + 8 * slot, dst = slots + (uint32_t)slot * tile;
      const unsigned long long ti = k % ntiles;
      mbar_arrive_expect_tx(b, tile);
      if (g.contig) {
        const unsigned long long off = base + ti * tile;
        bulk_g2s(dst, buf + off, tile, b);
        ex0[slot] = (uint32_t)(off / 4); ex1[slot] = (uint32_t)((off + tile) / 4 - 1);
      } else {
        for (uint32_t m = 0; m < nsub; ++m) bulk_g2s(dst + m * sub, buf + (uint64_t)__ldg(L + ti * nsub + m) * sub, sub, b);
        ex0[slot] = (uint32_t)((uint64_t)__ldg(L + ti * nsub) * sub / 4);
        ex1[slot] = (uint32_t)(((uint64_t)__ldg(L + ti * nsub + nsub - 1) * sub + sub) / 4 - 1);
      }
      ++k;
      return true;
    };
    int inflight = 0;
    bool live[8];
    for (int n = 0; n < D; ++n) { live[n] = issue(n); inflight += live[n]; }
    int sl = 0; uint32_t ph = 0;
    while (inflight) {
      if (live[sl]) {
        const uint32_t b = bars + 8 * sl;
        mbar_wait(b, ph);
        const unsigned long long now = gtimer();
        const uint32_t* t = reinterpret_cast<const uint32_t*>(smem + 1024 + (size_t)sl * tile);
        e += (t[0] != ex0[sl]) + (t[tile / 4 - 1] != ex1[sl]);
        --inflight;
        if (now <= tend) { nb += tile; tl_add(p, u, now, t0, cb, cby, tile); }
        live[sl] = issue(sl);
        inflight += live[sl];
      }
      if (++sl == D) { sl = 0; ph ^= 1; }
    }
    tl_flush(p, u, cb, cby);
    p.ob[u] = nb; p.oerr[u] = e; p.ocyc[u] = clk() - c0; p.ot[2 * u] = t0; p.ot[2 * u + 1] = gtimer();
    return;
  }

  if (g.mode == M_TMAST) {
    if (threadIdx.x != 0) return;  // one tile of constant data, written to consecutive tiles of the unit's write region
    const uint32_t src = smem_u32(smem) + 1024; const uint32_t tile = (uint32_t)g.tile; const int D = g.depth;
    for (uint32_t i = 0; i < tile / 4; ++i) reinterpret_cast<uint32_t*>(smem + 1024)[i] = i;
    asm volatile("fence.proxy.async.shared::cta;" ::: "memory");
    const unsigned long long ntiles = ubytes / tile;
    unsigned long long k = 0, done = 0, nb = 0; int cb = -1; unsigned long long cby = 0;
    while (gtimer() < t0) {}
    const long long c0 = clk();
    for (;;) {
      if (!gate(g, t0, tend, k)) break;
      bulk_s2g(buf + base + (k % ntiles) * tile, src, tile); ++k;
      bulk_wait(D - 1);
      const unsigned long long now = gtimer();
      const unsigned long long c = k >= (unsigned long long)(D - 1) ? k - (D - 1) : 0;
      if (now <= tend && c > done) { nb += (c - done) * tile; tl_add(p, u, now, t0, cb, cby, (c - done) * tile); }
      done = c > done ? c : done;
    }
    bulk_wait(0);
    tl_flush(p, u, cb, cby);
    p.ob[u] = nb; p.oerr[u] = 0; p.ocyc[u] = clk() - c0; p.ot[2 * u] = t0; p.ot[2 * u + 1] = gtimer();
    return;
  }

  if (g.mode == M_LD || g.mode == M_ST) {
    const int W = g.warps;
    if (warp >= W) return;
    const unsigned long long nch = ubytes / 4096;
    const uint32_t B = (uint32_t)g.B;
    unsigned long long c = (unsigned long long)warp % nch, it = 0, nb = 0;
    uint32_t s = 0; unsigned err = 0; int cb = -1; unsigned long long cby = 0;
    auto addr = [&](unsigned long long ch, int k) -> unsigned long long {
      const unsigned long long o = ch * 4096 + (unsigned long long)k * 512 + lane * 16;
      return g.contig ? base + o : (unsigned long long)__ldg(L + o / B) * B + o % B;
    };
    while (gtimer() < t0) {}
    const long long c0 = clk();
    for (;;) {
      if (!gate(g, t0, tend, it)) break;
      uint32_t a[8];  // byte offset / 16 of each lane's 16 B (fits 32 bits for buffers < 64 GiB); words compare mod 2^32
#pragma unroll
      for (int k = 0; k < 8; ++k) a[k] = (uint32_t)(addr(c, k) >> 4);
      uint4 v[8];
      if (g.mode == M_ST) {
#pragma unroll
        for (int k = 0; k < 8; ++k) { const uint32_t w = a[k] * 4; __stcg(reinterpret_cast<uint4*>(const_cast<char*>(buf) + (uint64_t)a[k] * 16), make_uint4(w, w + 1, w + 2, w + 3)); v[k] = make_uint4(w, 0, 0, 0); }
      } else {
#pragma unroll
        for (int k = 0; k < 8; ++k) v[k] = __ldcg(reinterpret_cast<const uint4*>(buf + (uint64_t)a[k] * 16));
      }
      c += W; while (c >= nch) c -= nch;
#pragma unroll
      for (int k = 0; k < 8; ++k) { s += v[k].y; err += v[k].x != a[k] * 4; }
      const unsigned long long now = gtimer();
      if (now > tend) break;
      ++it; nb += 4096;
      if (lane == 0) tl_add(p, u, now, t0, cb, cby, 4096);
    }
    if (lane == 0) tl_flush(p, u, cb, cby);
    if (s == 0x31415u) p.oerr[MAXSM - 1] = s;
    if (lane == 0) atomicAdd(&p.ob[u], nb);
    if (err && g.mode == M_LD) atomicAdd(&p.oerr[u], err);
    if (warp == 0 && lane == 0) { p.ocyc[u] = clk() - c0; p.ot[2 * u] = t0; p.ot[2 * u + 1] = gtimer(); }
    return;
  }

  if (g.mode == M_FMA) {
    if (warp >= g.warps) return;
    uint32_t a0 = 0x3c003c00u ^ lane, a1 = 0x3c003c00u, a2 = 0x3c003c00u, a3 = 0x3c003c00u, b0 = 0x3c003c00u, b1 = 0x3c00u ^ warp;
    float d[8][4];
#pragma unroll
    for (int i = 0; i < 8; ++i) for (int j = 0; j < 4; ++j) d[i][j] = 0.f;
    unsigned long long n = 0;
    while (gtimer() < t0) {}
    const long long c0 = clk();
    while (gtimer() < tend) {
#pragma unroll
      for (int r = 0; r < 16; ++r)
#pragma unroll
        for (int i = 0; i < 8; ++i)
          asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
                       : "+f"(d[i][0]), "+f"(d[i][1]), "+f"(d[i][2]), "+f"(d[i][3])
                       : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
      n += 16 * 8;
    }
    float t = 0; for (int i = 0; i < 8; ++i) t += d[i][0] + d[i][1] + d[i][2] + d[i][3];
    if (t == 1.2345f) p.oerr[MAXSM - 1] = 1;
    if (lane == 0) atomicAdd(&p.ob[u], n);  // mma count (4096 flop each)
    if (warp == 0 && lane == 0) { p.ocyc[u] = clk() - c0; p.ot[2 * u] = t0; p.ot[2 * u + 1] = gtimer(); }
    return;
  }

  if (g.mode == M_LAT) {
    for (int i = threadIdx.x; i < 2 * NB; i += blockDim.x) s_hist[i] = 0;
    __syncthreads();
    if (threadIdx.x != 0) return;
    // address sequence: hit: base + (i % n)*128, recorded from the second pass; miss: base + i*4224; far: lines L[i]*128
    const unsigned long long n = g.lat == L_HIT ? ubytes / 128 : g.lat == L_MISS ? ubytes / 4224 : ubytes;
    const unsigned long long cap = g.lat == L_HIT ? (g.latcap ? g.latcap + n : ~0ull) : (g.latcap && g.latcap < n ? g.latcap : n);
    auto A = [&](unsigned long long i) -> unsigned long long {
      return g.lat == L_HIT ? base + (i % n) * 128 : g.lat == L_MISS ? base + i * 4224 : (unsigned long long)__ldg(L + i) * 128;
    };
    unsigned long long sum0 = 0, n0 = 0, sum1 = 0, n1 = 0; unsigned e = 0;
    unsigned long long a_cur = A(0), a_nxt = A(1);
    while (gtimer() < t0) {}
    const long long c0 = clk();
    const char* pp = buf + a_cur;
    unsigned long long i = 0;
    for (; i < cap; ++i) {
      const unsigned long long a_n2 = i + 2 < cap ? A(i + 2) : 0;
      long long q0, q1; uint32_t v;
      asm volatile("mov.u64 %0, %%clock64;" : "=l"(q0) :: "memory");
      asm volatile("ld.global.cg.u32 %0, [%1];" : "=r"(v) : "l"(pp) : "memory");
      const uint32_t ex = (uint32_t)(a_cur / 4);
      const char* nx = buf + a_nxt + (v ^ ex);
      asm volatile("{\n\t.reg .u64 q;\n\tmov.u64 q, %1;\n\tmov.u64 %0, %%clock64;\n\t}" : "=l"(q1) : "l"(nx) : "memory");
      const unsigned long long d = (unsigned long long)(q1 - q0);
      e += v != ex;
      if (g.lat != L_HIT || i >= n) {
        int cls = 0;
        if (g.lat == L_MISS) { const uint8_t h = p.home[a_cur >> 12]; cls = h == 2 ? -1 : (h == (uint8_t)g.side ? 0 : 1); }
        if (cls >= 0) {
          const unsigned bin = (unsigned)(d / BINW) < NB ? (unsigned)(d / BINW) : NB - 1;
          s_hist[cls * NB + bin]++;
          if (cls == 0) { sum0 += d; ++n0; } else { sum1 += d; ++n1; }
        }
      }
      pp = nx; a_cur = a_nxt; a_nxt = a_n2;
      if ((i & 15) == 15 && gtimer() > tend) { ++i; break; }
    }
    for (int k = 0; k < 2 * NB; ++k) p.hist[(size_t)u * 2 * NB + k] = s_hist[k];
    p.lsum[4 * u] = sum0; p.lsum[4 * u + 1] = n0; p.lsum[4 * u + 2] = sum1; p.lsum[4 * u + 3] = n1;
    p.ob[u] = (n0 + n1) * 128; p.oerr[u] = e; p.ocyc[u] = clk() - c0; p.ot[2 * u] = t0; p.ot[2 * u + 1] = gtimer();
    return;
  }
  // idle: just mark
  if (threadIdx.x == 0) { p.ocyc[u] = 0; p.ot[2 * u] = t0; p.ot[2 * u + 1] = t0; }
}

// ------------------------------------------------------------------ host: misc
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
static std::map<int, std::vector<int>> g_bit2sms;
static std::vector<std::vector<int>> g_gpc_sms;   // GPC -> SMs (from map + gpc file)
static int g_sm2gpc[MAXSM];
static void load_map(const std::string& f) {
  for (int i = 0; i < MAXSM; ++i) g_sm2bit[i] = -1;
  FILE* fp = fopen(f.c_str(), "r"); if (!fp) { fprintf(stderr, "cannot open map %s\n", f.c_str()); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, fp)) {
    int b; if (sscanf(line, "bit %d sms", &b) != 1) continue;
    char* q = strstr(line, "sms") + 3; int s, n;
    while (sscanf(q, "%d%n", &s, &n) == 1) { if (s >= 0 && s < MAXSM) { g_sm2bit[s] = b; g_bit2sms[b].push_back(s); } q += n; }
  }
  fclose(fp);
}
static void load_gpc(const std::string& f) {
  for (int i = 0; i < MAXSM; ++i) g_sm2gpc[i] = -1;
  FILE* fp = fopen(f.c_str(), "r"); if (!fp) { fprintf(stderr, "cannot open gpc file %s\n", f.c_str()); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, fp)) {
    int g; if (sscanf(line, "gpc %d bits", &g) != 1) continue;
    char* q = strstr(line, "bits") + 4; int b, n;
    if ((int)g_gpc_sms.size() <= g) g_gpc_sms.resize(g + 1);
    while (sscanf(q, "%d%n", &b, &n) == 1) { for (int s : g_bit2sms[b]) { g_gpc_sms[g].push_back(s); g_sm2gpc[s] = g; } q += n; }
  }
  fclose(fp);
}
static u128 mask_of_sms(const std::vector<int>& sms) {
  u128 m = 0;
  for (int s : sms) { if (s < 0 || s >= MAXSM || g_sm2bit[s] < 0) { fprintf(stderr, "SM %d not in map\n", s); exit(1); } m |= u128_bit(g_sm2bit[s]); }
  return m;
}
static double g_timeout = 60;
static bool wait_dev(cudaStream_t st, const char* what) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    cudaError_t e = cudaStreamQuery(st);
    if (e == cudaSuccess) return true;
    if (e != cudaErrorNotReady) { fprintf(stderr, "%s: %s\n", what, cudaGetErrorString(e)); exit(1); }
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > g_timeout) { printf("TIMEOUT %s\n", what); fflush(stdout); _exit(124); }
    usleep(100);
  }
}
static double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Args {
  std::map<std::string, std::string> kv;
  Args(int argc, char** argv) { for (int i = 2; i + 1 < argc; i += 2) { if (strncmp(argv[i], "--", 2)) { fprintf(stderr, "bad arg %s\n", argv[i]); exit(1); } kv[argv[i] + 2] = argv[i + 1]; } }
  std::string s(const char* k, const char* d = "") { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
  double f(const char* k, double d) { auto it = kv.find(k); return it == kv.end() ? d : atof(it->second.c_str()); }
  long long i(const char* k, long long d) { auto it = kv.find(k); return it == kv.end() ? d : atoll(it->second.c_str()); }
};
template <class T> static T med(std::vector<T> v) { if (v.empty()) return T(0); std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
static double medd(std::vector<double> v) {
  if (v.empty()) return NAN; std::sort(v.begin(), v.end()); size_t n = v.size(); return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// ------------------------------------------------------------------ NVML sampler
struct Samp { double t; unsigned sm, mem, temp; double pw; unsigned long long rs; };
static std::vector<Samp> g_samps; static std::mutex g_smu; static std::atomic<bool> g_nv_run{false};
static nvmlDevice_t g_nvdev; static bool g_nvml = false;
static void nvml_thread(int ms) {
  while (g_nv_run) {
    Samp s; s.t = now_s(); s.sm = s.mem = s.temp = 0; s.pw = 0; s.rs = 0;
    nvmlDeviceGetClockInfo(g_nvdev, NVML_CLOCK_SM, &s.sm);
    nvmlDeviceGetClockInfo(g_nvdev, NVML_CLOCK_MEM, &s.mem);
    nvmlFieldValue_t fv; memset(&fv, 0, sizeof fv); fv.fieldId = NVML_FI_DEV_POWER_INSTANT;
    if (nvmlDeviceGetFieldValues(g_nvdev, 1, &fv) == NVML_SUCCESS && fv.nvmlReturn == NVML_SUCCESS) s.pw = fv.value.uiVal / 1000.0;
    else { unsigned mw = 0; nvmlDeviceGetPowerUsage(g_nvdev, &mw); s.pw = mw / 1000.0; }
    nvmlDeviceGetCurrentClocksEventReasons(g_nvdev, &s.rs);
    nvmlDeviceGetTemperature(g_nvdev, NVML_TEMPERATURE_GPU, &s.temp);
    { std::lock_guard<std::mutex> l(g_smu); g_samps.push_back(s); }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }
}
static void nvml_start(int ms) {
  if (nvmlInit_v2() != NVML_SUCCESS) { printf("nvml: init failed\n"); return; }
  int dev; CK(cudaGetDevice(&dev)); char bus[64]; CK(cudaDeviceGetPCIBusId(bus, sizeof bus, dev));
  if (nvmlDeviceGetHandleByPciBusId_v2(bus, &g_nvdev) != NVML_SUCCESS) { printf("nvml: no handle for %s\n", bus); return; }
  unsigned lim = 0; nvmlDeviceGetEnforcedPowerLimit(g_nvdev, &lim);
  printf("nvml: bus=%s power_limit_w=%.0f interval_ms=%d\n", bus, lim / 1000.0, ms);
  g_nvml = true; g_nv_run = true; std::thread(nvml_thread, ms).detach();
}
struct NvSum { int n = 0; double sm_med = NAN, sm_min = NAN, sm_max = NAN, pw_mean = NAN, pw_max = NAN, temp = NAN; unsigned long long rs = 0; };
static NvSum nvml_window(double a, double b) {
  NvSum r; std::vector<double> sm, pw; double tmax = 0;
  std::lock_guard<std::mutex> l(g_smu);
  for (auto& s : g_samps) if (s.t >= a && s.t <= b) { sm.push_back(s.sm); pw.push_back(s.pw); r.rs |= s.rs; tmax = std::max(tmax, (double)s.temp); }
  r.n = (int)sm.size();
  if (!sm.empty()) {
    r.sm_med = medd(sm); r.sm_min = *std::min_element(sm.begin(), sm.end()); r.sm_max = *std::max_element(sm.begin(), sm.end());
    double t = 0; for (double x : pw) t += x; r.pw_mean = t / pw.size(); r.pw_max = *std::max_element(pw.begin(), pw.end()); r.temp = tmax;
  }
  return r;
}

// ------------------------------------------------------------------ buffer, classification
static bool g_nomask = false;  // under ncu: no mask API at all, placement by %smid only (grids cover all SMs)
static void tpc_set(u128 m) { if (!g_nomask) tpc_next_enable(m); }
static int g_small_grid() { return g_nomask ? 264 : 4; }
static uint32_t* g_buf = nullptr; static size_t g_bytes = 0;
static const size_t MB = 1ull << 20, GB = 1ull << 30;
static size_t Z_PROBE = 0, Z_GRAN = 8 * MB, Z_MAIN = 16 * MB, Z_WRITE, Z_COLD, Z_END;
static std::vector<uint8_t> g_home;  // per 4 KB block
static uint8_t* d_home = nullptr;
static void alloc_buf(double gib) {
  g_bytes = (size_t)(gib * GB);
  CK(cudaMalloc(&g_buf, g_bytes));
  init_buf<<<132 * 4, 1024>>>(g_buf, g_bytes / 4); CK(cudaDeviceSynchronize());
  Z_END = g_bytes; Z_COLD = g_bytes - 6 * GB; Z_WRITE = Z_COLD - 2 * GB;
  printf("buf=%p gib=%.2f align_2MB=%s zones probe=0 gran=%zu main=%zu write=%zu cold=%zu end=%zu\n", (void*)g_buf, gib,
         ((uintptr_t)g_buf % (2u << 20)) ? "no" : "yes", Z_GRAN, Z_MAIN, Z_WRITE, Z_COLD, Z_END);
  if ((uintptr_t)g_buf % (2u << 20)) { printf("FATAL buffer not 2 MB aligned\n"); exit(1); }
}
static void flush_l2() {
  unsigned* o; CK(cudaMalloc(&o, 4));
  tpc_set(0);
  k_flush<<<132 * 2, 1024>>>(reinterpret_cast<const uint4*>((const char*)g_buf + Z_END - GB), GB / 16, o);
  CK(cudaDeviceSynchronize()); CK(cudaFree(o));
}
static int hparity(uint64_t off) { return (int)(((off >> 12) ^ (off >> 13) ^ (off >> 15) ^ (off >> 17) ^ (off >> 19)) & 1); }
// latency of lines[] read on SM rd after a warm by SM wm (lines are byte offset / 128)
static std::vector<unsigned short> lat_after_warm(const std::vector<uint32_t>& lines, int wm, int rd, unsigned* errs_out) {
  uint32_t* d_l; unsigned short* d_o; unsigned *d_e, *d_c;
  CK(cudaMalloc(&d_l, lines.size() * 4)); CK(cudaMalloc(&d_o, lines.size() * 2)); CK(cudaMalloc(&d_e, 4)); CK(cudaMalloc(&d_c, 16));
  CK(cudaMemcpy(d_l, lines.data(), lines.size() * 4, cudaMemcpyHostToDevice));
  CK(cudaMemset(d_c, 0, 16));
  if (wm >= 0) { tpc_set(mask_of_sms({wm})); k_touch<<<g_small_grid(), 1024>>>((const char*)g_buf, d_l, (int)lines.size(), wm, d_c); CK(cudaDeviceSynchronize()); }
  CK(cudaMemset(d_c, 0, 16));
  tpc_set(mask_of_sms({rd})); k_lat_list<<<g_small_grid(), 32>>>((const char*)g_buf, d_l, (int)lines.size(), rd, d_o, d_e, d_c); CK(cudaDeviceSynchronize());
  std::vector<unsigned short> h(lines.size()); CK(cudaMemcpy(h.data(), d_o, lines.size() * 2, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(errs_out, d_e, 4, cudaMemcpyDeviceToHost));
  CK(cudaFree(d_l)); CK(cudaFree(d_o)); CK(cudaFree(d_e)); CK(cudaFree(d_c));
  return h;
}
// per 2 MB page: c such that home side B = hparity(off) ^ c. Warm by SM wa (side A), read by SM rb (side B):
// lines homed on A are far hits for rb (> thr cycles).
static void classify(int wa, int rb, int thr) {
  const size_t npages = g_bytes / (2 * MB), lpp = 16;
  std::vector<int> c(npages, -1); std::vector<double> conf(npages, 0);
  g_home.assign(g_bytes / 4096, 2);
  const size_t chunk = 512;
  unsigned errs_tot = 0; std::vector<unsigned short> sample_lat;
  flush_l2();
  for (size_t p0 = 0; p0 < npages; p0 += chunk) {
    std::vector<uint32_t> lines;
    for (size_t pg = p0; pg < std::min(npages, p0 + chunk); ++pg)
      for (size_t k = 0; k < lpp; ++k) { const uint64_t off = pg * 2 * MB + k * 128 * 1024 + (k % 4) * 4096 + (k % 3) * 1024; lines.push_back((uint32_t)(off / 128)); }
    unsigned e = 0;
    std::vector<unsigned short> lat = lat_after_warm(lines, wa, rb, &e); errs_tot += e;
    if (p0 == 0) sample_lat = lat;
    for (size_t pg = p0, i = 0; pg < std::min(npages, p0 + chunk); ++pg) {
      int votes[2] = {0, 0};
      for (size_t k = 0; k < lpp; ++k, ++i) {
        const uint64_t off = (uint64_t)lines[i] * 128;
        const int homeB = lat[i] < thr;  // near hit for rb (side B) => homed on B
        votes[homeB ^ hparity(off)]++;
      }
      c[pg] = votes[1] > votes[0]; conf[pg] = std::max(votes[0], votes[1]) / (double)lpp;
    }
  }
  int lowc = 0, full = 0;
  for (size_t pg = 0; pg < npages; ++pg) {
    if (conf[pg] >= 0.8) { for (size_t b = 0; b < 512; ++b) { const uint64_t off = pg * 2 * MB + b * 4096; g_home[off / 4096] = (uint8_t)(hparity(off) ^ c[pg]); } }
    else ++lowc;
    full += conf[pg] == 1.0;
  }
  std::vector<unsigned short> s = sample_lat; std::sort(s.begin(), s.end());
  int nA = 0, nB = 0; for (uint8_t h : g_home) { nA += h == 0; nB += h == 1; }
  printf("classify warm_sm=%d read_sm=%d thr=%d pages=%zu conf1=%d lowconf=%d errs=%u blocksA=%d blocksB=%d sample_lat p10=%u p25=%u p50=%u p75=%u p90=%u\n",
         wa, rb, thr, npages, full, lowc, errs_tot, nA, nB, s[s.size() / 10], s[s.size() / 4], s[s.size() / 2], s[s.size() * 3 / 4], s[s.size() * 9 / 10]);
  if (full < (int)npages * 9 / 10 || errs_tot) { printf("FATAL classification unreliable\n"); exit(1); }
  CK(cudaMalloc(&d_home, g_home.size())); CK(cudaMemcpy(d_home, g_home.data(), g_home.size(), cudaMemcpyHostToDevice));
}
// side of each GPC: warm 4096 lines by SM wa, read from the first SM of each GPC; far fraction ~0 => same side as wa
static void side_check(int wa, int thr) {
  std::vector<uint32_t> lines;
  for (uint32_t i = 0; i < 4096; ++i) lines.push_back((uint32_t)((Z_COLD + 64 * MB) / 128 + i * 9));
  for (size_t g = 0; g < g_gpc_sms.size(); ++g) {
    const int rd = g_gpc_sms[g][0];
    if (rd == wa) continue;
    flush_l2(); unsigned e = 0;
    std::vector<unsigned short> lat = lat_after_warm(lines, wa, rd, &e);
    int far = 0; for (auto x : lat) far += x >= thr;
    std::vector<unsigned short> s = lat; std::sort(s.begin(), s.end());
    printf("sidecheck gpc=%zu sm=%d warm_sm=%d far_frac=%.3f p50=%u errs=%u => side %s\n", g, rd, wa, far / 4096.0, s[2048], e, far < 400 ? "same-as-warm(A)" : "other(B)");
  }
}

// ------------------------------------------------------------------ plan
struct Group {
  std::string name, data = "contig"; std::vector<int> sms; int mode = M_IDLE; double mb = 64; int depth = 6, tile = 32, warps = 32;
  double pace = 0, on_us = 0, per_us = 0; int lat = L_HIT, side = 0, warm = -1; long long latcap = 0;
};
struct Scen { std::string name; int trials = 5; double dur = 1000, bin = 1000; std::vector<Group> g; };
static std::map<std::string, std::string> kvparse(const std::string& line) {
  std::map<std::string, std::string> m; size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && line[i] == ' ') ++i;
    size_t j = line.find(' ', i); if (j == std::string::npos) j = line.size();
    std::string t = line.substr(i, j - i); size_t e = t.find('=');
    if (e != std::string::npos) m[t.substr(0, e)] = t.substr(e + 1);
    i = j;
  }
  return m;
}
static std::vector<Scen> load_plan(const std::string& f) {
  std::vector<Scen> v; FILE* fp = fopen(f.c_str(), "r"); if (!fp) { fprintf(stderr, "cannot open plan %s\n", f.c_str()); exit(1); }
  char buf[65536];
  while (fgets(buf, sizeof buf, fp)) {
    std::string line(buf); while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.size() < 2 || line[0] == '#') continue;
    auto m = kvparse(line.substr(2));
    if (line[0] == 'S') {
      Scen s; s.name = m["name"]; if (m.count("trials")) s.trials = atoi(m["trials"].c_str());
      if (m.count("dur")) s.dur = atof(m["dur"].c_str()); if (m.count("bin")) s.bin = atof(m["bin"].c_str());
      v.push_back(s);
    } else if (line[0] == 'G') {
      if (v.empty()) { fprintf(stderr, "G before S\n"); exit(1); }
      Group g; g.name = m["name"]; g.sms = parse_list(m["sms"]);
      const std::string md = m["mode"];
      g.mode = md == "tma" ? M_TMA : md == "ld" ? M_LD : md == "st" ? M_ST : md == "tmast" ? M_TMAST : md == "fma" ? M_FMA : md == "lat" ? M_LAT : M_IDLE;
      if (m.count("data")) g.data = m["data"]; if (m.count("mb")) g.mb = atof(m["mb"].c_str());
      if (m.count("depth")) g.depth = atoi(m["depth"].c_str()); if (m.count("tile")) g.tile = atoi(m["tile"].c_str());
      if (m.count("warps")) g.warps = atoi(m["warps"].c_str()); if (m.count("pace")) g.pace = atof(m["pace"].c_str());
      if (m.count("duty")) sscanf(m["duty"].c_str(), "%lf/%lf", &g.on_us, &g.per_us);
      if (m.count("lat")) g.lat = m["lat"] == "miss" ? L_MISS : m["lat"] == "far" ? L_FAR : L_HIT;
      if (m.count("side")) g.side = m["side"] == "B"; if (m.count("warm")) g.warm = atoi(m["warm"].c_str());
      if (m.count("latcap")) g.latcap = atoll(m["latcap"].c_str());
      if (g.mode == M_TMA && g.depth > 8) { fprintf(stderr, "depth <= 8\n"); exit(1); }
      v.back().g.push_back(g);
    }
  }
  fclose(fp);
  return v;
}

// ------------------------------------------------------------------ scenario setup
struct Setup {
  KP kp; int units = 0; std::vector<int> usm, ugrp; std::vector<unsigned long long> ubase, ubytes, uloff;
  std::vector<uint32_t> list; u128 mask = 0; size_t smem = 0; int threads = 32; int nbins = 1;
  // device
  int *d_sm2unit, *d_ugrp, *d_claim, *d_osm; unsigned long long *d_ubase, *d_ubytes, *d_uloff, *d_start, *d_ob, *d_ot, *d_tl, *d_lsum;
  uint32_t* d_list; long long* d_ocyc; unsigned *d_oerr, *d_hist;
  std::vector<std::pair<int, std::vector<uint32_t>>> warms;  // (warm SM, lines) before the launch
  std::vector<int> lat_units_miss;
};
static void build(const Scen& s, Setup& S) {
  S = Setup();
  std::vector<int> sm2unit(MAXSM, -1);
  size_t cur_main = Z_MAIN, cur_write = Z_WRITE, cur_hit = Z_PROBE;
  size_t cur_home[2] = {Z_MAIN / 4096, Z_MAIN / 4096};
  std::map<std::string, size_t> gran_cur;
  size_t far_cur[2] = {0, 0};
  std::vector<uint32_t> far_lines[2];  // lines in probe zone pages 2-3 homed on side 0/1
  for (size_t off = 4 * MB; off < 8 * MB; off += 128) { const uint8_t h = g_home[off / 4096]; if (h < 2) far_lines[h].push_back((uint32_t)(off / 128)); }
  memset(&S.kp, 0, sizeof S.kp);
  if (s.g.size() > MAXG) { fprintf(stderr, "too many groups\n"); exit(1); }
  for (size_t gi = 0; gi < s.g.size(); ++gi) {
    const Group& g = s.g[gi]; GP& P = S.kp.g[gi];
    P.mode = g.mode; P.depth = g.depth; P.tile = g.tile * 1024; P.warps = g.warps; P.lat = g.lat; P.side = g.side; P.latcap = g.latcap;
    P.on_ns = (unsigned long long)(g.on_us * 1e3); P.per_ns = (unsigned long long)(g.per_us * 1e3);
    P.B = 4096; P.contig = 1; P.shared = g.data == "shared";
    const bool home = !g.data.compare(0, 5, "home:"), gran = !g.data.compare(0, 5, "gran:");
    if (home) { P.contig = 0; P.B = 4096; }
    if (gran) { P.contig = 0; P.B = 256; }
    if (g.mode == M_TMA && gran) { fprintf(stderr, "TMA with gran data not supported\n"); exit(1); }
    // pacing quantum
    if (g.pace > 0) {
      if (g.mode == M_TMA || g.mode == M_TMAST) P.pace_q = (unsigned long long)(P.tile / g.pace);            // bytes / (GB/s) = ns
      else P.pace_q = (unsigned long long)(4096.0 * g.warps / g.pace);
    }
    if (g.mode == M_TMA) { S.smem = std::max(S.smem, (size_t)1024 + (size_t)g.depth * P.tile); S.threads = std::max(S.threads, 32); }
    if (g.mode == M_TMAST) { S.smem = std::max(S.smem, (size_t)1024 + (size_t)P.tile); }
    if (g.mode == M_LD || g.mode == M_ST || g.mode == M_FMA) S.threads = std::max(S.threads, 32 * g.warps);
    const size_t q = g.mode == M_TMA ? (size_t)P.tile : 4096;  // unit data multiple
    size_t ub = (size_t)(g.mb * MB); ub = std::max(q, ub / q * q);
    size_t shared_base = 0;
    if (P.shared) { shared_base = cur_main; cur_main += ub; }
    // far lines for this group (split among its units)
    for (size_t k = 0; k < g.sms.size(); ++k) {
      const int sm = g.sms[k];
      if (sm2unit[sm] >= 0) { fprintf(stderr, "scenario %s: SM %d in two groups\n", s.name.c_str(), sm); exit(1); }
      const int u = S.units++;
      sm2unit[sm] = u; S.usm.push_back(sm); S.ugrp.push_back((int)gi);
      unsigned long long base = 0, bytes = ub, loff = S.list.size();
      if (g.mode == M_LAT) {
        if (g.lat == L_HIT) { bytes = std::max<size_t>(4096, (size_t)(g.mb * MB) / 4096 * 4096); base = cur_hit; cur_hit += bytes; if (cur_hit > 4 * MB) { fprintf(stderr, "hit regions exceed 4 MB\n"); exit(1); } }
        else if (g.lat == L_MISS) { bytes = (size_t)(g.mb * MB); base = 0; S.lat_units_miss.push_back(u); }
        else {
          const int other = 1 - g.side; auto& FL = far_lines[other];
          const size_t n = g.latcap ? (size_t)g.latcap : 8192;
          if (far_cur[other] + n > FL.size()) { fprintf(stderr, "not enough far lines\n"); exit(1); }
          std::vector<uint32_t> mine(FL.begin() + far_cur[other], FL.begin() + far_cur[other] + n); far_cur[other] += n;
          S.list.insert(S.list.end(), mine.begin(), mine.end()); bytes = n;
          if (g.warm >= 0) S.warms.push_back({g.warm, mine});
        }
      } else if (g.mode == M_ST || g.mode == M_TMAST) { base = cur_write; cur_write += ub; if (cur_write > Z_COLD) { fprintf(stderr, "write area exhausted\n"); exit(1); } }
      else if (g.mode == M_FMA || g.mode == M_IDLE) { bytes = 4096; }
      else if (P.shared) { base = shared_base; }
      else if (home) {
        const int side = g.data[5] == 'B';
        const size_t need = ub / 4096; size_t got = 0;
        while (got < need) {
          if (cur_home[side] * 4096 >= Z_WRITE) { fprintf(stderr, "home blocks exhausted\n"); exit(1); }
          if (g_home[cur_home[side]] == side) { S.list.push_back((uint32_t)cur_home[side]); ++got; }
          ++cur_home[side];
        }
      } else if (gran) {
        // gran:P:M1:V1[:M2:V2] - 256 B granules of 2 MB page P of the granule zone, filtered by parities
        int pg = 0; unsigned m1 = 0, v1 = 0, m2 = 0, v2 = 0;
        int nf = sscanf(g.data.c_str() + 5, "%d:%x:%u:%x:%u", &pg, &m1, &v1, &m2, &v2);
        if (nf < 3) { fprintf(stderr, "bad gran spec %s\n", g.data.c_str()); exit(1); }
        if (nf < 5) m2 = 0;
        const size_t pbase = Z_GRAN + (size_t)pg * 2 * MB;
        if (pbase + 2 * MB > Z_MAIN) { fprintf(stderr, "gran page out of zone\n"); exit(1); }
        size_t& c = gran_cur[g.data.substr(5)];
        const size_t need = ub / 256; size_t got = 0;
        for (; c < 8192 && got < need; ++c) {
          const uint32_t off = (uint32_t)(c * 256);
          if ((__builtin_popcount(off & m1) & 1) != (int)v1) continue;
          if (m2 && (__builtin_popcount(off & m2) & 1) != (int)v2) continue;
          S.list.push_back((uint32_t)((pbase + off) / 256)); ++got;
        }
        if (got < need) { fprintf(stderr, "gran set %s exhausted (%zu of %zu)\n", g.data.c_str(), got, need); exit(1); }
      } else { base = cur_main; cur_main += ub; if (cur_main > Z_WRITE) { fprintf(stderr, "main area exhausted\n"); exit(1); } }
      S.ubase.push_back(base); S.ubytes.push_back(bytes); S.uloff.push_back(loff);
    }
  }
  if (S.list.empty()) S.list.push_back(0);
  std::vector<int> allsms; for (auto& g : s.g) allsms.insert(allsms.end(), g.sms.begin(), g.sms.end());
  S.mask = mask_of_sms(allsms);
  S.nbins = std::max(1, (int)std::ceil(s.dur * 1000.0 / s.bin));
  const int U = S.units;
  CK(cudaMalloc(&S.d_sm2unit, MAXSM * 4)); CK(cudaMemcpy(S.d_sm2unit, sm2unit.data(), MAXSM * 4, cudaMemcpyHostToDevice));
  CK(cudaMalloc(&S.d_ugrp, U * 4)); CK(cudaMemcpy(S.d_ugrp, S.ugrp.data(), U * 4, cudaMemcpyHostToDevice));
  CK(cudaMalloc(&S.d_ubase, U * 8)); CK(cudaMalloc(&S.d_ubytes, U * 8)); CK(cudaMalloc(&S.d_uloff, U * 8));
  CK(cudaMemcpy(S.d_ubytes, S.ubytes.data(), U * 8, cudaMemcpyHostToDevice)); CK(cudaMemcpy(S.d_uloff, S.uloff.data(), U * 8, cudaMemcpyHostToDevice));
  CK(cudaMalloc(&S.d_list, S.list.size() * 4)); CK(cudaMemcpy(S.d_list, S.list.data(), S.list.size() * 4, cudaMemcpyHostToDevice));
  CK(cudaMalloc(&S.d_claim, MAXSM * 4)); CK(cudaMalloc(&S.d_osm, U * 4)); CK(cudaMalloc(&S.d_start, 8));
  CK(cudaMalloc(&S.d_ob, U * 8)); CK(cudaMalloc(&S.d_ot, U * 16)); CK(cudaMalloc(&S.d_ocyc, U * 8)); CK(cudaMalloc(&S.d_oerr, MAXSM * 4));
  CK(cudaMalloc(&S.d_tl, (size_t)U * S.nbins * 8)); CK(cudaMalloc(&S.d_hist, (size_t)U * 2 * NB * 4)); CK(cudaMalloc(&S.d_lsum, U * 32));
  KP& k = S.kp;
  k.buf = (const char*)g_buf; k.sm2unit = S.d_sm2unit; k.ugrp = S.d_ugrp; k.ubase = S.d_ubase; k.ubytes = S.d_ubytes; k.uloff = S.d_uloff;
  k.list = S.d_list; k.home = d_home; k.claim = S.d_claim; k.start = S.d_start; k.dur_ns = (unsigned long long)(s.dur * 1e6);
  k.ob = S.d_ob; k.ocyc = S.d_ocyc; k.oerr = S.d_oerr; k.osm = S.d_osm; k.ot = S.d_ot; k.tl = S.d_tl; k.nbins = S.nbins;
  k.bin_ns = (unsigned long long)(s.bin * 1e3); k.hist = S.d_hist; k.lsum = S.d_lsum;
}
static void release(Setup& S) {
  cudaFree(S.d_sm2unit); cudaFree(S.d_ugrp); cudaFree(S.d_ubase); cudaFree(S.d_ubytes); cudaFree(S.d_uloff); cudaFree(S.d_list);
  cudaFree(S.d_claim); cudaFree(S.d_osm); cudaFree(S.d_start); cudaFree(S.d_ob); cudaFree(S.d_ot); cudaFree(S.d_ocyc); cudaFree(S.d_oerr);
  cudaFree(S.d_tl); cudaFree(S.d_hist); cudaFree(S.d_lsum);
}

// ------------------------------------------------------------------ run
struct GRes { double med_gbs, sum_gbs, min_gbs, max_gbs, mhz, bpc, on_gbs, off_gbs, l0p50, l0mean, l1p50, l1mean, l0p10, l0p90, l1p10, l1p90; long long n0, n1; };
static std::map<std::string, std::vector<GRes>> g_res;   // key "scen|group"
static std::map<std::string, std::vector<NvSum>> g_nvres;
static FILE* g_hist = nullptr;
static int g_gap_ms = 100; static size_t g_cold_rot = 0;
static double hq(const unsigned* h, double q) {
  unsigned long long t = 0; for (int i = 0; i < NB; ++i) t += h[i]; if (!t) return NAN;
  unsigned long long a = 0; for (int i = 0; i < NB; ++i) { a += h[i]; if (a >= q * t) return (i + 0.5) * BINW; } return NB * BINW;
}
static bool run_one(const Scen& s, Setup& S, int trial, cudaStream_t st) {
  const int U = S.units;
  // cold regions for miss probes: rotate through the cold area (1 GiB windows)
  std::vector<unsigned long long> ubase = S.ubase;
  for (int u : S.lat_units_miss) {
    const size_t span = std::max<size_t>(4096, S.ubytes[u] / 4096 * 4096);
    const size_t nslots = (Z_END - GB - Z_COLD) / span;  // leave the last 1 GiB (flush region)
    ubase[u] = Z_COLD + (g_cold_rot++ % nslots) * span;
  }
  CK(cudaMemcpy(S.d_ubase, ubase.data(), U * 8, cudaMemcpyHostToDevice));
  CK(cudaMemset(S.d_claim, 0, MAXSM * 4)); CK(cudaMemset(S.d_osm, 0xff, U * 4)); CK(cudaMemset(S.d_start, 0, 8));
  CK(cudaMemset(S.d_ob, 0, U * 8)); CK(cudaMemset(S.d_ot, 0, U * 16)); CK(cudaMemset(S.d_ocyc, 0, U * 8)); CK(cudaMemset(S.d_oerr, 0, MAXSM * 4));
  CK(cudaMemset(S.d_tl, 0, (size_t)U * S.nbins * 8)); CK(cudaMemset(S.d_hist, 0, (size_t)U * 2 * NB * 4)); CK(cudaMemset(S.d_lsum, 0, U * 32));
  CK(cudaDeviceSynchronize());
  // warms (lat far): drop earlier (persisting) replicas, flush, then one CTA on the warm SM reads the lines
  if (!S.warms.empty()) { cudaCtxResetPersistingL2Cache(); cudaGetLastError(); flush_l2(); }
  for (auto& w : S.warms) {
    uint32_t* d_l; unsigned* d_c; CK(cudaMalloc(&d_l, w.second.size() * 4)); CK(cudaMalloc(&d_c, 16)); CK(cudaMemset(d_c, 0, 16));
    CK(cudaMemcpy(d_l, w.second.data(), w.second.size() * 4, cudaMemcpyHostToDevice));
    tpc_set(mask_of_sms({w.first})); k_touch<<<g_small_grid(), 1024, 0, st>>>((const char*)g_buf, d_l, (int)w.second.size(), w.first, d_c);
    CK(cudaStreamSynchronize(st)); cudaFree(d_l); cudaFree(d_c);
  }
  const int nsm_launch = g_nomask ? 132 : u128_popc(S.mask) * 2;
  const int grid = nsm_launch * (S.smem > 100 * 1024 ? 1 : 4);
  tpc_set(S.mask);
  k_bys<<<grid, S.threads, S.smem, st>>>(S.kp);
  CK(cudaGetLastError());
  wait_dev(st, s.name.c_str());
  const double t_done = now_s();
  std::vector<unsigned long long> ob(U), ot(2 * U), tl((size_t)U * S.nbins), lsum(4 * U); std::vector<long long> oc(U); std::vector<int> osm(U); std::vector<unsigned> oe(MAXSM), hist((size_t)U * 2 * NB);
  CK(cudaMemcpy(ob.data(), S.d_ob, U * 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(ot.data(), S.d_ot, U * 16, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(oc.data(), S.d_ocyc, U * 8, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(osm.data(), S.d_osm, U * 4, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(oe.data(), S.d_oerr, MAXSM * 4, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(tl.data(), S.d_tl, (size_t)U * S.nbins * 8, cudaMemcpyDeviceToHost));
  CK(cudaMemcpy(hist.data(), S.d_hist, (size_t)U * 2 * NB * 4, cudaMemcpyDeviceToHost)); CK(cudaMemcpy(lsum.data(), S.d_lsum, U * 32, cudaMemcpyDeviceToHost));
  int missing = 0, wrong = 0; unsigned errs = 0;
  for (int u = 0; u < U; ++u) { if (osm[u] < 0) ++missing; else if (osm[u] != S.usm[u]) ++wrong; errs += oe[u]; }
  const NvSum nv = nvml_window(t_done - s.dur / 1000.0 + 0.02, t_done - 0.01);
  const bool ok = !missing && !wrong && !errs;
  printf("R s=%s t=%d ok=%d missing=%d wrong_sm=%d errs=%u nv_n=%d nv_sm_med=%.0f nv_sm_min=%.0f nv_sm_max=%.0f nv_pw_mean=%.1f nv_pw_max=%.1f nv_temp=%.0f nv_reasons=0x%llx\n",
         s.name.c_str(), trial, ok, missing, wrong, errs, nv.n, nv.sm_med, nv.sm_min, nv.sm_max, nv.pw_mean, nv.pw_max, nv.temp, nv.rs);
  const double dur_ns = s.dur * 1e6;
  for (size_t gi = 0; gi < s.g.size(); ++gi) {
    const Group& g = s.g[gi];
    std::vector<double> gbs, mhz, bpc; double sum = 0; std::string per;
    std::vector<double> segsum(10, 0), onv, offv;
    std::vector<unsigned> h0(NB, 0), h1(NB, 0); long long n0 = 0, n1 = 0; double s0 = 0, s1 = 0;
    for (int u = 0; u < U; ++u) {
      if (S.ugrp[u] != (int)gi || osm[u] < 0) continue;
      const double r = ob[u] / dur_ns;  // GB/s (FMA: mma/ns)
      const double ns = (double)(ot[2 * u + 1] - ot[2 * u]);
      const double f = ns > 0 ? oc[u] * 1e3 / ns : 0;   // MHz over the unit's own run
      const double cyc_win = f * 1e-3 * dur_ns;
      gbs.push_back(r); mhz.push_back(f); bpc.push_back(cyc_win > 0 ? ob[u] / cyc_win : 0); sum += r;
      char b[64]; snprintf(b, sizeof b, " %d:%.1f:%.0f", osm[u], g.mode == M_FMA ? r * 4096e-3 : r, f); per += b;
      for (int k = 0; k < S.nbins; ++k) segsum[std::min(9, k * 10 / S.nbins)] += tl[(size_t)u * S.nbins + k];
      if (g.mode == M_LAT) {
        for (int k = 0; k < NB; ++k) { h0[k] += hist[(size_t)u * 2 * NB + k]; h1[k] += hist[(size_t)u * 2 * NB + NB + k]; }
        s0 += lsum[4 * u]; n0 += lsum[4 * u + 1]; s1 += lsum[4 * u + 2]; n1 += lsum[4 * u + 3];
        if (g_hist) {
          fprintf(g_hist, "H s=%s t=%d g=%s sm=%d c0", s.name.c_str(), trial, g.name.c_str(), osm[u]);
          for (int k = 0; k < NB; ++k) fprintf(g_hist, " %u", hist[(size_t)u * 2 * NB + k]);
          fprintf(g_hist, " c1"); for (int k = 0; k < NB; ++k) fprintf(g_hist, " %u", hist[(size_t)u * 2 * NB + NB + k]);
          fprintf(g_hist, "\n");
        }
      }
    }
    // duty fold: if any group of the scenario has a duty cycle, rate of this group in on / off bins (whole bins only, first bin after a switch excluded)
    double on_r = NAN, off_r = NAN;
    for (auto& dg : s.g) if (dg.per_us > 0) {
      double son = 0, soff = 0; int non = 0, noff = 0;
      for (int k = 0; k < S.nbins; ++k) {
        const double b0 = k * s.bin, b1 = b0 + s.bin; const double ph0 = fmod(b0, dg.per_us), ph1 = ph0 + s.bin;
        double tot = 0; for (int u = 0; u < U; ++u) if (S.ugrp[u] == (int)gi) tot += tl[(size_t)u * S.nbins + k];
        if (ph1 <= dg.on_us && ph0 >= s.bin) { son += tot; ++non; }
        else if (ph0 >= dg.on_us + s.bin && ph1 <= dg.per_us) { soff += tot; ++noff; }
        (void)b1;
      }
      const double ng = (double)gbs.size();
      if (non) on_r = son / (non * s.bin * 1e3) / ng; if (noff) off_r = soff / (noff * s.bin * 1e3) / ng;
      break;
    }
    GRes R; memset(&R, 0, sizeof R);
    R.med_gbs = medd(gbs); R.sum_gbs = sum; R.min_gbs = gbs.empty() ? NAN : *std::min_element(gbs.begin(), gbs.end());
    R.max_gbs = gbs.empty() ? NAN : *std::max_element(gbs.begin(), gbs.end()); R.mhz = medd(mhz); R.bpc = medd(bpc); R.on_gbs = on_r; R.off_gbs = off_r;
    R.l0p50 = hq(h0.data(), 0.5); R.l0p10 = hq(h0.data(), 0.1); R.l0p90 = hq(h0.data(), 0.9); R.l0mean = n0 ? s0 / n0 : NAN;
    R.l1p50 = hq(h1.data(), 0.5); R.l1p10 = hq(h1.data(), 0.1); R.l1p90 = hq(h1.data(), 0.9); R.l1mean = n1 ? s1 / n1 : NAN; R.n0 = n0; R.n1 = n1;
    const char* unit = g.mode == M_FMA ? "tflops" : "gbs";
    const double sc = g.mode == M_FMA ? 4.096 : 1.0;
    printf("  g=%s mode=%d n=%zu med_%s=%.2f sum_%s=%.1f min=%.2f max=%.2f mhz_med=%.0f bpc_med=%.2f", g.name.c_str(), g.mode, gbs.size(), unit, R.med_gbs * sc, unit, sum * sc,
           R.min_gbs * sc, R.max_gbs * sc, R.mhz, R.bpc);
    if (!std::isnan(on_r)) printf(" duty_on_gbs=%.2f duty_off_gbs=%.2f", on_r, off_r);
    if (g.mode == M_LAT) printf(" lat0 n=%lld p10=%.0f p50=%.0f p90=%.0f mean=%.1f lat1 n=%lld p10=%.0f p50=%.0f p90=%.0f mean=%.1f", n0, R.l0p10, R.l0p50, R.l0p90, R.l0mean, n1, R.l1p10, R.l1p50, R.l1p90, R.l1mean);
    else if (g.mode != M_FMA && g.mode != M_IDLE) {
      printf(" seg_gbs="); const double segns = dur_ns / 10.0;
      for (int k = 0; k < 10; ++k) printf("%s%.1f", k ? "," : "", segsum[k] / segns / std::max<size_t>(1, gbs.size()));
    }
    printf("\n    u%s\n", per.c_str());
    if (ok) g_res[s.name + "|" + g.name].push_back(R);
  }
  if (ok) g_nvres[s.name].push_back(nv);
  fflush(stdout); if (g_hist) fflush(g_hist);
  return ok;
}
static void summary(const std::vector<Scen>& P) {
  auto rng = [](std::vector<double> v, double& md, double& lo, double& hi) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return std::isnan(x); }), v.end());
    if (v.empty()) { md = lo = hi = NAN; return; }
    md = medd(v); lo = *std::min_element(v.begin(), v.end()); hi = *std::max_element(v.begin(), v.end());
  };
  for (auto& s : P) {
    auto& nvs = g_nvres[s.name];
    std::vector<double> a, b; for (auto& x : nvs) { a.push_back(x.sm_med); b.push_back(x.pw_mean); }
    double m1, l1, h1, m2, l2, h2; rng(a, m1, l1, h1); rng(b, m2, l2, h2);
    unsigned long long rs = 0; for (auto& x : nvs) rs |= x.rs;
    printf("SUMS s=%s trials_ok=%zu nv_sm_mhz=%.0f [%.0f-%.0f] nv_pw=%.1f [%.1f-%.1f] reasons=0x%llx\n", s.name.c_str(), nvs.size(), m1, l1, h1, m2, l2, h2, rs);
    for (auto& g : s.g) {
      auto& v = g_res[s.name + "|" + g.name];
      std::vector<double> x[8];
      for (auto& r : v) { x[0].push_back(r.med_gbs); x[1].push_back(r.sum_gbs); x[2].push_back(r.mhz); x[3].push_back(r.bpc); x[4].push_back(r.on_gbs); x[5].push_back(r.off_gbs); x[6].push_back(r.l0p50); x[7].push_back(r.l1p50); }
      double m[8], lo[8], hi[8]; for (int k = 0; k < 8; ++k) rng(x[k], m[k], lo[k], hi[k]);
      const double sc = g.mode == M_FMA ? 4.096 : 1.0;
      printf("SUMG s=%s g=%s mode=%d n=%zu trials=%zu med_gbs=%.2f [%.2f-%.2f] sum_gbs=%.1f [%.1f-%.1f] mhz=%.0f [%.0f-%.0f] bpc=%.3f [%.3f-%.3f]",
             s.name.c_str(), g.name.c_str(), g.mode, g.sms.size(), v.size(), m[0] * sc, lo[0] * sc, hi[0] * sc, m[1] * sc, lo[1] * sc, hi[1] * sc, m[2], lo[2], hi[2], m[3], lo[3], hi[3]);
      if (!std::isnan(m[4])) printf(" on=%.2f [%.2f-%.2f] off=%.2f [%.2f-%.2f]", m[4], lo[4], hi[4], m[5], lo[5], hi[5]);
      if (g.mode == M_LAT) printf(" lat0_p50=%.0f [%.0f-%.0f] lat1_p50=%.0f [%.0f-%.0f]", m[6], lo[6], hi[6], m[7], lo[7], hi[7]);
      printf("\n");
    }
  }
  fflush(stdout);
}

static int cmd_run(Args& a) {
  load_map(a.s("map")); load_gpc(a.s("gpc"));
  g_nomask = a.i("nomask", 0); g_gap_ms = (int)a.i("gap-ms", 100);
  std::vector<Scen> P = load_plan(a.s("plan"));
  const std::string only = a.s("only");
  if (!only.empty()) { std::vector<Scen> q; for (auto& s : P) if (s.name == only) q.push_back(s); P = q; }
  int maxtr = 0; for (auto& s : P) maxtr = std::max(maxtr, s.trials);
  printf("plan=%s scenarios=%zu max_trials=%d nomask=%d\n", a.s("plan").c_str(), P.size(), maxtr, (int)g_nomask);
  alloc_buf(a.f("gib", 24));
  nvml_start((int)a.i("nvml-ms", 10));
  CK(cudaFuncSetAttribute(k_bys, cudaFuncAttributeMaxDynamicSharedMemorySize, 210 * 1024));
  cudaStream_t st; CK(cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking));
  if (a.i("persist", 1)) {
    cudaError_t e1 = cudaDeviceSetLimit(cudaLimitPersistingL2CacheSize, 8 * MB);
    cudaStreamAttrValue v; memset(&v, 0, sizeof v);
    v.accessPolicyWindow.base_ptr = (void*)g_buf; v.accessPolicyWindow.num_bytes = 8 * MB; v.accessPolicyWindow.hitRatio = 1.0f;
    v.accessPolicyWindow.hitProp = cudaAccessPropertyPersisting; v.accessPolicyWindow.missProp = cudaAccessPropertyStreaming;
    cudaError_t e2 = cudaStreamSetAttribute(st, cudaStreamAttributeAccessPolicyWindow, &v);
    printf("persist probe zone 8 MB: limit=%s window=%s\n", cudaGetErrorString(e1), cudaGetErrorString(e2));
    cudaGetLastError();
  }
  classify((int)a.i("warm-a", 0), (int)a.i("read-b", 4), (int)a.i("thr", 380));
  const std::string hf = a.s("hist"); if (!hf.empty()) g_hist = fopen(hf.c_str(), "a");
  std::vector<Setup> S(P.size());
  for (size_t i = 0; i < P.size(); ++i) build(P[i], S[i]);
  g_timeout = 120;
  for (int t = 0; t < maxtr; ++t)
    for (size_t i = 0; i < P.size(); ++i) {
      if (t >= P[i].trials) continue;
      int tries = 0;
      while (!run_one(P[i], S[i], t, st) && ++tries < 3) printf("RETRY s=%s t=%d\n", P[i].name.c_str(), t);
      if (g_gap_ms) usleep(g_gap_ms * 1000);
    }
  summary(P);
  g_nv_run = false; usleep(50000);
  return 0;
}
static int cmd_check(Args& a) {
  load_map(a.s("map")); load_gpc(a.s("gpc"));
  int bad = 0, nb = 0;
  int* d; CK(cudaMalloc(&d, 64 * 4));
  for (auto& kv : g_bit2sms) {
    std::vector<int> h(64, -1);
    CK(cudaMemset(d, 0xff, 64 * 4));
    tpc_next_enable(u128_bit(kv.first)); k_where<<<64, 32>>>(d); CK(cudaDeviceSynchronize());
    CK(cudaMemcpy(h.data(), d, 64 * 4, cudaMemcpyDeviceToHost));
    std::set<int> got(h.begin(), h.end()), want(kv.second.begin(), kv.second.end());
    got.erase(-1); ++nb;
    if (got != want) { ++bad; printf("mapcheck bit %d: want", kv.first); for (int x : want) printf(" %d", x); printf(" got"); for (int x : got) printf(" %d", x); printf("\n"); }
  }
  printf("mapcheck bits=%d mismatches=%d\n", nb, bad);
  for (size_t g = 0; g < g_gpc_sms.size(); ++g) { printf("gpc %zu sms", g); for (int s : g_gpc_sms[g]) printf(" %d", s); printf("\n"); }
  alloc_buf(a.f("gib", 8));
  classify((int)a.i("warm-a", 0), (int)a.i("read-b", 4), (int)a.i("thr", 380));
  side_check((int)a.i("warm-a", 0), (int)a.i("thr", 380));
  return 0;
}
int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: bys_lab check|run ...\n"); return 1; }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Args a(argc, argv);
  if (!strcmp(argv[1], "run")) return cmd_run(a);
  if (!strcmp(argv[1], "check")) return cmd_check(a);
  return 1;
}
