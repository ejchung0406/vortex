// Bandwidth at which cuBLAS reads a large weight matrix in a small-batch
// product, C = A B with A (R x N, the weights), B (N x M, the batch).
//
//   cublas_smallm [--gib G] [--cols N] [--half 0|1] [--iters K]
//
// Prints one line per M in {1, 2, 4, 8, 16, 32, 64}: the GB/s of A read and the
// GFLOP/s. M = 1 in FP32 also runs cublasSgemv. --half 1 uses FP16 data through
// cublasGemmEx, which may use the tensor cores. Limit the SMs from outside
// with an MPS active thread percentage. --iters K repeats each product K times
// per timing, for a matrix small enough to stay in the caches.
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cublas_v2.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)
#define CB(x) do { cublasStatus_t s_ = (x); if (s_ != CUBLAS_STATUS_SUCCESS) { \
  fprintf(stderr, "%s:%d cublas status %d\n", __FILE__, __LINE__, (int)s_); exit(1); } } while (0)

static int g_iters = 1;

// Seconds per product: the best of `reps` timings of g_iters back-to-back calls.
template <class F>
static double best_seconds(F run, int reps) {
  run();
  CK(cudaDeviceSynchronize());
  double best = 1e30;
  for (int r = 0; r < reps; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    for (int k = 0; k < g_iters; ++k) run();
    CK(cudaDeviceSynchronize());
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / g_iters;
    best = s < best ? s : best;
  }
  return best;
}

int main(int argc, char** argv) {
  double gib = 4;
  int N = 8192, half = 0;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--cols")) N = atoi(argv[++i]);
    else if (is("--half")) half = atoi(argv[++i]);
    else if (is("--iters")) g_iters = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  const size_t esz = half ? 2 : 4;
  const int R = (int)((size_t)(gib * (1ull << 30)) / esz / N);
  const double bytes = (double)R * N * esz;
  const int maxM = 64;

  void *A, *B, *C;
  CK(cudaMalloc(&A, (size_t)R * N * esz));
  CK(cudaMalloc(&B, (size_t)N * maxM * esz));
  CK(cudaMalloc(&C, (size_t)R * maxM * esz));
  CK(cudaMemset(A, 0, (size_t)R * N * esz));
  CK(cudaMemset(B, 0, (size_t)N * maxM * esz));

  cublasHandle_t h;
  CB(cublasCreate(&h));
  const float one = 1.f, zero = 0.f;
  const __half hone = __float2half(1.f), hzero = __float2half(0.f);

  if (!half) {
    double s = best_seconds([&]() {
      CB(cublasSgemv(h, CUBLAS_OP_N, R, N, &one, (const float*)A, R, (const float*)B, 1, &zero, (float*)C, 1));
    }, 3);
    printf("gpu=%s sms=%d kernel=sgemv half=0 m=1 gib=%.3f gbs=%.1f gflops=%.0f\n", prop.name,
           prop.multiProcessorCount, bytes / double(1ull << 30), bytes / s / 1e9, 2.0 * R * N / s / 1e9);
  }
  for (int M = 1; M <= maxM; M *= 2) {
    double s = best_seconds([&]() {
      if (half)
        CB(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, R, M, N, &hone, A, CUDA_R_16F, R, B, CUDA_R_16F, N,
                        &hzero, C, CUDA_R_16F, R, CUBLAS_COMPUTE_16F, CUBLAS_GEMM_DEFAULT_TENSOR_OP));
      else
        CB(cublasSgemm(h, CUBLAS_OP_N, CUBLAS_OP_N, R, M, N, &one, (const float*)A, R, (const float*)B, N,
                       &zero, (float*)C, R));
    }, 3);
    printf("gpu=%s sms=%d kernel=gemm half=%d m=%d gib=%.3f gbs=%.1f gflops=%.0f\n", prop.name,
           prop.multiProcessorCount, half, M, bytes / double(1ull << 30), bytes / s / 1e9,
           2.0 * R * N * M / s / 1e9);
  }
  return 0;
}
