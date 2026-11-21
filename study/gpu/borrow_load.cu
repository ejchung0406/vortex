// E1, plain loads: per-SM read bandwidth with the data in DRAM or in L2.
//
//   borrow_load [--cg 0|1] [--blocks B] [--threads T] [--unroll U] [--mb-per-block X]
//               [--window-mb W] [--passes P] [--gib G] [--reps R] [--smem-kb S]
//
// One block per SM (S KB of dynamic shared memory, default 120, so two blocks
// of T = 1024 threads cannot share an SM; the SM of every block is read back).
// Each thread issues U independent 16-byte loads (ld.global.v4) per step; a
// step of the block covers T * 16 B contiguous bytes, so U * T * 16 B are in
// flight per SM (U = 1, 2, 4, 8 with T = 1024: 16, 32, 64, 128 KB).
//   DRAM mode (W = 0): block b reads its own X-MB slice of a G-GiB buffer once.
//   L2 mode   (W > 0): every block reads the same W-MB window P times (each
//                       block starts at a different offset); one untimed
//                       launch first brings the window into L2.
// --cg 1 loads with ld.global.cg (__ldcg: cache in L2 only, not L1).
// Time: %globaltimer per block between two __syncthreads (excludes launch),
// and cudaEvent for the kernel. Checksum: sum of all words (a[e] = e) mod 2^32.
#include <cuda_runtime.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
    a[i] = (uint32_t)i;
}

__device__ __forceinline__ unsigned long long gtimer() {
  unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t;
}

// steps of T uint4; block starts at step `start` of a region of `nsteps` steps
// beginning at `base` (in uint4), reads `count` steps with wrap-around
template <int U, int CG>
__global__ void load_kernel(const uint4* __restrict__ a, uint64_t region_steps, uint64_t block_stride_steps,
                            uint64_t base_mult, uint64_t count, unsigned* sums, unsigned long long* times,
                            int* smid) {
  const int t = threadIdx.x, T = blockDim.x;
  const uint64_t base = (uint64_t)blockIdx.x * base_mult * T;           // DRAM: own slice
  uint64_t step = ((uint64_t)blockIdx.x * block_stride_steps) % region_steps;  // L2: rotated start
  uint32_t acc[U];
  for (int u = 0; u < U; ++u) acc[u] = 0;
  __syncthreads();
  unsigned long long t0 = 0;
  if (t == 0) { t0 = gtimer(); int s; asm volatile("mov.u32 %0, %%smid;" : "=r"(s)); smid[blockIdx.x] = s; }
  for (uint64_t i = 0; i < count; i += U) {
    uint4 v[U];
#pragma unroll
    for (int u = 0; u < U; ++u) {
      uint64_t st = step + u; if (st >= region_steps) st -= region_steps;
      v[u] = CG ? __ldcg(&a[base + st * T + t]) : a[base + st * T + t];
    }
#pragma unroll
    for (int u = 0; u < U; ++u) acc[u] += v[u].x + v[u].y + v[u].z + v[u].w;
    step += U; if (step >= region_steps) step -= region_steps;
  }
  uint32_t r = 0;
  for (int u = 0; u < U; ++u) r += acc[u];
  atomicAdd(&sums[blockIdx.x], r);
  __syncthreads();
  if (t == 0) { times[2 * blockIdx.x] = t0; times[2 * blockIdx.x + 1] = gtimer(); }
}

// sum of e for e in [s, s + n), mod 2^32
static uint32_t range_sum(uint64_t s, uint64_t n) {
  unsigned __int128 v = (unsigned __int128)n * s + (unsigned __int128)n * (n - 1) / 2;
  return (uint32_t)(uint64_t)v;
}

int main(int argc, char** argv) {
  int blocks = 1, threads = 1024, U = 4, reps = 5, smem_kb = 120, passes = 20, cg = 0;
  double mbpb = 1024, window_mb = 0, gib = 16;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--blocks")) blocks = atoi(argv[++i]);
    else if (is("--threads")) threads = atoi(argv[++i]);
    else if (is("--unroll")) U = atoi(argv[++i]);
    else if (is("--mb-per-block")) mbpb = atof(argv[++i]);
    else if (is("--window-mb")) window_mb = atof(argv[++i]);
    else if (is("--passes")) passes = atoi(argv[++i]);
    else if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--smem-kb")) smem_kb = atoi(argv[++i]);
    else if (is("--cg")) cg = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  const size_t elems = (size_t)(gib * (1ull << 30)) / 4;
  uint32_t* buf;
  CK(cudaMalloc(&buf, elems * 4));
  init_buf<<<prop.multiProcessorCount * 4, 1024>>>(buf, elems);
  CK(cudaDeviceSynchronize());
  const uint64_t step_bytes = (uint64_t)threads * 16;
  uint64_t region_steps, stride_steps, base_mult, count;
  if (window_mb > 0) {
    region_steps = (uint64_t)(window_mb * (1 << 20)) / step_bytes;
    stride_steps = region_steps / blocks;
    base_mult = 0;
    count = region_steps * passes;
  } else {
    region_steps = std::min<uint64_t>((uint64_t)(mbpb * (1 << 20)), elems * 4 / blocks) / step_bytes;
    stride_steps = 0;
    base_mult = region_steps;
    count = region_steps;
  }
  count -= count % U;
  // expected per block
  std::vector<uint32_t> expect(blocks);
  for (int b = 0; b < blocks; ++b) {
    uint32_t s = 0;
    const uint64_t base = (uint64_t)b * base_mult;  // in steps
    uint64_t st = (b * stride_steps) % region_steps;
    // count steps starting at st with wrap, each step = threads*4 words
    uint64_t left = count;
    while (left) {
      const uint64_t n = std::min(left, region_steps - st);
      s += range_sum((base + st) * threads * 4, n * threads * 4);
      left -= n; st = 0;
    }
    expect[b] = s;
  }
  unsigned* sums; unsigned long long* times; int* sm;
  CK(cudaMallocManaged(&sums, blocks * 4)); CK(cudaMallocManaged(&times, blocks * 16));
  CK(cudaMallocManaged(&sm, blocks * 4));
  auto kern = cg ? (U == 1 ? load_kernel<1, 1> : U == 2 ? load_kernel<2, 1> : U == 4 ? load_kernel<4, 1>
                     : U == 8 ? load_kernel<8, 1> : U == 16 ? load_kernel<16, 1> : nullptr)
                  : (U == 1 ? load_kernel<1, 0> : U == 2 ? load_kernel<2, 0> : U == 4 ? load_kernel<4, 0>
                     : U == 8 ? load_kernel<8, 0> : U == 16 ? load_kernel<16, 0> : nullptr);
  if (!kern) { fprintf(stderr, "unroll must be 1, 2, 4, 8 or 16\n"); return 1; }
  const size_t smem = (size_t)smem_kb * 1024;
  CK(cudaFuncSetAttribute(kern, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  int occ = 0;
  CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&occ, kern, threads, smem));
  cudaFuncAttributes fa;
  CK(cudaFuncGetAttributes(&fa, kern));
  cudaEvent_t e0, e1;
  CK(cudaEventCreate(&e0)); CK(cudaEventCreate(&e1));
  std::vector<double> per_block, ev;
  int bad = 0, distinct = 0;
  const double bytes_block = (double)count * step_bytes;
  for (int r = 0; r <= reps; ++r) {
    memset(sums, 0, blocks * 4);
    CK(cudaEventRecord(e0));
    kern<<<blocks, threads, smem>>>((const uint4*)buf, region_steps, stride_steps, base_mult, count, sums, times, sm);
    CK(cudaGetLastError());
    CK(cudaEventRecord(e1));
    CK(cudaDeviceSynchronize());
    for (int b = 0; b < blocks; ++b) bad += sums[b] != expect[b];
    if (r == 0) { distinct = (int)std::set<int>(sm, sm + blocks).size(); continue; }
    float ms; CK(cudaEventElapsedTime(&ms, e0, e1));
    double s = 0;
    for (int b = 0; b < blocks; ++b) s += bytes_block / (double)(times[2 * b + 1] - times[2 * b]);
    per_block.push_back(s / blocks);
    ev.push_back(bytes_block * blocks / (ms * 1e6));
  }
  std::sort(per_block.begin(), per_block.end());
  std::sort(ev.begin(), ev.end());
  printf("gpu=%s l2_bytes=%d test=load cg=%d blocks=%d distinct_sms=%d occ_blocks_per_sm=%d regs=%d threads=%d unroll=%d "
         "kb_in_flight_per_sm=%llu window_mb=%.0f passes=%d mb_per_block=%.1f reps=%d check=%s "
         "per_sm_gbs_med=%.1f per_sm_gbs_range=%.1f-%.1f event_aggregate_gbs=%.1f\n",
         prop.name, prop.l2CacheSize, cg, blocks, distinct, occ, fa.numRegs, threads, U,
         (unsigned long long)(U * step_bytes / 1024), window_mb, window_mb > 0 ? passes : 1, bytes_block / (1 << 20),
         reps, bad ? "FAIL" : "ok", per_block[per_block.size() / 2], per_block.front(), per_block.back(),
         ev[ev.size() / 2]);
  return 0;
}
