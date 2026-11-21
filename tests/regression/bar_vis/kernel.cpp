#include <vx_spawn2.h>
#include "common.h"

// The even CTAs check what a barrier orders: in each round every thread stores
// a value of the round to its shared-memory word and to its global word, as
// late as its warp's delay makes it, so that its warp reaches the barrier with
// the stores just issued; after the barrier every thread reads the words of a
// thread of another warp. A second barrier keeps the next round's stores from
// overtaking those reads. The odd CTAs walk a large buffer a line apart, so that
// the core always has reads to memory outstanding of warps the checking CTAs'
// barriers do not wait for.
__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t T = blockDim.x;
  const uint32_t t = threadIdx.x;
  const uint32_t W = get_num_sub_groups();
  const uint32_t warp = get_sub_group_id();
  const uint32_t gid = blockIdx.x * T + t;

  if (blockIdx.x & 1) {
    auto src = reinterpret_cast<const volatile uint32_t*>(arg->noise_addr);
    uint32_t acc = 0;
    uint32_t i = gid * 16;
    for (uint32_t n = 0; n < arg->noise_loads; ++n) {
      acc += src[i % arg->noise_words];
      i += T * 16 + 16;
    }
    reinterpret_cast<uint32_t*>(arg->sink_addr)[gid] = acc;
    return;
  }

  auto smem = reinterpret_cast<volatile uint32_t*>(__local_mem());
  auto gmem = reinterpret_cast<volatile uint32_t*>(arg->gbuf_addr) + blockIdx.x * T;
  const uint32_t lanes = T / W;
  uint32_t err = 0;
  for (uint32_t r = 0; r < arg->rounds; ++r) {
    uint32_t delay = ((r + warp) % arg->delay_sweep) * arg->delay_step;
    __asm__ volatile(
      "beqz %[n], 2f\n\t"
      "1:\n\t"
      "addi %[n], %[n], -1\n\t"
      "bnez %[n], 1b\n\t"
      "2:\n\t"
      : [n] "+r"(delay) : : "memory");
    const uint32_t v = (r << 24) ^ (blockIdx.x << 12) ^ t;
    smem[t] = v;
    gmem[t] = ~v;
    __syncthreads();
    const uint32_t o = (t + lanes * (1 + r % (W - 1))) % T;
    const uint32_t w = (r << 24) ^ (blockIdx.x << 12) ^ o;
    if (smem[o] != w)
      err |= BAR_ERR_LMEM;
    if (gmem[o] != ~w)
      err |= BAR_ERR_GMEM;
    __syncthreads();
  }
  reinterpret_cast<uint32_t*>(arg->err_addr)[gid] = err;
}
