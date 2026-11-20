// A streaming kernel and a compute kernel on one GPU at the same time.
//
//   corun [--gib G] [--bw-blocks B] [--cp-blocks C] [--unroll U] [--reps R]
//         [--smem-kb S]
//
// The streaming kernel sums a G-GiB float buffer with B blocks of 1024 threads
// (a warp reads 32 adjacent floats per load, U loads outstanding). The compute
// kernel runs a register-only multiply-add loop in C blocks of 1024 threads; a
// 1024-thread block does not fit beside another on one SM, so the two kernels
// occupy different SMs there. --smem-kb S makes every block of both kernels
// reserve S KB of shared memory, which keeps two blocks off one SM on a part
// whose SM holds 64 warps (S above half the SM's shared memory). Each kernel is timed alone and then with the other
// running on a second stream; the compute kernel is sized to outlast the
// streaming one.
#include <cuda_runtime.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

template <int U>
__global__ void stream(const float* __restrict__ a, size_t per_block, float* out) {
  const int t = threadIdx.x, T = blockDim.x;
  const float* p = a + (size_t)blockIdx.x * per_block + t;
  const size_t n = per_block / T;
  float s[U];
  for (int u = 0; u < U; ++u) s[u] = 0.f;
  for (size_t i = 0; i + U <= n; i += U)
    for (int u = 0; u < U; ++u) s[u] += p[(i + u) * T];
  float r = 0.f;
  for (int u = 0; u < U; ++u) r += s[u];
  if (t == 0) out[blockIdx.x] = r;
}

__global__ void muladd(unsigned long long iters, float mul, float add, float* out) {
  float a0 = 0.f, a1 = 0.f, a2 = 0.f, a3 = 0.f;
  for (unsigned long long i = 0; i < iters; i += 4) {
    a0 = a0 * mul + add;
    a1 = a1 * mul + add;
    a2 = a2 * mul + add;
    a3 = a3 * mul + add;
  }
  if (threadIdx.x == 0) out[blockIdx.x] = a0 + a1 + a2 + a3;
}

static size_t g_smem = 0;

static void launch_stream(int U, cudaStream_t st, int blocks, const float* a, size_t per_block, float* out) {
  switch (U) {
    case 1: stream<1><<<blocks, 1024, g_smem, st>>>(a, per_block, out); break;
    case 2: stream<2><<<blocks, 1024, g_smem, st>>>(a, per_block, out); break;
    case 4: stream<4><<<blocks, 1024, g_smem, st>>>(a, per_block, out); break;
    case 8: stream<8><<<blocks, 1024, g_smem, st>>>(a, per_block, out); break;
    default: fprintf(stderr, "unroll must be 1, 2, 4 or 8\n"); exit(1);
  }
}

static double ms(cudaEvent_t a, cudaEvent_t b) {
  float t;
  CK(cudaEventElapsedTime(&t, a, b));
  return t;
}

int main(int argc, char** argv) {
  double gib = 4;
  int bw_blocks = 8, cp_blocks = -1, unroll = 4, reps = 3;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--bw-blocks")) bw_blocks = atoi(argv[++i]);
    else if (is("--cp-blocks")) cp_blocks = atoi(argv[++i]);
    else if (is("--unroll")) unroll = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--smem-kb")) g_smem = (size_t)atoi(argv[++i]) * 1024;
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  if (cp_blocks < 0) cp_blocks = prop.multiProcessorCount - bw_blocks;
  if (g_smem) {
    CK(cudaFuncSetAttribute(stream<1>, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)g_smem));
    CK(cudaFuncSetAttribute(stream<2>, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)g_smem));
    CK(cudaFuncSetAttribute(stream<4>, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)g_smem));
    CK(cudaFuncSetAttribute(stream<8>, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)g_smem));
    CK(cudaFuncSetAttribute(muladd, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)g_smem));
  }

  const size_t elems = (size_t)(gib * (1ull << 30)) / sizeof(float);
  size_t per_block = elems / bw_blocks;
  per_block -= per_block % ((size_t)1024 * unroll);
  const double bytes = (double)per_block * bw_blocks * sizeof(float);

  float *a, *out_bw, *out_cp;
  CK(cudaMalloc(&a, elems * sizeof(float)));
  CK(cudaMalloc(&out_bw, bw_blocks * sizeof(float)));
  CK(cudaMalloc(&out_cp, (cp_blocks > 0 ? cp_blocks : 1) * sizeof(float)));
  CK(cudaMemset(a, 0, elems * sizeof(float)));

  cudaStream_t s_bw, s_cp;
  CK(cudaStreamCreate(&s_bw));
  CK(cudaStreamCreate(&s_cp));
  cudaEvent_t b0, b1, c0, c1;
  CK(cudaEventCreate(&b0)); CK(cudaEventCreate(&b1));
  CK(cudaEventCreate(&c0)); CK(cudaEventCreate(&c1));

  // streaming alone
  double bw_alone_ms = 1e30;
  for (int r = 0; r < reps + 1; ++r) {
    CK(cudaEventRecord(b0, s_bw));
    launch_stream(unroll, s_bw, bw_blocks, a, per_block, out_bw);
    CK(cudaEventRecord(b1, s_bw));
    CK(cudaDeviceSynchronize());
    if (r > 0 && ms(b0, b1) < bw_alone_ms) bw_alone_ms = ms(b0, b1);
  }

  double cp_alone_ms = 0, bw_co_ms = 0, cp_co_ms = 0;
  unsigned long long iters = 0;
  if (cp_blocks > 0) {
    // size the compute kernel to run about three times as long as the stream
    const unsigned long long probe = 1ull << 22;
    CK(cudaEventRecord(c0, s_cp));
    muladd<<<cp_blocks, 1024, g_smem, s_cp>>>(probe, 1.f, 1.f, out_cp);
    CK(cudaEventRecord(c1, s_cp));
    CK(cudaDeviceSynchronize());
    CK(cudaEventRecord(c0, s_cp));
    muladd<<<cp_blocks, 1024, g_smem, s_cp>>>(probe, 1.f, 1.f, out_cp);
    CK(cudaEventRecord(c1, s_cp));
    CK(cudaDeviceSynchronize());
    iters = (unsigned long long)(probe * (3.0 * bw_alone_ms / ms(c0, c1)));
    iters -= iters % 4;
    if (iters < 4) iters = 4;

    cp_alone_ms = 1e30;
    for (int r = 0; r < reps; ++r) {
      CK(cudaEventRecord(c0, s_cp));
      muladd<<<cp_blocks, 1024, g_smem, s_cp>>>(iters, 1.f, 1.f, out_cp);
      CK(cudaEventRecord(c1, s_cp));
      CK(cudaDeviceSynchronize());
      if (ms(c0, c1) < cp_alone_ms) cp_alone_ms = ms(c0, c1);
    }

    // both: the compute kernel starts first and outlasts the stream
    bw_co_ms = 1e30; cp_co_ms = 1e30;
    for (int r = 0; r < reps; ++r) {
      CK(cudaEventRecord(c0, s_cp));
      muladd<<<cp_blocks, 1024, g_smem, s_cp>>>(iters, 1.f, 1.f, out_cp);
      CK(cudaEventRecord(c1, s_cp));
      CK(cudaEventRecord(b0, s_bw));
      launch_stream(unroll, s_bw, bw_blocks, a, per_block, out_bw);
      CK(cudaEventRecord(b1, s_bw));
      CK(cudaDeviceSynchronize());
      if (ms(b0, b1) < bw_co_ms) bw_co_ms = ms(b0, b1);
      if (ms(c0, c1) < cp_co_ms) cp_co_ms = ms(c0, c1);
    }
  }

  printf("gpu=%s sms=%d unroll=%d smem_kb=%d bw_blocks=%d cp_blocks=%d gib=%.2f "
         "bw_alone_gbs=%.1f bw_corun_gbs=%.1f cp_alone_ms=%.1f cp_corun_ms=%.1f bw_alone_ms=%.1f bw_corun_ms=%.1f\n",
         prop.name, prop.multiProcessorCount, unroll, (int)(g_smem / 1024), bw_blocks, cp_blocks, bytes / double(1ull << 30),
         bytes / (bw_alone_ms * 1e-3) / 1e9, cp_blocks > 0 ? bytes / (bw_co_ms * 1e-3) / 1e9 : 0.0,
         cp_alone_ms, cp_co_ms, bw_alone_ms, bw_co_ms);
  return 0;
}
