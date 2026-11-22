#include <vx_spawn2.h>
#include "common.h"

// Sums a float buffer no one else touches. Each CTA owns a contiguous slice of
// steps * blockDim.x floats.
//   coalesced: at step i thread t reads element i * T + t, so a warp reads T
//              adjacent floats per load.
//              With block_warps = W, W consecutive CTAs share one slice as
//              the warps of a W * T thread block would: at step i warp w
//              reads elements i * W * T + w * T + t, so the W warps read
//              adjacent pieces of one stripe and a warp's successive loads
//              are W * T floats apart.
//   rows:      thread t walks its own contiguous steps floats, one stream per
//              thread.
// U independent accumulators let U loads be outstanding before one is used.
template <int U>
static inline float sum_stride(const float* p, uint32_t steps, uint32_t stride) {
  float s[U];
  for (int u = 0; u < U; ++u) {
    s[u] = 0.0f;
  }
  for (uint32_t i = 0; i < steps; i += U) {
    for (int u = 0; u < U; ++u) {
      s[u] += p[(uint64_t)(i + u) * stride];
    }
  }
  float r = 0.0f;
  for (int u = 0; u < U; ++u) {
    r += s[u];
  }
  return r;
}

static inline float sum_unrolled(const float* p, uint32_t steps, uint32_t stride, uint32_t unroll) {
  switch (unroll) {
  case 8:  return sum_stride<8>(p, steps, stride);
  case 4:  return sum_stride<4>(p, steps, stride);
  case 2:  return sum_stride<2>(p, steps, stride);
  default: return sum_stride<1>(p, steps, stride);
  }
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto src = reinterpret_cast<const float*>(arg->src_addr);
  auto out = reinterpret_cast<float*>(arg->out_addr);
  uint32_t steps = arg->steps;
  uint32_t T = blockDim.x;
  uint32_t t = threadIdx.x;
  uint32_t gid = blockIdx.x * T + t;
  uint32_t W = arg->block_warps;
  const float* base = src + (uint64_t)(blockIdx.x / W) * W * steps * T + (uint64_t)(blockIdx.x % W) * T;

  float s;
  if (arg->pattern == BWSTREAM_ROWS) {
    s = sum_unrolled(base + (uint64_t)t * steps, steps, 1, arg->unroll);
  } else {
    s = sum_unrolled(base + t, steps, W * T, arg->unroll);
  }
  out[gid] = s + (float)(gid + 1);
}
