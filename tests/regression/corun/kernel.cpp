#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// Two workloads in one launch, split by core: a CTA that lands on a core below
// bw_cores streams its slice of the source buffer (bwstream's coalesced read),
// one on any other core runs a register-only multiply-add loop. Each thread
// records the core it ran on, so the host can tell how much of each role ran.
template <int U>
static inline float sum_stride(const float* p, uint32_t steps, uint32_t stride) {
  float s[U];
  for (int u = 0; u < U; ++u)
    s[u] = 0.0f;
  for (uint32_t i = 0; i < steps; i += U) {
    for (int u = 0; u < U; ++u)
      s[u] += p[(uint64_t)(i + u) * stride];
  }
  float r = 0.0f;
  for (int u = 0; u < U; ++u)
    r += s[u];
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

// Four independent chains, so the loop is bound by FPU throughput, not latency.
static inline float muladd(uint32_t iters, float mul, float add) {
  float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
  for (uint32_t i = 0; i < iters; i += 4) {
    a0 = a0 * mul + add;
    a1 = a1 * mul + add;
    a2 = a2 * mul + add;
    a3 = a3 * mul + add;
  }
  return a0 + a1 + a2 + a3;
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto src  = reinterpret_cast<const float*>(arg->src_addr);
  auto out  = reinterpret_cast<float*>(arg->out_addr);
  auto core = reinterpret_cast<uint32_t*>(arg->core_addr);
  uint32_t T = blockDim.x;
  uint32_t t = threadIdx.x;
  uint32_t gid = blockIdx.x * T + t;
  uint32_t cid = vx_core_id();

  float r;
  if (cid < arg->bw_cores) {
    uint32_t steps = arg->steps;
    const float* base = src + (uint64_t)blockIdx.x * steps * T;
    r = sum_unrolled(base + t, steps, T, arg->unroll);
  } else {
    r = muladd(arg->iters, arg->mul, arg->add);
  }
  out[gid] = r;
  core[gid] = cid;
}
