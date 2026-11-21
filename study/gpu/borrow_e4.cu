// E4: does __syncthreads() of one CTA wait for other CTAs' global loads on
// the same SM? Load-staged loop, many CTAs per SM, lockstep vs staggered starts.
//
//   borrow_e4 --mode stage [--stagger none|linear|random] [--per-sm K]
//             [--iters N] [--reps R]
//   borrow_e4 --mode probe --neighbours idle|loads|loads_bar [--iters N]
//
// stage: grid = SMs x K CTAs of 128 threads (4 warps). Per iteration a CTA
//   loads an 8 KB chunk of a 16 GiB buffer (load u = 0..3 of thread t reads
//   the 16 B at chunk + u*2 KB + t*16: a warp covers 512 B contiguous,
//   neighbouring warps neighbouring 512 B, successive loads 2 KB apart), stores
//   it to shared memory, __syncthreads(), every thread reads the 64 B another
//   thread (t+37 mod 128) stored and folds it into a checksum, __syncthreads().
//   Chunks are interleaved over the grid (chunk = it * grid + block), so the
//   whole grid streams the buffer once (16 GiB >> 60 MB L2).
//   Dynamic shared memory: 13 KB for K = 16 (16 x (13 + 1 reserved) KB fits the
//   228 KB SM), or 200 KB for K = 1 (then only one fits).
//   stagger: the CTA's arrival rank k on its SM (atomic counter per %smid)
//   delays its first iteration by k * P / K cycles (linear) or by a hashed
//   random value in [0, P) (random), P = the lockstep iteration period passed
//   with --period (cycles).
//   Times: clock64 per iteration on thread 0; steady = iterations
//   [N/4, 3N/4) (all CTAs of an SM running then, also when staggered).
// probe: per SM, the first CTA to arrive is a probe that runs a shared-memory-
//   only loop (store, __syncthreads, read neighbour, __syncthreads), timed per
//   iteration after a 100 us delay; the other K-1 CTAs are neighbours:
//   idle (exit at once), loads (the stage loop's loads with no barrier) or
//   loads_bar (the full stage loop). Neighbours run long enough to outlast it.
//   --probe-kind smem_bar (default) | bar_only (two barriers, register work)
//   | smem_only (store and read back own shared word, no barrier).
// --active-sms N: CTAs on SMs with %smid >= N exit at once (the grid still
//   places K CTAs on every SM), so only N SMs load and DRAM is not the limit.
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
    a[i] = (uint32_t)i;
}
__device__ __forceinline__ uint32_t sm_id() { uint32_t r; asm volatile("mov.u32 %0, %%smid;" : "=r"(r)); return r; }
__device__ __forceinline__ void spin(long long cyc) { const long long t = clock64(); while (clock64() - t < cyc) {} }

struct E4 {
  const uint4* src; uint64_t nchunks; int iters; int stagger; int K; long long period;
  int probe; int neighbours; int probe_iters; int probe_kind; int active_sms;
  unsigned* counter;     // per SM arrival counter
  int* smid; int* rank;  // per CTA
  unsigned* sums;        // per CTA
  unsigned* itcyc;       // per CTA x iters
  unsigned long long* span;  // per CTA: first-iteration start, last-iteration end (globaltimer)
};

__device__ __forceinline__ unsigned long long gtimer() {
  unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t;
}

__global__ void __launch_bounds__(128, 16) e4_kernel(E4 p) {
  extern __shared__ uint4 sh[];
  const int t = threadIdx.x;
  __shared__ int role_k;
  if (t == 0) {
    const uint32_t s = sm_id();
    role_k = (int)atomicAdd(&p.counter[s], 1u);
    p.smid[blockIdx.x] = (int)s; p.rank[blockIdx.x] = role_k;
  }
  __syncthreads();
  const int k = role_k;
  uint32_t acc = 0;
  if (p.active_sms && p.smid[blockIdx.x] >= p.active_sms) return;  // only SMs 0..active_sms-1 work
  const bool is_probe = p.probe && k == 0;
  if (p.probe && !is_probe && p.neighbours == 0) return;
  if (is_probe) {
    spin(200000);  // ~100 us: let the neighbours start
    for (int it = 0; it < p.probe_iters; ++it) {
      const long long c0 = clock64();
      if (p.probe_kind == 0) {         // smem_bar: store, barrier, read a neighbour's word, barrier
        sh[t] = make_uint4(acc + it, t, it, 0);
        __syncthreads();
        const uint4 v = sh[(t + 37) & 127];
        acc += v.x + v.y;
        __syncthreads();
      } else if (p.probe_kind == 1) {  // bar_only: two barriers, register work only
        acc = acc * 3 + it;
        __syncthreads();
        acc ^= t;
        __syncthreads();
      } else {                         // smem_only: store and read back own word, no barrier
        sh[t] = make_uint4(acc + it, t, it, 0);
        const volatile uint32_t* vp = reinterpret_cast<volatile uint32_t*>(&sh[t]);
        acc += vp[0] + vp[1];
        __syncwarp();
      }
      if (t == 0) p.itcyc[(size_t)blockIdx.x * p.iters + it] = (unsigned)(clock64() - c0);
    }
    p.sums[blockIdx.x] = acc;
    return;
  }
  if (!p.probe && p.stagger == 1) spin((long long)k * p.period / p.K);
  if (!p.probe && p.stagger == 2) {
    uint32_t h = blockIdx.x * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    spin((long long)(h % 1024u) * p.period / 1024);
  }
  const bool bar = !p.probe || p.neighbours == 2;
  unsigned long long g0 = 0;
  if (t == 0) g0 = gtimer();
  for (int it = 0; it < p.iters; ++it) {
    const long long c0 = clock64();
    const uint64_t chunk = ((uint64_t)it * gridDim.x + blockIdx.x) % p.nchunks;
    const uint4* q = p.src + chunk * 512 + t;  // 8 KB chunk = 512 uint4
    uint4 v[4];
#pragma unroll
    for (int u = 0; u < 4; ++u) v[u] = q[u * 128];
    if (bar) {
#pragma unroll
      for (int u = 0; u < 4; ++u) sh[u * 128 + t] = v[u];
      __syncthreads();
      const int o = (t + 37) & 127;
#pragma unroll
      for (int u = 0; u < 4; ++u) { const uint4 w = sh[u * 128 + o]; acc += w.x + w.y + w.z + w.w; }
      __syncthreads();
    } else {
#pragma unroll
      for (int u = 0; u < 4; ++u) acc += v[u].x + v[u].y + v[u].z + v[u].w;
    }
    if (t == 0 && !p.probe) p.itcyc[(size_t)blockIdx.x * p.iters + it] = (unsigned)(clock64() - c0);
  }
  if (t == 0) { p.span[2 * blockIdx.x] = g0; p.span[2 * blockIdx.x + 1] = gtimer(); }
  atomicAdd(&p.sums[blockIdx.x], acc);
}

static double pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
}

int main(int argc, char** argv) {
  std::string mode = "stage", stagger = "none", neigh = "idle";
  int K = 16, iters = 400, reps = 3, probe_iters = 2000, probe_kind = 0, active_sms = 0;
  long long period = 0;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--mode")) mode = argv[++i];
    else if (is("--stagger")) stagger = argv[++i];
    else if (is("--neighbours")) neigh = argv[++i];
    else if (is("--per-sm")) K = atoi(argv[++i]);
    else if (is("--iters")) iters = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--period")) period = atoll(argv[++i]);
    else if (is("--probe-kind")) { std::string k = argv[++i]; probe_kind = k == "bar_only" ? 1 : k == "smem_only" ? 2 : 0; }
    else if (is("--active-sms")) active_sms = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  const int sms = prop.multiProcessorCount;
  const size_t bytes = 16ull << 30;
  uint32_t* buf;
  CK(cudaMalloc(&buf, bytes));
  init_buf<<<sms * 4, 1024>>>(buf, bytes / 4);
  CK(cudaDeviceSynchronize());
  E4 p = {};
  p.src = (const uint4*)buf; p.nchunks = bytes / 8192; p.K = K; p.period = period;
  p.stagger = stagger == "linear" ? 1 : stagger == "random" ? 2 : 0;
  p.probe = mode == "probe"; p.neighbours = neigh == "loads" ? 1 : neigh == "loads_bar" ? 2 : 0;
  if (p.probe) { iters = std::max(iters, probe_iters); }
  p.iters = p.probe && p.neighbours ? 20000 : iters;
  p.probe_iters = probe_iters; p.probe_kind = probe_kind; p.active_sms = active_sms;
  if (p.probe) p.iters = std::max(p.iters, probe_iters);
  const int grid = sms * K;
  const size_t smem = K == 1 ? 200 * 1024 : (K <= 16 ? 13 * 1024 : 8 * 1024 + 256);
  CK(cudaFuncSetAttribute(e4_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  int occ = 0;
  CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&occ, e4_kernel, 128, smem));
  cudaFuncAttributes fa; CK(cudaFuncGetAttributes(&fa, e4_kernel));
  CK(cudaMalloc(&p.counter, sms * 4 * 2));
  CK(cudaMallocManaged(&p.smid, grid * 4)); CK(cudaMallocManaged(&p.rank, grid * 4));
  CK(cudaMallocManaged(&p.sums, grid * 4));
  CK(cudaMallocManaged(&p.itcyc, (size_t)grid * p.iters * 4));
  CK(cudaMallocManaged(&p.span, grid * 16));
  // expected checksum (stage / loads neighbours): sum of the words of every chunk read
  auto expect = [&](int b) {
    uint32_t s = 0;
    for (int it = 0; it < p.iters; ++it) {
      const uint64_t c = ((uint64_t)it * grid + b) % p.nchunks, e0 = c * 2048, n = 2048;
      s += (uint32_t)(n * e0 + n * (n - 1) / 2);
    }
    return s;
  };
  cudaEvent_t e0, e1; CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
  for (int r = 0; r <= reps; ++r) {
    CK(cudaMemset(p.counter, 0, sms * 8));
    memset(p.sums, 0, grid * 4);
    CK(cudaEventRecord(e0));
    e4_kernel<<<grid, 128, smem>>>(p);
    CK(cudaGetLastError());
    CK(cudaEventRecord(e1));
    CK(cudaDeviceSynchronize());
    if (r == 0) continue;  // warm-up
    float ms; CK(cudaEventElapsedTime(&ms, e0, e1));
    std::map<int, int> per;
    for (int b = 0; b < grid; ++b) per[p.smid[b]]++;
    int mn = 1 << 30, mx = 0;
    for (auto& kv : per) { mn = std::min(mn, kv.second); mx = std::max(mx, kv.second); }
    int bad = 0;
    std::vector<double> cta_med, pooled, probe_it;
    if (!p.probe) {
      int nact = 0;
      for (int b = 0; b < grid; ++b) {
        if (active_sms && p.smid[b] >= active_sms) continue;
        ++nact;
        bad += p.sums[b] != expect(b);
        std::vector<double> v;
        for (int it = p.iters / 4; it < 3 * p.iters / 4; ++it) v.push_back(p.itcyc[(size_t)b * p.iters + it]);
        cta_med.push_back(pct(v, 0.5));
        pooled.insert(pooled.end(), v.begin(), v.end());
      }
      // per-SM bytes/s over the steady window: K CTAs x 8 KB per median iteration at 1.98 GHz
      const double ghz = prop.clockRate * 1e-6;
      const double per_sm = K * 8192.0 / (pct(cta_med, 0.5) / ghz);  // B/ns = GB/s
      const double total = (double)nact * p.iters * 8192 / (ms * 1e6);
      const size_t nsm_act = active_sms ? (size_t)active_sms : per.size();
      printf("gpu=%s test=e4_stage stagger=%s period=%lld per_sm=%d grid=%d occ_blocks_per_sm=%d regs=%d smem_b=%zu "
             "ctas_per_sm_min=%d ctas_per_sm_max=%d active_sms=%zu active_ctas=%d iters=%d check=%s "
             "cta_median_iter_cycles: p25=%.0f p50=%.0f p75=%.0f pooled_iter_cycles: p25=%.0f p50=%.0f p75=%.0f p95=%.0f "
             "per_sm_gbs_from_median=%.1f whole_kernel_gbs=%.1f whole_kernel_per_sm_gbs=%.1f kernel_ms=%.2f\n",
             prop.name, stagger.c_str(), period, K, grid, occ, fa.numRegs, smem, mn, mx, nsm_act, nact, p.iters,
             bad ? "FAIL" : "ok", pct(cta_med, .25), pct(cta_med, .5), pct(cta_med, .75), pct(pooled, .25),
             pct(pooled, .5), pct(pooled, .75), pct(pooled, .95), per_sm, total, total / nsm_act, ms);
    } else {
      for (int b = 0; b < grid; ++b)
        if (p.rank[b] == 0)
          for (int it = 100; it < probe_iters; ++it) probe_it.push_back(p.itcyc[(size_t)b * p.iters + it]);
      int nb_bad = 0;
      if (p.neighbours)
        for (int b = 0; b < grid; ++b) if (p.rank[b] != 0) nb_bad += p.sums[b] != expect(b);
      printf("gpu=%s test=e4_probe probe_kind=%s neighbours=%s per_sm=%d grid=%d occ_blocks_per_sm=%d ctas_per_sm_min=%d "
             "ctas_per_sm_max=%d probes=%zu neighbour_check=%s probe_iter_cycles(2 barriers): p25=%.0f p50=%.0f p75=%.0f "
             "p95=%.0f kernel_ms=%.2f\n",
             prop.name, probe_kind == 0 ? "smem_bar" : probe_kind == 1 ? "bar_only" : "smem_only", neigh.c_str(), K,
             grid, occ, mn, mx, probe_it.size() / (probe_iters - 100),
             p.neighbours ? (nb_bad ? "FAIL" : "ok") : "n/a", pct(probe_it, .25), pct(probe_it, .5),
             pct(probe_it, .75), pct(probe_it, .95), ms);
    }
  }
  return 0;
}
