// How fast the Tensor Memory Accelerator delivers into one block's shared
// memory when nothing else limits it.
//
//   tmacopy [--gib G] [--blocks B] [--tile-kb K] [--depth D] [--reps R]
//
// Each block is one warp, and only its first thread works: it keeps D bulk
// global->shared copies of K KB outstanding over a ring of D buffers, each
// with its own barrier, waits for the oldest, reads one word of it and issues
// the next. The other threads exit, so no consumer and no block-wide
// synchronization bounds the rate.
#include <cuda_runtime.h>
#include <cuda/barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

using barrier = cuda::barrier<cuda::thread_scope_block>;
namespace cde = cuda::device::experimental;

template <int DEPTH>
__global__ void copy(const float* __restrict__ a, size_t per_block, uint32_t tile_elems, float* out) {
  extern __shared__ __align__(128) unsigned char smem_raw[];
  if (threadIdx.x != 0) return;
  barrier* bar = reinterpret_cast<barrier*>(smem_raw);
  float* buf0 = reinterpret_cast<float*>(smem_raw + 128);
  for (int d = 0; d < DEPTH; ++d)
    init(&bar[d], 1);
  cde::fence_proxy_async_shared_cta();

  const float* src = a + (size_t)blockIdx.x * per_block;
  const uint32_t tiles = (uint32_t)(per_block / tile_elems);
  const uint32_t bytes = tile_elems * sizeof(float);
  barrier::arrival_token token[DEPTH];
  float s = 0.f;
  auto issue = [&](uint32_t tile) {
    const uint32_t slot = tile % DEPTH;
    cde::cp_async_bulk_global_to_shared(buf0 + (size_t)slot * tile_elems,
                                        src + (size_t)tile * tile_elems, bytes, bar[slot]);
    token[slot] = cuda::device::barrier_arrive_tx(bar[slot], 1, bytes);
  };
  for (uint32_t i = 0; i < DEPTH && i < tiles; ++i)
    issue(i);
  for (uint32_t i = 0; i < tiles; ++i) {
    const uint32_t cur = i % DEPTH;
    bar[cur].wait(std::move(token[cur]));
    s += buf0[(size_t)cur * tile_elems];
    if (i + DEPTH < tiles)
      issue(i + DEPTH);
  }
  out[blockIdx.x] = s;
}

template <int DEPTH>
static void launch_depth(int blocks, size_t smem, const float* a, size_t per_block, uint32_t tile_elems,
                         float* out) {
  CK(cudaFuncSetAttribute(copy<DEPTH>, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  copy<DEPTH><<<blocks, 32, smem>>>(a, per_block, tile_elems, out);
}

int main(int argc, char** argv) {
  double gib = 8;
  int blocks = 0, tile_kb = 16, depth = 2, reps = 5;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--blocks")) blocks = atoi(argv[++i]);
    else if (is("--tile-kb")) tile_kb = atoi(argv[++i]);
    else if (is("--depth")) depth = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  if (blocks == 0) blocks = prop.multiProcessorCount;

  const uint32_t tile_elems = (uint32_t)tile_kb * 1024 / sizeof(float);
  const size_t smem = 128 + (size_t)depth * tile_elems * sizeof(float);
  const size_t elems = (size_t)(gib * (1ull << 30)) / sizeof(float);
  size_t per_block = elems / blocks;
  per_block -= per_block % tile_elems;
  const double bytes = (double)per_block * blocks * sizeof(float);

  float *a, *out;
  CK(cudaMalloc(&a, elems * sizeof(float)));
  CK(cudaMalloc(&out, blocks * sizeof(float)));
  CK(cudaMemset(a, 0, elems * sizeof(float)));

  auto launch = [&]() {
    switch (depth) {
    case 1:  launch_depth<1>(blocks, smem, a, per_block, tile_elems, out); break;
    case 2:  launch_depth<2>(blocks, smem, a, per_block, tile_elems, out); break;
    case 4:  launch_depth<4>(blocks, smem, a, per_block, tile_elems, out); break;
    case 8:  launch_depth<8>(blocks, smem, a, per_block, tile_elems, out); break;
    case 16: launch_depth<16>(blocks, smem, a, per_block, tile_elems, out); break;
    default: fprintf(stderr, "depth must be 1, 2, 4, 8 or 16\n"); exit(1);
    }
    CK(cudaGetLastError());
  };
  launch();
  CK(cudaDeviceSynchronize());
  double best = 0, sum = 0;
  for (int r = 0; r < reps; ++r) {
    auto t0 = std::chrono::steady_clock::now();
    launch();
    CK(cudaDeviceSynchronize());
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double g = bytes / s / 1e9;
    best = g > best ? g : best;
    sum += g;
  }
  printf("gpu=%s sms=%d pattern=tmacopy depth=%d tile_kb=%d blocks=%d threads=32 gib=%.2f "
         "gbs_best=%.1f gbs_mean=%.1f\n",
         prop.name, prop.multiProcessorCount, depth, tile_kb, blocks, bytes / double(1ull << 30), best,
         sum / reps);
  return 0;
}
