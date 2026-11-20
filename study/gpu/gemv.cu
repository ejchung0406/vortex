// Memory bandwidth of a dense matrix-vector product, y = W x, as a function
// of the blocks it is given.
//
//   gemv [--gib G] [--cols N] [--blocks B] [--unroll U] [--reps R]
//
// W is a G-GiB row-major FP32 matrix with N columns. Each block of 1024
// threads owns a contiguous band of rows; a warp computes one row at a time,
// its 32 threads reading 32 adjacent elements of the row and of x per step
// (U steps outstanding) and reducing the 32 partial sums at the end of the row.
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

template <int U>
__global__ void gemv(const float* __restrict__ w, const float* __restrict__ x, float* __restrict__ y,
                     size_t rows_per_block, int cols) {
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
  const size_t row0 = (size_t)blockIdx.x * rows_per_block;
  for (size_t r = warp; r < rows_per_block; r += warps) {
    const float* wr = w + (row0 + r) * cols + lane;
    const float* xr = x + lane;
    float s[U];
    for (int u = 0; u < U; ++u) s[u] = 0.f;
    for (int c = 0; c + 32 * U <= cols; c += 32 * U)
      for (int u = 0; u < U; ++u) s[u] += wr[c + 32 * u] * xr[c + 32 * u];
    float acc = 0.f;
    for (int u = 0; u < U; ++u) acc += s[u];
    for (int o = 16; o > 0; o >>= 1) acc += __shfl_xor_sync(0xffffffffu, acc, o);
    if (lane == 0) y[row0 + r] = acc;
  }
}

static void launch(int U, int blocks, const float* w, const float* x, float* y, size_t rpb, int cols) {
  switch (U) {
    case 1: gemv<1><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 2: gemv<2><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 4: gemv<4><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 8: gemv<8><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    default: fprintf(stderr, "unroll must be 1, 2, 4 or 8\n"); exit(1);
  }
}

int main(int argc, char** argv) {
  double gib = 8;
  int cols = 8192, blocks = 0, unroll = 4, reps = 5;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--cols")) cols = atoi(argv[++i]);
    else if (is("--blocks")) blocks = atoi(argv[++i]);
    else if (is("--unroll")) unroll = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  if (cols % (32 * unroll) != 0) { fprintf(stderr, "cols must be a multiple of 32 * unroll\n"); return 1; }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  if (blocks == 0) blocks = prop.multiProcessorCount;

  size_t rows = (size_t)(gib * (1ull << 30)) / sizeof(float) / cols;
  const size_t rpb = rows / blocks;
  rows = rpb * blocks;
  const double bytes = (double)rows * cols * sizeof(float);

  float *w, *x, *y;
  CK(cudaMalloc(&w, rows * cols * sizeof(float)));
  CK(cudaMalloc(&x, cols * sizeof(float)));
  CK(cudaMalloc(&y, rows * sizeof(float)));
  CK(cudaMemset(w, 0, rows * cols * sizeof(float)));
  CK(cudaMemset(x, 0, cols * sizeof(float)));

  launch(unroll, blocks, w, x, y, rpb, cols);
  CK(cudaDeviceSynchronize());
  double best = 0, sum = 0;
  for (int r = 0; r < reps; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    launch(unroll, blocks, w, x, y, rpb, cols);
    CK(cudaDeviceSynchronize());
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double g = bytes / s / 1e9;
    best = g > best ? g : best;
    sum += g;
  }
  printf("gpu=%s sms=%d pattern=gemv cols=%d unroll=%d blocks=%d threads=1024 gib=%.2f "
         "gbs_best=%.1f gbs_mean=%.1f\n",
         prop.name, prop.multiProcessorCount, cols, unroll, blocks, bytes / double(1ull << 30), best,
         sum / reps);
  return 0;
}
