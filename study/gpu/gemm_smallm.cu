// Memory bandwidth of a dense product with a small batch, Y = W X, where W is
// a large R x N FP32 matrix and X holds M input vectors of N elements: every
// element of W is read once and used in M multiply-adds.
//
//   gemm_smallm [--gib G] [--cols N] [--m M] [--blocks B] [--reps R]
//
// Each block of 1024 threads owns a contiguous band of W's rows; a warp
// computes one row at a time, its 32 threads reading 32 adjacent elements of
// the row per step and accumulating M partial sums against the M inputs.
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

template <int M>
__global__ void gemm(const float* __restrict__ w, const float* __restrict__ x, float* __restrict__ y,
                     size_t rows_per_block, int cols) {
  const int lane = threadIdx.x & 31, warp = threadIdx.x >> 5, warps = blockDim.x >> 5;
  const size_t row0 = (size_t)blockIdx.x * rows_per_block;
  for (size_t r = warp; r < rows_per_block; r += warps) {
    const float* wr = w + (row0 + r) * cols + lane;
    float s[M];
    for (int m = 0; m < M; ++m) s[m] = 0.f;
    for (int c = 0; c + 64 <= cols; c += 64) {
      const float w0 = wr[c], w1 = wr[c + 32];
      for (int m = 0; m < M; ++m) {
        const float* xm = x + (size_t)m * cols + lane;
        s[m] += w0 * xm[c] + w1 * xm[c + 32];
      }
    }
    for (int m = 0; m < M; ++m) {
      float acc = s[m];
      for (int o = 16; o > 0; o >>= 1) acc += __shfl_xor_sync(0xffffffffu, acc, o);
      if (lane == 0) y[(row0 + r) * M + m] = acc;
    }
  }
}

static void launch(int M, int blocks, const float* w, const float* x, float* y, size_t rpb, int cols) {
  switch (M) {
    case 1:  gemm<1><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 2:  gemm<2><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 4:  gemm<4><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 8:  gemm<8><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 16: gemm<16><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    case 32: gemm<32><<<blocks, 1024>>>(w, x, y, rpb, cols); break;
    default: fprintf(stderr, "m must be 1, 2, 4, 8, 16 or 32\n"); exit(1);
  }
}

int main(int argc, char** argv) {
  double gib = 8;
  int cols = 4096, M = 1, blocks = 0, reps = 5;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--cols")) cols = atoi(argv[++i]);
    else if (is("--m")) M = atoi(argv[++i]);
    else if (is("--blocks")) blocks = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  if (cols % 64 != 0) { fprintf(stderr, "cols must be a multiple of 64\n"); return 1; }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  if (blocks == 0) blocks = prop.multiProcessorCount;

  size_t rows = (size_t)(gib * (1ull << 30)) / sizeof(float) / cols;
  const size_t rpb = rows / blocks;
  rows = rpb * blocks;
  const double bytes = (double)rows * cols * sizeof(float);

  float *w, *x, *y;
  CK(cudaMalloc(&w, rows * cols * sizeof(float)));
  CK(cudaMalloc(&x, (size_t)M * cols * sizeof(float)));
  CK(cudaMalloc(&y, rows * M * sizeof(float)));
  CK(cudaMemset(w, 0, rows * cols * sizeof(float)));
  CK(cudaMemset(x, 0, (size_t)M * cols * sizeof(float)));

  launch(M, blocks, w, x, y, rpb, cols);
  CK(cudaDeviceSynchronize());
  double best = 0;
  for (int r = 0; r < reps; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    launch(M, blocks, w, x, y, rpb, cols);
    CK(cudaDeviceSynchronize());
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double g = bytes / s / 1e9;
    best = g > best ? g : best;
  }
  printf("gpu=%s sms=%d pattern=gemm cols=%d m=%d blocks=%d threads=1024 gib=%.2f gbs_best=%.1f "
         "gflops=%.0f\n",
         prop.name, prop.multiProcessorCount, cols, M, blocks, bytes / double(1ull << 30), best,
         best / 4 * 2 * M);
  return 0;
}
