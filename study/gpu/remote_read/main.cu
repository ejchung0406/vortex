#include "MeasurementSeries.hpp"

#include <cuda.h>
#include <iostream>
#include <iomanip>
#include <sys/time.h>

using namespace std;

double dtime() {
  struct timeval t;
  gettimeofday(&t, NULL);
  return (double)t.tv_sec + (double)t.tv_usec * 1.0e-6;
}

#define GPU_ERROR(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, const char *file, int line,
                      bool abort = true) {
  if (code != cudaSuccess) {
    cerr << "GPUassert: \"" << cudaGetErrorString(code) << "\"  in "
         << file << ": " << line << "\n";
    if (abort) exit(code);
  }
}

// Minimal pure-read kernel.
// 128-bit vectorized loads (double2), register accumulation,
// single 8-byte write per thread to prevent dead-code elimination.
__global__ void read_kernel(const double2 *__restrict__ src,
                            double *__restrict__ sink, size_t N) {
  size_t tid    = threadIdx.x + blockIdx.x * (size_t)blockDim.x;
  size_t stride = blockDim.x * (size_t)gridDim.x;

  double acc = 0.0;
  for (size_t i = tid; i < N; i += stride) {
    double2 v = src[i];
    acc += v.x + v.y;
  }
  sink[tid] = acc;
}

__global__ void init_kernel(double *A, size_t N) {
  size_t tid = threadIdx.x + blockIdx.x * (size_t)blockDim.x;
  for (size_t i = tid; i < N; i += blockDim.x * (size_t)gridDim.x)
    A[i] = 0.23 * i;
}

int main(int argc, char **argv) {
  GPU_ERROR(cudaSetDevice(0));

  const size_t MB    = (argc > 1) ? stoull(argv[1]) : 1024;
  const size_t N     = MB * 1024ULL * 1024ULL / sizeof(double);
  const size_t BYTES = N * sizeof(double);
  const size_t N2    = N / 2; // double2 element count

  cudaDeviceProp prop;
  GPU_ERROR(cudaGetDeviceProperties(&prop, 0));

  const int BS = (argc > 3) ? stoi(argv[3]) : 256;
  int maxActiveBlocks = 0;
  GPU_ERROR(cudaOccupancyMaxActiveBlocksPerMultiprocessor(
      &maxActiveBlocks, read_kernel, BS, 0));
  int numBlocks = (argc > 2) ? stoi(argv[2]) : prop.multiProcessorCount * maxActiveBlocks;

  cout << "Device:     " << prop.name << "\n"
       << "SMs:        " << prop.multiProcessorCount << "\n"
       << "Blocks:     " << numBlocks << " (" << maxActiveBlocks << "/SM)\n"
       << "BlockSize:  " << BS << "\n"
       << "Buffer:     " << MB << " MB (" << N << " doubles)\n\n";

  // Allocate source
  double *src_dev = nullptr;
  GPU_ERROR(cudaMalloc(&src_dev, BYTES));
  init_kernel<<<numBlocks, BS>>>(src_dev, N);
  GPU_ERROR(cudaDeviceSynchronize());

  // Sink: one double per thread — negligible write traffic
  double *sink = nullptr;
  GPU_ERROR(cudaMalloc(&sink, (size_t)numBlocks * BS * sizeof(double)));

  const double2 *src2 = reinterpret_cast<const double2 *>(src_dev);

  // Warmup
  // read_kernel<<<numBlocks, BS>>>(src2, sink, N2);
  // GPU_ERROR(cudaDeviceSynchronize());

  // Benchmark
  MeasurementSeries time;
  for (int i = 0; i < 3; ++i) {
    double t1 = dtime();
    read_kernel<<<numBlocks, BS>>>(src2, sink, N2);
    GPU_ERROR(cudaDeviceSynchronize());
    double t2 = dtime();
    time.add(t2 - t1);
  }

  double GiB = 1024.0 * 1024.0 * 1024.0;
  cout << fixed << setprecision(2)
       << "Read BW:    " << (double)BYTES / time.value() / GiB << " GB/s\n"
       << "Min time:   " << time.minValue() * 1e6 << " us\n"
       << "Avg time:   " << time.value() * 1e6 << " us\n"
       << "Max time:   " << time.maxValue() * 1e6 << " us\n";

  GPU_ERROR(cudaFree(sink));
  GPU_ERROR(cudaFree(src_dev));
  return 0;
}
