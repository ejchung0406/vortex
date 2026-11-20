// Memory bandwidth a streaming kernel reaches when the Tensor Memory
// Accelerator brings the data into shared memory (sm_90 and later).
//
//   tmasweep [--gib G] [--blocks B] [--threads T] [--tile-kb K] [--depth 1|2|3|4|6|8]
//            [--reps R]
//
// Each block owns a contiguous slice of a G-GiB float buffer and walks it in
// tiles of K KB: one thread issues a bulk global->shared copy
// (cp.async.bulk), every thread waits on the copy's barrier and then sums its
// share of the tile. With --depth 2 the next tile's copy is issued before the
// current tile is consumed; --depth D keeps D transfers per block outstanding.
#include <cuda_runtime.h>
#include <cuda/barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

using barrier = cuda::barrier<cuda::thread_scope_block>;
namespace cde = cuda::device::experimental;

static __device__ inline void issue(float* dst, const float* src, uint32_t bytes, barrier& bar,
                                    barrier::arrival_token& token) {
  if (threadIdx.x == 0) {
    cde::cp_async_bulk_global_to_shared(dst, src, bytes, bar);
    token = cuda::device::barrier_arrive_tx(bar, 1, bytes);
  } else {
    token = bar.arrive();
  }
}

template <int DEPTH>
__global__ void stream(const float* __restrict__ a, size_t per_block, uint32_t tile_elems, float* out) {
  extern __shared__ __align__(128) unsigned char smem_raw[];
  barrier* bar = reinterpret_cast<barrier*>(smem_raw);
  float* buf0 = reinterpret_cast<float*>(smem_raw + 128);

  const int t = threadIdx.x, T = blockDim.x;
  if (t == 0) {
    for (int d = 0; d < DEPTH; ++d)
      init(&bar[d], T);
    cde::fence_proxy_async_shared_cta();
  }
  __syncthreads();

  const float* src = a + (size_t)blockIdx.x * per_block;
  const uint32_t tiles = (uint32_t)(per_block / tile_elems);
  const uint32_t bytes = tile_elems * sizeof(float);
  barrier::arrival_token token[DEPTH];
  float s = 0.f;

  // Tile i lives in slot i % DEPTH. DEPTH - 1 tiles are issued ahead, so DEPTH
  // transfers are outstanding while the block waits.
  for (uint32_t i = 0; i + 1 < DEPTH && i < tiles; ++i)
    issue(buf0 + (size_t)i * tile_elems, src + (size_t)i * tile_elems, bytes, bar[i], token[i]);
  for (uint32_t i = 0; i < tiles; ++i) {
    const uint32_t ahead = i + DEPTH - 1;
    if (ahead < tiles) {
      const uint32_t slot = ahead % DEPTH;
      issue(buf0 + (size_t)slot * tile_elems, src + (size_t)ahead * tile_elems, bytes, bar[slot],
            token[slot]);
    }
    const uint32_t cur = i % DEPTH;
    bar[cur].wait(std::move(token[cur]));
    const float* buf = buf0 + (size_t)cur * tile_elems;
    for (uint32_t j = t; j < tile_elems; j += T) s += buf[j];
    __syncthreads();  // everyone is done with the slot before it is refilled
  }
  if (t == 0) out[blockIdx.x] = s;
}

template <int DEPTH>
static void launch_depth(int blocks, int threads, size_t smem, const float* a, size_t per_block,
                         uint32_t tile_elems, float* out) {
  CK(cudaFuncSetAttribute(stream<DEPTH>, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  stream<DEPTH><<<blocks, threads, smem>>>(a, per_block, tile_elems, out);
}

int main(int argc, char** argv) {
  double gib = 8;
  int blocks = 0, threads = 1024, tile_kb = 16, depth = 1, reps = 5;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--gib")) gib = atof(argv[++i]);
    else if (is("--blocks")) blocks = atoi(argv[++i]);
    else if (is("--threads")) threads = atoi(argv[++i]);
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
  const size_t bytes = per_block * blocks * sizeof(float);

  float *a, *out;
  CK(cudaMalloc(&a, elems * sizeof(float)));
  CK(cudaMalloc(&out, blocks * sizeof(float)));
  CK(cudaMemset(a, 0, elems * sizeof(float)));

  auto launch = [&]() {
    switch (depth) {
    case 1: launch_depth<1>(blocks, threads, smem, a, per_block, tile_elems, out); break;
    case 2: launch_depth<2>(blocks, threads, smem, a, per_block, tile_elems, out); break;
    case 3: launch_depth<3>(blocks, threads, smem, a, per_block, tile_elems, out); break;
    case 4: launch_depth<4>(blocks, threads, smem, a, per_block, tile_elems, out); break;
    case 6: launch_depth<6>(blocks, threads, smem, a, per_block, tile_elems, out); break;
    case 8: launch_depth<8>(blocks, threads, smem, a, per_block, tile_elems, out); break;
    default: fprintf(stderr, "depth must be 1, 2, 3, 4, 6 or 8\n"); exit(1);
    }
    CK(cudaGetLastError());
  };

  launch();  // warm-up
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
  printf("gpu=%s sms=%d pattern=tma depth=%d tile_kb=%d blocks=%d threads=%d gib=%.2f "
         "gbs_best=%.1f gbs_mean=%.1f\n",
         prop.name, prop.multiProcessorCount, depth, tile_kb, blocks, threads,
         bytes / double(1ull << 30), best, sum / reps);
  return 0;
}
