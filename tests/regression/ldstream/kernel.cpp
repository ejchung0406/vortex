#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// One CTA per SM streams plain loads, the shape of the H200 probe
// (gpu/borrow_load.cu): at each step thread t of a T-thread CTA issues U
// independent loads of B bytes, load u at element (step + u) * T + t, so a
// step covers T * B contiguous bytes and U * T * B bytes are in flight.
//   DRAM mode: CTA b reads its own slice of region_steps steps once.
//   L2 mode:   every CTA reads the same window region_steps steps long,
//              starting at a different step, after one untimed pass over it.
// Each thread sums the B-byte words it reads, mod 2^64; the host checks it.
//   lean = 0: the probe's loop, an index per load wrapped at the region end.
//   lean = 1: U pointers advanced by U steps (a load, an add and a pointer
//             add per load), wrapping only between passes: fewer instructions
//             per load, for separating the issue rate from the memory path.
// Holds the U loads of a step ahead of their uses: without it the compiler
// interleaves the adds with the loads, and an in-order warp then waits on an
// early load's result before issuing the later loads of the same step.
template <typename T, int U>
static inline void after_loads(T (&v)[U]) {
  #pragma unroll
  for (int u = 0; u < U; ++u)
    __asm__ volatile ("" : "+r"(v[u]));
}

template <typename T, int U>
static inline uint64_t sum_steps(const T* a, uint64_t step, uint64_t region, uint64_t count, uint32_t nt) {
  uint64_t acc[U];
  for (int u = 0; u < U; ++u)
    acc[u] = 0;
  for (uint64_t i = 0; i < count; i += U) {
    T v[U];
    #pragma unroll
    for (int u = 0; u < U; ++u) {
      uint64_t st = step + u;
      if (st >= region) st -= region;
      v[u] = a[st * nt];
    }
    after_loads(v);
    #pragma unroll
    for (int u = 0; u < U; ++u)
      acc[u] += v[u];
    step += U;
    if (step >= region) step -= region;
  }
  uint64_t r = 0;
  for (int u = 0; u < U; ++u)
    r += acc[u];
  return r;
}

template <typename T, int U>
static inline uint64_t sum_lean(const T* a, uint64_t step, uint64_t region, uint64_t count, uint32_t nt) {
  uint64_t acc[U];
  for (int u = 0; u < U; ++u)
    acc[u] = 0;
  while (count != 0) {
    // the steps up to the region's end; step and region are multiples of U
    uint64_t n = region - step;
    if (n > count) n = count;
    const T* q[U];
    #pragma unroll
    for (int u = 0; u < U; ++u)
      q[u] = a + (step + u) * nt;
    const T* end = a + (step + n) * nt;
    const uint64_t adv = uint64_t(U) * nt;
    while (q[0] != end) {
      // every load of the step before any use: an in-order warp would wait
      // on a load's result before issuing the next
      T v[U];
      #pragma unroll
      for (int u = 0; u < U; ++u)
        v[u] = *q[u];
      after_loads(v);
      #pragma unroll
      for (int u = 0; u < U; ++u) {
        acc[u] += v[u];
        q[u] += adv;
      }
    }
    count -= n;
    step = 0;
  }
  uint64_t r = 0;
  for (int u = 0; u < U; ++u)
    r += acc[u];
  return r;
}

template <typename T>
static inline uint64_t sum_any(const T* a, uint64_t step, uint64_t region, uint64_t count, uint32_t nt,
                               uint32_t unroll, uint32_t lean) {
  if (lean) {
    switch (unroll) {
    case 16: return sum_lean<T, 16>(a, step, region, count, nt);
    case 8:  return sum_lean<T, 8>(a, step, region, count, nt);
    case 4:  return sum_lean<T, 4>(a, step, region, count, nt);
    case 2:  return sum_lean<T, 2>(a, step, region, count, nt);
    default: return sum_lean<T, 1>(a, step, region, count, nt);
    }
  }
  switch (unroll) {
  case 16: return sum_steps<T, 16>(a, step, region, count, nt);
  case 8:  return sum_steps<T, 8>(a, step, region, count, nt);
  case 4:  return sum_steps<T, 4>(a, step, region, count, nt);
  case 2:  return sum_steps<T, 2>(a, step, region, count, nt);
  default: return sum_steps<T, 1>(a, step, region, count, nt);
  }
}

static inline uint64_t sum_bytes(uint64_t addr, uint32_t bytes, uint64_t step, uint64_t region, uint64_t count,
                                 uint32_t nt, uint32_t unroll, uint32_t lean) {
  if (bytes == 8)
    return sum_any(reinterpret_cast<const uint64_t*>(addr), step, region, count, nt, unroll, lean);
  return sum_any(reinterpret_cast<const uint32_t*>(addr), step, region, count, nt, unroll, lean);
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t t = threadIdx.x, nt = blockDim.x, b = blockIdx.x;
  const uint32_t B = arg->bytes;
  const uint64_t region = arg->region_steps;
  // this thread's first element, in bytes
  const uint64_t addr = arg->src_addr + (uint64_t(b) * arg->base_steps * nt + t) * B;
  const uint64_t step0 = (uint64_t(b) * arg->stride_steps) % region;
  uint64_t warm = 0;
  if (arg->warm) {
    // brings the window into the L2; its words are not checked
    warm = sum_bytes(addr, B, 0, region, region, nt, 1, 0);
  }
  __syncthreads();
  uint64_t t0 = 0;
  if (t == 0)
    t0 = csr_read(VX_CSR_MCYCLE);
  uint64_t s = sum_bytes(addr, B, step0, region, arg->count, nt, arg->unroll, arg->lean);
  __syncthreads();
  auto times = reinterpret_cast<uint64_t*>(arg->times_addr);
  if (t == 0) {
    times[2 * b] = t0;
    times[2 * b + 1] = csr_read(VX_CSR_MCYCLE);
  }
  // the warm-up pass's sum goes beside the timed one, so it is not optimized away
  reinterpret_cast<uint64_t*>(arg->sum_addr)[2 * (uint64_t(b) * nt + t)] = s;
  reinterpret_cast<uint64_t*>(arg->sum_addr)[2 * (uint64_t(b) * nt + t) + 1] = warm;
}
