#include <vx_spawn2.h>
#include "common.h"

// Per iteration k every warp runs pattern p = k % NUM_PATTERNS over a region
// of its own: each lane loads its element and checks it, then stores it
// (8 bytes on even k, the low 4 bytes on odd k). On every third iteration
// some lanes sit out, so groups also go out partly masked.
static inline bool lane_active(uint32_t lane, uint32_t k) {
  return (k % 3) != 2 || ((lane * 5 + k) % 7) != 0;
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto src = reinterpret_cast<volatile uint64_t*>(arg->src_addr);
  auto dst = reinterpret_cast<volatile uint64_t*>(arg->dst_addr);
  auto err = reinterpret_cast<uint32_t*>(arg->err_addr);
  const uint32_t W    = arg->warp_size;
  const uint32_t tid  = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t lane = tid % W;
  const uint32_t warp = tid / W;
  uint32_t errors = 0;
  for (uint32_t k = 0; k < arg->num_iters; ++k) {
    if (!lane_active(lane, k)) {
      continue;
    }
    uint32_t p = k % NUM_PATTERNS;
    uint64_t slot = uint64_t(warp) * arg->num_iters + k;
    uint64_t e = (slot * arg->groups + lane / 8) * REGION + lane_elem[p][lane % 8];
    uint64_t v = src[e];
    if (v != src_value(e)) {
      ++errors;
    }
    if (p < STORE_PATTERNS) {
      if (k & 1) {
        reinterpret_cast<volatile uint32_t*>(dst)[2 * e] = uint32_t(v >> 7);
      } else {
        dst[e] = ~v;
      }
    }
  }
  err[tid] = errors;
}
