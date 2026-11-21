// E2 (cluster-TMA borrowing) and E1 (TMA part) on a Hopper GPU.
//
//   borrow_tma [--cluster C] [--helpers h] [--self 0|1] [--consume 0|1]
//              [--depth D] [--tile-kb K] [--clusters N] [--nw W]
//              [--mb-per-consumer X] [--window-mb M] [--gib G] [--reps R]
//              [--smem-kb S] [--tag text]
//   borrow_tma --latency R [--cluster C] [--tile-kb K]
//   borrow_tma --placement C     (print the SMs of every cluster of size C)
//
// See borrow_tma.cuh for the kernel. N = 0 launches as many clusters as can be
// co-resident (cudaOccupancyMaxActiveClusters), i.e. one wave. Every CTA
// reserves S KB of dynamic shared memory (default: the per-block maximum), so
// only one CTA fits per SM; the SM of every CTA is read back (%smid) and the
// count of distinct SMs is printed. --window-mb M > 0 makes every consumer
// read tiles from one M-MB window (L2-resident after a warm-up launch);
// otherwise each consumer streams its own slice of a G-GiB buffer once.
// Time: %globaltimer on CTA 0 from just after the cluster barrier that follows
// barrier init to the last consumed transfer (excludes launch); also
// cudaEvent time of the whole kernel. One untimed launch precedes the R timed.
#include "borrow_tma.cuh"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
    a[i] = (uint32_t)i;
}
__global__ void flush_l2(const uint4* a, size_t n, unsigned* out) {
  uint32_t s = 0;
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x) {
    uint4 v = a[i]; s += v.x ^ v.w;
  }
  if (s == 0x12345678u) out[0] = s;
}

// single-transfer latency: local (CTA 0 fetches into itself) and remote (CTA
// rrank fetches into CTA 0, CTA 0 signals back), plus a signal ping-pong
__global__ void lat_kernel(const uint32_t* src, uint64_t stride_elems, int trials, uint32_t bytes, int rrank,
                           long long* cyc, unsigned long long* ns, int* smid) {
  extern __shared__ __align__(1024) unsigned char smem[];
  const uint32_t rank = cluster_rank();
  const uint32_t full = smem_u32(smem), ping = full + 8, done = full + 16, slot = full + 1024;
  if (threadIdx.x == 0) {
    smid[rank] = (int)sm_id();
    mbar_init(full, 1); mbar_init(ping, 1); mbar_init(done, 1);
    fence_mbar_init();
  }
  cluster_sync_all();
  if (rank == 0 && threadIdx.x == 0) {
    uint32_t ph = 0;
    for (int t = 0; t < trials; ++t) {
      const uint32_t* a = src + (uint64_t)(2 * t) * stride_elems;
      for (int rep = 0; rep < 2; ++rep) {  // cold, then the same address again (L2 hit)
        mbar_arrive_expect_tx(full, bytes);
        const long long c0 = clock64(); const unsigned long long g0 = gtimer();
        bulk_g2s(mapa(slot, 0), a, bytes, mapa(full, 0));
        mbar_wait(full, ph); ph ^= 1;
        const long long c1 = clock64(); const unsigned long long g1 = gtimer();
        cyc[t * 8 + rep] = c1 - c0; ns[t * 8 + rep] = g1 - g0;
      }
    }
  }
  cluster_sync_all();
  if ((int)rank == rrank && threadIdx.x == 0) {
    uint32_t ph = 0;
    for (int t = 0; t < trials; ++t) {
      const uint32_t* a = src + (uint64_t)(2 * t + 1) * stride_elems;
      for (int rep = 0; rep < 2; ++rep) {
        const long long c0 = clock64(); const unsigned long long g0 = gtimer();
        mbar_arrive_expect_tx_cluster(mapa(full, 0), bytes);
        bulk_g2s(mapa(slot, 0), a, bytes, mapa(full, 0));
        mbar_wait(done, ph); ph ^= 1;
        const long long c1 = clock64(); const unsigned long long g1 = gtimer();
        cyc[t * 8 + 2 + rep] = c1 - c0; ns[t * 8 + 2 + rep] = g1 - g0;
      }
      const long long c0 = clock64(); const unsigned long long g0 = gtimer();
      mbar_arrive_cluster(mapa(ping, 0));
      mbar_wait(done, ph); ph ^= 1;
      const long long c1 = clock64(); const unsigned long long g1 = gtimer();
      cyc[t * 8 + 4] = c1 - c0; ns[t * 8 + 4] = g1 - g0;
    }
  }
  if (rank == 0 && threadIdx.x == 0) {
    uint32_t fph = (2 * trials) & 1, pph = 0;
    for (int t = 0; t < trials; ++t) {
      for (int rep = 0; rep < 2; ++rep) {
        mbar_wait(full, fph); fph ^= 1;
        if (rep == 0 && reinterpret_cast<const uint32_t*>(smem + 1024)[0] != (uint32_t)((2 * t + 1) * stride_elems))
          cyc[t * 8 + 7] = 1;  // data check
        mbar_arrive_cluster(mapa(done, rrank));
      }
      mbar_wait(ping, pph); pph ^= 1;
      mbar_arrive_cluster(mapa(done, rrank));
    }
  }
  cluster_sync_all();
}

static double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v.empty() ? 0 : v[v.size() / 2];
}

int main(int argc, char** argv) {
  int C = 1, helpers = -1, self = 1, consume = 0, D = 4, tile_kb = 16, nclusters = 1, nw = 4, reps = 5;
  int smem_kb = 0, latency = 0, placement = 0, self_mapa = 0, fused = 0, nlocal = 0;
  double mbpc = 1024, window_mb = 0, gib = 16;
  std::string tag = "-";
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--cluster")) C = atoi(argv[++i]);
    else if (is("--helpers")) helpers = atoi(argv[++i]);
    else if (is("--self")) self = atoi(argv[++i]);
    else if (is("--consume")) consume = atoi(argv[++i]);
    else if (is("--depth")) D = atoi(argv[++i]);
    else if (is("--tile-kb")) tile_kb = atoi(argv[++i]);
    else if (is("--clusters")) nclusters = atoi(argv[++i]);
    else if (is("--nw")) nw = atoi(argv[++i]);
    else if (is("--mb-per-consumer")) mbpc = atof(argv[++i]);
    else if (is("--window-mb")) window_mb = atof(argv[++i]);
    else if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--smem-kb")) smem_kb = atoi(argv[++i]);
    else if (is("--latency")) latency = atoi(argv[++i]);
    else if (is("--placement")) placement = atoi(argv[++i]);
    else if (is("--tag")) tag = argv[++i];
    else if (is("--self-mapa")) self_mapa = atoi(argv[++i]);
    else if (is("--fused")) fused = atoi(argv[++i]);
    else if (is("--local-issuers")) nlocal = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  if (helpers < 0) helpers = C - 1;
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  const uint32_t tile_bytes = (uint32_t)tile_kb * 1024;
  const size_t elems = (size_t)(gib * (1ull << 30)) / 4;
  uint32_t* buf;
  CK(cudaMalloc(&buf, elems * 4));
  init_buf<<<prop.multiProcessorCount * 4, 1024>>>(buf, elems);
  CK(cudaDeviceSynchronize());
  const int maxsmem = (int)prop.sharedMemPerBlockOptin;

  if (latency) {
    if (C < 2) C = 2;
    uint4* fl; unsigned* fo;
    const size_t fl_bytes = 512ull << 20;
    CK(cudaMalloc(&fl, fl_bytes)); CK(cudaMalloc(&fo, 4));
    long long* cyc; unsigned long long* ns; int* sm;
    CK(cudaMallocManaged(&cyc, latency * 8 * sizeof(long long)));
    CK(cudaMallocManaged(&ns, latency * 8 * sizeof(unsigned long long)));
    CK(cudaMallocManaged(&sm, 32 * sizeof(int)));
    memset(cyc, 0, latency * 8 * sizeof(long long));
    const uint64_t stride = (elems / (2 * latency + 1)) & ~(uint64_t)1023;
    CK(cudaFuncSetAttribute(lat_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, maxsmem));
    CK(cudaFuncSetAttribute(lat_kernel, cudaFuncAttributeNonPortableClusterSizeAllowed, 1));
    flush_l2<<<prop.multiProcessorCount * 4, 1024>>>(fl, fl_bytes / 16, fo);
    CK(cudaDeviceSynchronize());
    cudaLaunchConfig_t cfg = {};
    cudaLaunchAttribute at[1];
    at[0].id = cudaLaunchAttributeClusterDimension;
    at[0].val.clusterDim.x = C; at[0].val.clusterDim.y = 1; at[0].val.clusterDim.z = 1;
    cfg.gridDim = dim3(C); cfg.blockDim = dim3(32); cfg.dynamicSmemBytes = maxsmem;
    cfg.attrs = at; cfg.numAttrs = 1;
    CK(cudaLaunchKernelEx(&cfg, lat_kernel, (const uint32_t*)buf, stride, latency, tile_bytes, C - 1, cyc, ns, sm));
    CK(cudaDeviceSynchronize());
    const char* names[5] = {"local_cold", "local_warm", "remote_cold", "remote_warm", "pingpong"};
    int bad = 0;
    for (int t = 0; t < latency; ++t) bad += cyc[t * 8 + 7] != 0;
    printf("gpu=%s test=latency cluster=%d remote_rank=%d tile_kb=%d trials=%d sm_rank0=%d sm_remote=%d data_errs=%d\n",
           prop.name, C, C - 1, tile_kb, latency, sm[0], sm[C - 1], bad);
    for (int k = 0; k < 5; ++k) {
      std::vector<double> c, n;
      for (int t = 1; t < latency; ++t) { c.push_back((double)cyc[t * 8 + k]); n.push_back((double)ns[t * 8 + k]); }
      std::vector<double> cs = c; std::sort(cs.begin(), cs.end());
      printf("  %-12s median_cycles=%.0f min=%.0f max=%.0f median_ns=%.0f\n", names[k], median(c), cs.front(),
             cs.back(), median(n));
    }
    return 0;
  }

  if (placement) {
    // one cluster of size C per wave slot, read back the SMs (GPC inference)
    int* sm;
    const int maxcl = 64;
    CK(cudaMallocManaged(&sm, maxcl * placement * sizeof(int)));
    BorrowParams p = {};
    // a do-nothing launch of the borrow kernel: H = 1 issuer with T = 0
    p.src = buf; p.window_tiles = 1; p.T = 0; p.tile_bytes = tile_bytes; p.D = 1; p.self_issue = 1; p.nhelpers = 0;
    p.nw = 1;
    unsigned long long *sums, *times; unsigned* errs; long long* cyc;
    CK(cudaMallocManaged(&sums, maxcl * 8)); CK(cudaMallocManaged(&times, maxcl * 16));
    CK(cudaMallocManaged(&errs, maxcl * 4)); CK(cudaMallocManaged(&cyc, maxcl * 8));
    p.sums = sums; p.times = times; p.errs = errs; p.cycles = cyc; p.smid = sm;
    CK(cudaFuncSetAttribute(borrow_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, maxsmem));
    CK(cudaFuncSetAttribute(borrow_kernel, cudaFuncAttributeNonPortableClusterSizeAllowed, 1));
    cudaLaunchConfig_t cfg = {};
    cudaLaunchAttribute at[1];
    at[0].id = cudaLaunchAttributeClusterDimension;
    at[0].val.clusterDim.x = placement; at[0].val.clusterDim.y = 1; at[0].val.clusterDim.z = 1;
    cfg.blockDim = dim3(64); cfg.dynamicSmemBytes = maxsmem; cfg.attrs = at; cfg.numAttrs = 1;
    cfg.gridDim = dim3(placement);
    int ncl = 0;
    cudaError_t e = cudaOccupancyMaxActiveClusters(&ncl, borrow_kernel, &cfg);
    printf("gpu=%s test=placement cluster=%d max_active_clusters=%d (%s)\n", prop.name, placement, ncl,
           cudaGetErrorString(e));
    if (e != cudaSuccess || ncl == 0) return 0;
    ncl = std::min(ncl, maxcl);
    cfg.gridDim = dim3(ncl * placement);
    CK(cudaLaunchKernelEx(&cfg, borrow_kernel, p));
    CK(cudaDeviceSynchronize());
    for (int c = 0; c < ncl; ++c) {
      printf("  cluster %2d sms:", c);
      std::vector<int> v(sm + c * placement, sm + (c + 1) * placement);
      std::sort(v.begin(), v.end());
      for (int x : v) printf(" %d", x);
      printf("\n");
    }
    return 0;
  }

  if (fused) { C = 1; helpers = 0; self = 1; consume = 0; }
  if (nlocal) { self = 0; helpers = 0; }
  const int H = nlocal ? nlocal : self + helpers;
  if (H < 1 || helpers > C - 1 || H * D > BORROW_MAX_SLOTS) { fprintf(stderr, "bad issuer/depth configuration\n"); return 1; }
  // every slot must always be handled by the same consumer warp (transfer n goes
  // to warp n % nw and to slot f(n % (H*D))), so nw must divide H*D
  {
    const int ns = H * D, want = nw;
    int best = 0;
    for (int d = want; d <= 16 && !best; ++d) if (ns % d == 0) best = d;
    for (int d = want; d >= 1 && !best; --d) if (ns % d == 0) best = d;
    nw = best;
  }
  const size_t need = BORROW_BAR_BYTES + (size_t)H * D * tile_bytes;
  size_t smem = smem_kb ? (size_t)smem_kb * 1024 : (size_t)maxsmem;
  if (smem < need) {
    printf("gpu=%s tag=%s SKIP cluster=%d helpers=%d self=%d depth=%d tile_kb=%d: ring of %d slots needs %zu B > %zu B\n",
           prop.name, tag.c_str(), fused, C, helpers, self, D, tile_kb, H * D, need, smem);
    return 0;
  }
  CK(cudaFuncSetAttribute(borrow_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  CK(cudaFuncSetAttribute(fused_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  if (C > 8) {
    cudaError_t e = cudaFuncSetAttribute(borrow_kernel, cudaFuncAttributeNonPortableClusterSizeAllowed, 1);
    if (e != cudaSuccess) { printf("non-portable cluster size not allowed: %s\n", cudaGetErrorString(e)); return 0; }
  }
  const int threads = 32 * (1 + nw + nlocal);
  cudaLaunchConfig_t cfg = {};
  cudaLaunchAttribute at[1];
  at[0].id = cudaLaunchAttributeClusterDimension;
  at[0].val.clusterDim.x = C; at[0].val.clusterDim.y = 1; at[0].val.clusterDim.z = 1;
  cfg.blockDim = dim3(threads); cfg.dynamicSmemBytes = smem; cfg.attrs = at; cfg.numAttrs = 1;
  cfg.gridDim = dim3(C);
  int maxcl = 0, occ = 0;
  CK(cudaOccupancyMaxActiveClusters(&maxcl, borrow_kernel, &cfg));
  if (fused) {
    CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&occ, fused_kernel, 32, smem));
    maxcl = occ * prop.multiProcessorCount;
  } else
    CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&occ, borrow_kernel, threads, smem));
  if (nclusters == 0) nclusters = maxcl;
  if (nclusters > maxcl) printf("warning: %d clusters > %d co-resident\n", nclusters, maxcl);
  cfg.gridDim = dim3(nclusters * C);

  const uint64_t buf_tiles = elems * 4 / tile_bytes;
  BorrowParams p = {};
  p.src = buf; p.tile_bytes = tile_bytes; p.D = D; p.self_issue = self; p.nhelpers = helpers; p.consume = consume;
  p.nw = nw; p.self_mapa = self_mapa; p.nlocal = nlocal;
  uint64_t T;
  if (window_mb > 0) {
    p.window_tiles = std::max<uint64_t>(1, (uint64_t)(window_mb * (1 << 20)) / tile_bytes);
    p.cluster_stride = p.window_tiles / nclusters;  // different start, same window
    T = (uint64_t)(mbpc * (1 << 20)) / tile_bytes;
  } else {
    p.window_tiles = buf_tiles;
    T = std::min<uint64_t>((uint64_t)(mbpc * (1 << 20)) / tile_bytes, buf_tiles / nclusters);
    p.cluster_stride = T;
  }
  T -= T % H;
  p.T = (uint32_t)T;
  unsigned long long *sums, *times; unsigned* errs; long long* cyc; int* sm;
  CK(cudaMallocManaged(&sums, nclusters * 8)); CK(cudaMallocManaged(&times, nclusters * 16));
  CK(cudaMallocManaged(&errs, nclusters * 4)); CK(cudaMallocManaged(&cyc, nclusters * 8));
  CK(cudaMallocManaged(&sm, nclusters * C * sizeof(int)));
  p.sums = sums; p.times = times; p.errs = errs; p.cycles = cyc; p.smid = sm;
  long long* pst;
  CK(cudaMallocManaged(&pst, nclusters * 16 * 2 * sizeof(long long)));
  memset(pst, 0, nclusters * 16 * 2 * sizeof(long long));
  p.pstats = pst;
  std::vector<uint32_t> expect(nclusters);
  for (int c = 0; c < nclusters; ++c) expect[c] = borrow_expected(p, c);

  cudaEvent_t e0, e1;
  CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
  std::vector<double> per_consumer, aggregate, event_agg, minc, maxc;
  int bad = 0, distinct = 0, cluster_distinct_ok = 1;
  for (int r = 0; r <= reps; ++r) {
    memset(sums, 0, nclusters * 8); memset(errs, 0, nclusters * 4);
    CK(cudaEventRecord(e0));
    if (fused) fused_kernel<<<nclusters, 32, smem>>>(p);
    else CK(cudaLaunchKernelEx(&cfg, borrow_kernel, p));
    CK(cudaGetLastError());
    CK(cudaEventRecord(e1));
    CK(cudaDeviceSynchronize());
    float ms; CK(cudaEventElapsedTime(&ms, e0, e1));
    for (int c = 0; c < nclusters; ++c)
      if (errs[c] || (uint32_t)sums[c] != expect[c]) ++bad;
    if (r == 0) {
      std::set<int> all(sm, sm + nclusters * C);
      distinct = (int)all.size();
      for (int c = 0; c < nclusters; ++c) {
        std::set<int> one(sm + c * C, sm + (c + 1) * C);
        if ((int)one.size() != C) cluster_distinct_ok = 0;
      }
      continue;  // warm-up (also fills the L2 window in L2 mode)
    }
    unsigned long long tmin = ~0ull, tmax = 0;
    double s = 0, mn = 1e30, mx = 0;
    for (int c = 0; c < nclusters; ++c) {
      const double g = (double)T * tile_bytes / (double)(times[2 * c + 1] - times[2 * c]);
      s += g; mn = std::min(mn, g); mx = std::max(mx, g);
      tmin = std::min(tmin, times[2 * c]); tmax = std::max(tmax, times[2 * c + 1]);
    }
    per_consumer.push_back(s / nclusters);
    minc.push_back(mn); maxc.push_back(mx);
    aggregate.push_back((double)T * tile_bytes * nclusters / (double)(tmax - tmin));
    event_agg.push_back((double)T * tile_bytes * nclusters / (ms * 1e6));
  }
  {
    const uint32_t K = p.T / H;
    if (!fused) printf("issuer0 of cluster0: wait_cycles_per_transfer=%.0f issue_cycles_per_transfer=%.0f consumer_cycles_total=%lld\n",
           (double)pst[0] / K, (double)pst[1] / K, cyc[0]);
  }
  auto mm = [](const std::vector<double>& v) { return std::make_pair(*std::min_element(v.begin(), v.end()),
                                                                       *std::max_element(v.begin(), v.end())); };
  printf("gpu=%s tag=%s fused=%d local_issuers=%d cluster=%d helpers=%d self=%d issuers=%d consume=%d depth=%d tile_kb=%d nw=%d clusters=%d "
         "ctas=%d distinct_sms=%d cluster_on_distinct_sms=%d occ_blocks_per_sm=%d max_active_clusters=%d smem_kb=%zu "
         "self_mapa=%d window_mb=%.0f mb_per_consumer=%.1f reps=%d check=%s bad=%d "
         "consumer_gbs_med=%.1f consumer_gbs_range=%.1f-%.1f slowest_consumer=%.1f fastest_consumer=%.1f "
         "aggregate_gbs=%.1f event_aggregate_gbs=%.1f\n",
         prop.name, tag.c_str(), fused, nlocal, C, helpers, self, H, consume, D, tile_kb, nw, nclusters, nclusters * C, distinct,
         cluster_distinct_ok, occ, maxcl, smem / 1024, self_mapa, window_mb, (double)T * tile_bytes / (1 << 20), reps,
         bad ? "FAIL" : "ok", bad, median(per_consumer), mm(per_consumer).first, mm(per_consumer).second,
         median(minc), median(maxc), median(aggregate), median(event_agg));
  return 0;
}
