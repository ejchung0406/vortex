// Bandwidth at which cuBLAS reads a large half-precision weight matrix in a
// small-batch product, C = A B with A (R x N, the weights), B (N x M).
//
//   cublas_lowprec [--gib G] [--cols N] [--dtype f16|bf16] [--iters K]
//
// Prints one line per batch size M: the GB/s of A read and the GFLOP/s.
// cublasGemmEx with FP16 or BF16 data. Limit the SMs from outside with an MPS
// active thread percentage. --iters K repeats each product K times per timing.
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

int main(int argc, char** argv) {
  double gib = 4;
  int N = 8192, iters = 1;
  std::string dtype = "f16";
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--cols")) N = atoi(argv[++i]);
    else if (is("--dtype")) dtype = argv[++i];
    else if (is("--iters")) iters = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  const bool bf = dtype == "bf16";
  if (!bf && dtype != "f16") { fprintf(stderr, "dtype must be f16 or bf16\n"); return 1; }
  const cudaDataType dt = bf ? CUDA_R_16BF : CUDA_R_16F;
  const cublasComputeType_t ct = bf ? CUBLAS_COMPUTE_32F : CUBLAS_COMPUTE_16F;

  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  const size_t esz = 2;
  const int R = (int)((size_t)(gib * (1ull << 30)) / esz / N);
  const double bytes = (double)R * N * esz;
  const int Ms[] = {1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 128, 256};
  const int maxM = 256;

  void *A, *B, *C;
  CK(cudaMalloc(&A, (size_t)R * N * esz));
  CK(cudaMalloc(&B, (size_t)N * maxM * esz));
  CK(cudaMalloc(&C, (size_t)R * maxM * esz));
  CK(cudaMemset(A, 0, (size_t)R * N * esz));
  CK(cudaMemset(B, 0, (size_t)N * maxM * esz));

  cublasHandle_t h;
  if (cublasCreate(&h) != CUBLAS_STATUS_SUCCESS) { fprintf(stderr, "cublasCreate failed\n"); return 1; }
  // 1.0 and 0.0 have the same bit patterns' meaning in FP16 (0x3c00) and BF16 (0x3f80)
  const unsigned short one = bf ? 0x3f80 : 0x3c00, zero = 0;

  for (int M : Ms) {
    auto run = [&]() {
      return cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, R, M, N, &one, A, dt, R, B, dt, N, &zero, C, dt, R,
                          ct, CUBLAS_GEMM_DEFAULT_TENSOR_OP);
    };
    cublasStatus_t st = run();
    CK(cudaDeviceSynchronize());
    if (st != CUBLAS_STATUS_SUCCESS) {
      printf("gpu=%s sms=%d dtype=%s m=%d unsupported=%d\n", prop.name, prop.multiProcessorCount,
             dtype.c_str(), M, (int)st);
      continue;
    }
    double best = 1e30;
    for (int r = 0; r < 3; ++r) {
      auto t0 = std::chrono::steady_clock::now();
      for (int k = 0; k < iters; ++k) run();
      CK(cudaDeviceSynchronize());
      double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / iters;
      best = s < best ? s : best;
    }
    printf("gpu=%s sms=%d dtype=%s m=%d gib=%.3f gbs=%.1f gflops=%.0f\n", prop.name,
           prop.multiProcessorCount, dtype.c_str(), M, bytes / double(1ull << 30), bytes / best / 1e9,
           2.0 * R * N * M / best / 1e9);
  }
  return 0;
}
