// Shared-memory read rate of the E2(b) consumer loop without any TMA traffic:
// W warps of one CTA (one CTA on one SM) each read 16 KB tiles from shared
// memory with the same ld.shared.v4 + add loop as borrow_kernel's consumer,
// cycling over S slots. Gives the consumer's own ceiling, to compare with the
// staged-consumed ingest under TMA writes.
//   borrow_smemread [--nw W] [--slots S] [--tiles N]
#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

__global__ void rd(int slots, int tiles, unsigned* out, unsigned long long* t) {
  extern __shared__ __align__(1024) unsigned char smem[];
  const int lane = threadIdx.x % 32, w = threadIdx.x / 32, nw = blockDim.x / 32;
  uint4* s4 = reinterpret_cast<uint4*>(smem);
  for (int i = threadIdx.x; i < slots * 1024; i += blockDim.x) s4[i] = make_uint4(i, i + 1, i + 2, i + 3);
  __syncthreads();
  unsigned long long t0;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t0));
  uint32_t acc = 0;
  for (int n = w; n < tiles; n += nw) {
    const uint4* v4 = s4 + (size_t)(n % slots) * 1024;
#pragma unroll 8
    for (uint32_t q = lane; q < 1024; q += 32) { const uint4 v = v4[q]; acc += v.x + v.y + v.z + v.w; }
    __syncwarp();
  }
  __syncthreads();
  unsigned long long t1;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t1));
  atomicAdd(out, acc);
  if (threadIdx.x == 0) { t[0] = t0; t[1] = t1; }
}

int main(int argc, char** argv) {
  int nw = 4, slots = 12, tiles = 200000;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--nw")) nw = atoi(argv[++i]);
    else if (is("--slots")) slots = atoi(argv[++i]);
    else if (is("--tiles")) tiles = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  const size_t smem = (size_t)slots * 16384;
  CK(cudaFuncSetAttribute(rd, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  unsigned* out; unsigned long long* t;
  CK(cudaMallocManaged(&out, 4)); CK(cudaMallocManaged(&t, 16));
  for (int r = 0; r < 3; ++r) {
    *out = 0;
    rd<<<1, nw * 32, smem>>>(slots, tiles, out, t);
    CK(cudaDeviceSynchronize());
  }
  printf("test=smemread nw=%d slots=%d tiles=%d gbs=%.1f bytes_per_clk_at_1.98GHz=%.1f\n", nw, slots, tiles,
         tiles * 16384.0 / (double)(t[1] - t[0]), tiles * 16384.0 / (double)(t[1] - t[0]) / 1.98);
  return 0;
}
