// Cluster-TMA borrowing kernel shared by borrow_tma.cu (E1 TMA, E2) and
// borrow_corun.cu (E3).
//
// One cluster of C CTAs. CTA 0 is the consumer: it owns a ring of 1-D bulk
// copy slots (cp.async.bulk, no tensor descriptor) and one "full" mbarrier per
// slot. Issuers are CTA 0's warp 0 (if self_issue) and CTAs 1..nhelpers. Each
// issuer keeps D transfers of tile_bytes outstanding, every one destined to
// CTA 0's shared memory (address from mapa.shared::cluster) and completing on
// CTA 0's mbarrier. Issuer i owns slots [i*D, i*D+D). Transfer n of the
// cluster goes to issuer n % H, which uses slot i*D + (n/H) % D. When a
// consumer warp has finished with a slot it re-arms the slot's full barrier
// (expect_tx) and arrives on the issuer's "empty" barrier for that slot
// (remote arrive when the issuer is another CTA).
//
// Consumer warps 1..nw of CTA 0 take transfers n = w-1, w-1+nw, ... in order.
//   consume = 1: every byte is read from shared memory into registers and
//                summed (models a tensor core reading what was staged).
//   consume = 0: only the first and last 4-byte word of the transfer are read
//                (ingest path alone, but still a check that the data arrived).
// The global buffer holds a[e] = e (uint32), so the host can compute the
// expected checksum (mod 2^32) and the first-word value of every tile.
#pragma once
#include <cuda_runtime.h>
#include <cstdint>

struct BorrowParams {
  const uint32_t* src;
  uint64_t window_tiles;    // source tile index wraps modulo this
  uint64_t cluster_stride;  // first tile of cluster c = c * cluster_stride
  uint32_t T;               // transfers into each consumer, multiple of H
  uint32_t tile_bytes;
  int D;
  int self_issue;
  int nhelpers;
  int consume;
  int nw;
  int self_mapa;
  int nlocal;                 // >0: L issuer warps inside CTA 0 (warps nw+1..nw+L), no helpers
  unsigned long long* sums;   // per cluster, sum of u32 words (mod 2^32 used)
  unsigned* errs;             // per cluster, first/last-word mismatches
  unsigned long long* times;  // per cluster: t0, t1 (globaltimer, ns)
  long long* cycles;          // per cluster: clock64 t1 - t0 on CTA 0
  int* smid;                  // per CTA, grid order
  long long* pstats;          // optional: per (cluster, issuer<16) wait / issue cycles
  unsigned* started;          // optional: incremented by every CTA when it starts
};

#define BORROW_BAR_BYTES 1024  // full[64] at 0, empty[64] at 512
#define BORROW_MAX_SLOTS 64

__device__ __forceinline__ uint32_t smem_u32(const void* p) {
  return static_cast<uint32_t>(__cvta_generic_to_shared(p));
}
__device__ __forceinline__ uint32_t cluster_rank() {
  uint32_t r; asm volatile("mov.u32 %0, %%cluster_ctarank;" : "=r"(r)); return r;
}
__device__ __forceinline__ uint32_t cluster_id_x() {
  uint32_t r; asm volatile("mov.u32 %0, %%clusterid.x;" : "=r"(r)); return r;
}
__device__ __forceinline__ uint32_t sm_id() {
  uint32_t r; asm volatile("mov.u32 %0, %%smid;" : "=r"(r)); return r;
}
__device__ __forceinline__ unsigned long long gtimer() {
  unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t;
}
__device__ __forceinline__ void cluster_sync_all() {
  __syncwarp();
  asm volatile("barrier.cluster.arrive.release.aligned;\n\tbarrier.cluster.wait.acquire.aligned;" ::: "memory");
}
__device__ __forceinline__ uint32_t mapa(uint32_t a, uint32_t rank) {
  uint32_t r; asm volatile("mapa.shared::cluster.u32 %0, %1, %2;" : "=r"(r) : "r"(a), "r"(rank)); return r;
}
__device__ __forceinline__ void mbar_init(uint32_t bar, uint32_t count) {
  asm volatile("mbarrier.init.shared::cta.b64 [%0], %1;" :: "r"(bar), "r"(count) : "memory");
}
__device__ __forceinline__ void fence_mbar_init() {
  asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
}
__device__ __forceinline__ void mbar_arrive_expect_tx(uint32_t bar, uint32_t tx) {
  asm volatile("mbarrier.arrive.expect_tx.shared::cta.b64 _, [%0], %1;" :: "r"(bar), "r"(tx) : "memory");
}
__device__ __forceinline__ void mbar_arrive_local(uint32_t bar) {
  asm volatile("mbarrier.arrive.shared::cta.b64 _, [%0];" :: "r"(bar) : "memory");
}
// arrive (+expect_tx) on a barrier given by a shared::cluster address (CUTLASS
// ClusterBarrier::arrive(cta_id) form: default .release.cta semantics)
__device__ __forceinline__ void mbar_arrive_cluster(uint32_t cbar) {
  asm volatile("mbarrier.arrive.shared::cluster.b64 _, [%0];" :: "r"(cbar) : "memory");
}
__device__ __forceinline__ void mbar_arrive_expect_tx_cluster(uint32_t cbar, uint32_t tx) {
  asm volatile("mbarrier.arrive.expect_tx.release.cluster.shared::cluster.b64 _, [%0], %1;"
               :: "r"(cbar), "r"(tx) : "memory");
}
// BORROW_WAIT 0: try_wait (may suspend the thread), 1: test_wait spin
#ifndef BORROW_WAIT
#define BORROW_WAIT 0
#endif
__device__ __forceinline__ void mbar_wait(uint32_t bar, uint32_t parity) {
  uint32_t ok = 0;
  while (!ok) {
#if BORROW_WAIT == 0
    asm volatile("{\n\t.reg .pred p;\n\tmbarrier.try_wait.parity.shared::cta.b64 p, [%1], %2;\n\t"
                 "selp.u32 %0, 1, 0, p;\n\t}" : "=r"(ok) : "r"(bar), "r"(parity) : "memory");
#else
    asm volatile("{\n\t.reg .pred p;\n\tmbarrier.test_wait.parity.shared::cta.b64 p, [%1], %2;\n\t"
                 "selp.u32 %0, 1, 0, p;\n\t}" : "=r"(ok) : "r"(bar), "r"(parity) : "memory");
#endif
  }
}
// 1-D bulk copy global -> shared::cluster (dst and mbar may be another CTA's)
__device__ __forceinline__ void bulk_g2s(uint32_t cdst, const void* gsrc, uint32_t bytes, uint32_t cbar) {
  asm volatile("cp.async.bulk.shared::cluster.global.mbarrier::complete_tx::bytes [%0], [%1], %2, [%3];"
               :: "r"(cdst), "l"(gsrc), "r"(bytes), "r"(cbar) : "memory");
}
__device__ __forceinline__ void named_bar(int id, int threads) {
  asm volatile("bar.sync %0, %1;" :: "r"(id), "r"(threads) : "memory");
}

__global__ void borrow_kernel(BorrowParams p) {
  extern __shared__ __align__(1024) unsigned char smem[];
  const uint32_t rank = cluster_rank();
  const uint32_t cid = cluster_id_x();
  const int H = p.nlocal ? p.nlocal : p.self_issue + p.nhelpers;
  const int D = p.D;
  const int nslots = H * D;
  const uint32_t full0 = smem_u32(smem);
  const uint32_t empty0 = full0 + 512;
  const uint32_t slot0 = full0 + BORROW_BAR_BYTES;
  const int warp = threadIdx.x / 32, lane = threadIdx.x % 32;
  int issuer = -1;
  uint32_t ebase = empty0;
  bool is_issuing_thread = false;
  if (p.nlocal) {
    if (rank == 0 && warp > p.nw && warp <= p.nw + p.nlocal) {
      issuer = warp - p.nw - 1; ebase = empty0 + 8 * issuer * D; is_issuing_thread = lane == 0;
    }
  } else {
    if (rank == 0 && p.self_issue) issuer = 0;
    else if (rank >= 1 && (int)rank <= p.nhelpers) issuer = p.self_issue + (int)rank - 1;
    is_issuing_thread = issuer >= 0 && threadIdx.x == 0;
  }

  if (threadIdx.x == 0) {
    p.smid[blockIdx.x] = (int)sm_id();
    if (p.started) atomicAdd(p.started, 1u);
    if (rank == 0)
      for (int s = 0; s < nslots; ++s) {
        mbar_init(full0 + 8 * s, 1);
        mbar_arrive_expect_tx(full0 + 8 * s, p.tile_bytes);  // arm phase 0
      }
    if (p.nlocal) {
      if (rank == 0) for (int j = 0; j < p.nlocal * D; ++j) mbar_init(empty0 + 8 * j, 1);
    } else if (issuer >= 0)
      for (int j = 0; j < D; ++j) mbar_init(empty0 + 8 * j, 1);
    fence_mbar_init();
  }
  cluster_sync_all();

  const uint32_t tile_elems = p.tile_bytes / 4;
  if (is_issuing_thread) {
    const uint32_t K = p.T / H;
    // own CTA: plain shared::cta addresses (valid shared::cluster addresses of
    // the executing CTA); another CTA: mapa. p.self_mapa forces mapa for self.
    const bool local = rank == 0 && !p.self_mapa;
    const uint32_t rfull = local ? full0 : mapa(full0, 0), rslot = local ? slot0 : mapa(slot0, 0);
    const uint64_t base = (uint64_t)cid * p.cluster_stride;
    // incremental indices: no division or 64-bit modulo in the issue loop
    uint64_t g = (base + issuer) % p.window_tiles;
    const uint32_t* gsrc = p.src;
    uint32_t j = 0, ph = 0;
    long long wcyc = 0, icyc = 0;
    for (uint32_t k = 0; k < K; ++k) {
#ifdef BORROW_PSTATS
      const long long a0 = clock64();
#endif
      if (k >= (uint32_t)D) mbar_wait(ebase + 8 * j, ph);
#ifdef BORROW_PSTATS
      const long long a1 = clock64();
#endif
      const uint32_t s = issuer * D + j;
      bulk_g2s(rslot + s * p.tile_bytes, gsrc + g * tile_elems, p.tile_bytes, rfull + 8 * s);
#ifdef BORROW_PSTATS
      icyc += clock64() - a1; wcyc += a1 - a0;
#endif
      g += H; while (g >= p.window_tiles) g -= p.window_tiles;
      if (++j == (uint32_t)D) { j = 0; if (k >= (uint32_t)D) ph ^= 1; }
    }
    if (p.pstats) { p.pstats[2 * (cid * 16 + issuer)] = wcyc; p.pstats[2 * (cid * 16 + issuer) + 1] = icyc; }
  }

  if (rank == 0 && warp >= 1 && warp <= p.nw) {
    const int w = warp - 1;
    unsigned long long t0 = 0;
    long long c0 = 0;
    if (w == 0 && lane == 0) { t0 = gtimer(); c0 = clock64(); }
    uint32_t acc = 0, err = 0;
    const uint64_t base = (uint64_t)cid * p.cluster_stride;
    uint64_t g = (base + w) % p.window_tiles;
    for (uint32_t n = w; n < p.T; n += p.nw, g += p.nw) {
      while (g >= p.window_tiles) g -= p.window_tiles;
      const uint32_t i = n % H, k = n / H, j = k % D, s = i * D + j, m = k / D;
      mbar_wait(full0 + 8 * s, m & 1);
      const uint32_t first = (uint32_t)(g * tile_elems);
      const unsigned char* t = smem + BORROW_BAR_BYTES + (size_t)s * p.tile_bytes;
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
        const uint32_t a = reinterpret_cast<const uint32_t*>(t)[0];
        const uint32_t b = reinterpret_cast<const uint32_t*>(t)[tile_elems - 1];
        acc += a + b;
        if (a != first) ++err;
        if (b != first + tile_elems - 1) ++err;
      }
      __syncwarp();
      if (lane == 0) {
        mbar_arrive_expect_tx(full0 + 8 * s, p.tile_bytes);  // arm the next phase
        const uint32_t irank = p.nlocal ? 0 : (p.self_issue && i == 0) ? 0 : (i - p.self_issue + 1);
        if (p.nlocal) mbar_arrive_local(empty0 + 8 * (i * D + j));
        else if (irank == 0) mbar_arrive_local(empty0 + 8 * j);
        else mbar_arrive_cluster(mapa(empty0 + 8 * j, irank));
      }
    }
    for (int o = 16; o > 0; o >>= 1) {
      acc += __shfl_xor_sync(0xffffffffu, acc, o);
      err += __shfl_xor_sync(0xffffffffu, err, o);
    }
    if (lane == 0) {
      atomicAdd(&p.sums[cid], (unsigned long long)acc);
      atomicAdd(&p.errs[cid], err);
    }
    named_bar(1, p.nw * 32);
    if (w == 0 && lane == 0) {
      const unsigned long long t1 = gtimer();
      const long long c1 = clock64();
      p.times[2 * cid] = t0;
      p.times[2 * cid + 1] = t1;
      p.cycles[cid] = c1 - c0;
    }
  }
  cluster_sync_all();
}

// expected checksum (mod 2^32) of cluster c's received data
static inline uint32_t borrow_expected(const BorrowParams& p, uint64_t c) {
  const uint32_t te = p.tile_bytes / 4;
  uint32_t s = 0;
  const uint32_t tile_inner = (uint32_t)((uint64_t)te * (te - 1) / 2);
  for (uint32_t n = 0; n < p.T; ++n) {
    const uint64_t g = (c * p.cluster_stride + n) % p.window_tiles;
    const uint32_t first = (uint32_t)(g * te);
    if (p.consume) s += first * te + tile_inner;
    else s += first + first + te - 1;
  }
  return s;
}

// Single-thread self-fetch, tmacopy.cu structure (the issuing thread waits for
// its oldest transfer, checks its first and last word, re-issues the slot):
// no producer/consumer hand-off. Launched without clusters, one CTA per SM.
// Uses p.src, window_tiles, cluster_stride (per CTA), T, tile_bytes, D, sums,
// errs, times, cycles, smid (cluster index = blockIdx.x).
__global__ void fused_kernel(BorrowParams p) {
  extern __shared__ __align__(1024) unsigned char smem[];
  const uint32_t full0 = smem_u32(smem), slot0 = full0 + BORROW_BAR_BYTES;
  const uint32_t cid = blockIdx.x, D = p.D, te = p.tile_bytes / 4;
  if (threadIdx.x != 0) return;
  p.smid[blockIdx.x] = (int)sm_id();
  if (p.started) atomicAdd(p.started, 1u);
  for (uint32_t s = 0; s < D; ++s) mbar_init(full0 + 8 * s, 1);
  asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
  const uint64_t base = (uint64_t)cid * p.cluster_stride;
  uint64_t gi = base % p.window_tiles, gc = gi;  // issue and check cursors
  const unsigned long long t0 = gtimer();
  const long long c0 = clock64();
  uint32_t acc = 0, err = 0;
  auto issue = [&](uint32_t s) {
    mbar_arrive_expect_tx(full0 + 8 * s, p.tile_bytes);
    bulk_g2s(slot0 + s * p.tile_bytes, p.src + gi * te, p.tile_bytes, full0 + 8 * s);
    if (++gi >= p.window_tiles) gi = 0;
  };
  for (uint32_t n = 0; n < D && n < p.T; ++n) issue(n);
  uint32_t s = 0, ph = 0;
  for (uint32_t n = 0; n < p.T; ++n) {
    mbar_wait(full0 + 8 * s, ph);
    const uint32_t* t = reinterpret_cast<const uint32_t*>(smem + BORROW_BAR_BYTES + (size_t)s * p.tile_bytes);
    const uint32_t first = (uint32_t)(gc * te), a = t[0], b = t[te - 1];
    acc += a + b; err += (a != first) + (b != first + te - 1);
    if (++gc >= p.window_tiles) gc = 0;
    if (n + D < p.T) issue(s);
    if (++s == D) { s = 0; ph ^= 1; }
  }
  p.sums[cid] = acc; p.errs[cid] = err;
  p.times[2 * cid] = t0; p.times[2 * cid + 1] = gtimer();
  p.cycles[cid] = clock64() - c0;
}
