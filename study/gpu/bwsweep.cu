// Memory bandwidth a read-only streaming kernel reaches as a function of how
// much of the GPU it is given.
//
//   bwsweep [--gib G] [--blocks B] [--threads T] [--pattern coalesced|rows]
//           [--unroll U] [--reps R]
//
// The kernel sums a G-GiB float buffer. Each block owns a contiguous slice.
//   coalesced: a warp's 32 threads read 32 adjacent floats per step (128 B,
//              two cache lines per warp load).
//   rows:      every thread walks its own contiguous sub-slice (32 separate
//              streams per warp, the access pattern of a row-per-thread GEMV).
// --unroll issues U independent loads per step before any is consumed.
// Limit the SMs available to the process from outside (MPS active thread
// percentage); --blocks limits the blocks launched.
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

template <int U>
__global__ void stream(const float* __restrict__ a, size_t per_block, int rows, float* out) {
  const size_t base = (size_t)blockIdx.x * per_block;
  const int t = threadIdx.x, T = blockDim.x;
  float s[U];
  for (int u = 0; u < U; ++u) s[u] = 0.f;
  if (rows) {
    // thread t walks [t * n, (t + 1) * n) of the block's slice
    const size_t n = per_block / T;
    const float* p = a + base + (size_t)t * n;
    for (size_t i = 0; i + U <= n; i += U)
      for (int u = 0; u < U; ++u) s[u] += p[i + u];
  } else {
    // step i: thread t reads element i * T + t, so a warp reads 32 neighbours
    const size_t n = per_block / T;
    const float* p = a + base + t;
    for (size_t i = 0; i + U <= n; i += U)
      for (int u = 0; u < U; ++u) s[u] += p[(i + u) * T];
  }
  float r = 0.f;
  for (int u = 0; u < U; ++u) r += s[u];
  if (t == 0) out[blockIdx.x] = r;
}

static void launch(int U, int blocks, int threads, const float* a, size_t per_block, int rows, float* out) {
  switch (U) {
    case 1: stream<1><<<blocks, threads>>>(a, per_block, rows, out); break;
    case 2: stream<2><<<blocks, threads>>>(a, per_block, rows, out); break;
    case 4: stream<4><<<blocks, threads>>>(a, per_block, rows, out); break;
    case 8: stream<8><<<blocks, threads>>>(a, per_block, rows, out); break;
    default: fprintf(stderr, "unroll must be 1, 2, 4 or 8\n"); exit(1);
  }
}

int main(int argc, char** argv) {
  double gib = 8;
  int blocks = 0, threads = 1024, unroll = 1, reps = 5;
  std::string pattern = "coalesced";
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--blocks")) blocks = atoi(argv[++i]);
    else if (is("--threads")) threads = atoi(argv[++i]);
    else if (is("--pattern")) pattern = argv[++i];
    else if (is("--unroll")) unroll = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  const int rows = pattern == "rows";
  if (!rows && pattern != "coalesced") { fprintf(stderr, "bad pattern\n"); return 1; }

  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  if (blocks == 0) blocks = prop.multiProcessorCount * (prop.maxThreadsPerMultiProcessor / threads);

  const size_t elems = (size_t)(gib * (1ull << 30)) / sizeof(float);
  // a whole number of unrolled steps per thread, so every element is read once
  size_t per_block = elems / blocks;
  per_block -= per_block % ((size_t)threads * unroll);
  const size_t bytes = per_block * blocks * sizeof(float);

  float *a, *out;
  CK(cudaMalloc(&a, elems * sizeof(float)));
  CK(cudaMalloc(&out, blocks * sizeof(float)));
  CK(cudaMemset(a, 0, elems * sizeof(float)));

  launch(unroll, blocks, threads, a, per_block, rows, out);  // warm-up
  CK(cudaDeviceSynchronize());

  std::vector<double> gbs;
  for (int r = 0; r < reps; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    launch(unroll, blocks, threads, a, per_block, rows, out);
    CK(cudaDeviceSynchronize());
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    gbs.push_back(bytes / s / 1e9);
  }
  double best = 0, sum = 0;
  for (double g : gbs) { best = g > best ? g : best; sum += g; }
  printf("gpu=%s sms=%d pattern=%s unroll=%d blocks=%d threads=%d gib=%.2f "
         "gbs_best=%.1f gbs_mean=%.1f\n",
         prop.name, prop.multiProcessorCount, pattern.c_str(), unroll, blocks, threads,
         bytes / double(1ull << 30), best, sum / reps);
  return 0;
}
