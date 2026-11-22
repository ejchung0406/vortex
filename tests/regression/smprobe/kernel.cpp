#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// Shared-memory access beside streaming neighbours, the shape of the H200
// probe (gpu/borrow_e4.cu --mode probe): 16 CTAs of 128 threads on one SM.
// CTA 0 is the probe; after a delay it runs probe_iters iterations of
//   smem_bar:  store 16 B to shared memory, barrier, read another thread's
//              16 B (t + 37 mod 128), barrier
//   bar_only:  register work and two barriers
//   smem_only: store 16 B and read back its own first 8 B, no barrier
// and times each with mcycle. The other CTAs are neighbours until the probe
// is done: idle (exit at once), loads (each iteration 8 KB of a stream, a
// thread's 8 loads 1 KB apart; no barrier), or loads_bar (the loads stored to
// shared memory, barrier, another thread's words read, barrier). The part
// loads 16 B per thread four times; here 8 B eight times moves the same 8 KB.
struct alignas(16) quad { uint32_t x, y, z, w; };

static inline uint64_t now() { return csr_read(VX_CSR_MCYCLE); }

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t t = threadIdx.x, b = blockIdx.x;
  auto flag = reinterpret_cast<volatile uint32_t*>(arg->flag_addr);
  uint32_t acc = 0;
  if (b == 0) {
    auto sh = reinterpret_cast<volatile quad*>(__local_mem());
    auto itcyc = reinterpret_cast<uint32_t*>(arg->itcyc_addr);
    const uint64_t d0 = now();
    while (now() - d0 < arg->delay) {}
    __syncthreads();
    for (uint32_t it = 0; it < arg->probe_iters; ++it) {
      const uint64_t c0 = now();
      if (arg->probe_kind == PROBE_SMEM_BAR) {
        sh[t].x = acc + it; sh[t].y = t; sh[t].z = it; sh[t].w = 0;
        __syncthreads();
        const uint32_t o = (t + 37) & 127;
        acc += sh[o].x + sh[o].y;
        __syncthreads();
      } else if (arg->probe_kind == PROBE_BAR_ONLY) {
        acc = acc * 3 + it;
        __syncthreads();
        acc ^= t;
        __syncthreads();
      } else {
        sh[t].x = acc + it; sh[t].y = t; sh[t].z = it; sh[t].w = 0;
        acc += sh[t].x + sh[t].y;
      }
      if (t == 0)
        itcyc[it] = uint32_t(now() - c0);
    }
    __syncthreads();
    if (t == 0) {
      *flag = 1;
      vx_fence();
    }
  } else if (arg->neighbours != NEIGH_IDLE) {
    auto sh = reinterpret_cast<volatile uint64_t*>(__local_mem());
    auto src = reinterpret_cast<const volatile uint64_t*>(arg->src_addr);
    const bool bar = (arg->neighbours == NEIGH_LOADS_BAR);
    // With barriers in the loop every warp leaves in the same iteration:
    // thread 0 copies the flag before the first barrier, and all test the copy
    // after it.
    auto stop = reinterpret_cast<volatile uint32_t*>(sh + 8 * 128);
    for (uint32_t it = 0; it < arg->max_neigh; ++it) {
      if (!bar && *flag != 0)
        break;
      if (bar && t == 0)
        *stop = *flag;
      const uint64_t chunk = (uint64_t(it) * gridDim.x + b) % arg->nchunks;
      const volatile uint64_t* q = src + chunk * 1024 + t;
      uint64_t v[8];
      #pragma unroll
      for (int u = 0; u < 8; ++u)
        v[u] = q[u * 128];
      if (bar) {
        #pragma unroll
        for (int u = 0; u < 8; ++u)
          sh[u * 128 + t] = v[u];
        __syncthreads();
        if (*stop != 0)
          break;
        const uint32_t o = (t + 37) & 127;
        #pragma unroll
        for (int u = 0; u < 8; ++u) {
          uint64_t w = sh[u * 128 + o];
          acc += uint32_t(w) + uint32_t(w >> 32);
        }
        __syncthreads();
      } else {
        #pragma unroll
        for (int u = 0; u < 8; ++u)
          acc += uint32_t(v[u]) + uint32_t(v[u] >> 32);
      }
    }
  }
  reinterpret_cast<uint32_t*>(arg->sums_addr)[uint64_t(b) * blockDim.x + t] = acc;
}
