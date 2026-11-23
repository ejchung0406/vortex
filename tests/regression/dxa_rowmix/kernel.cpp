#include <vx_spawn2.h>
#include <vx_dxa.h>
#include <vx_barrier.h>

#include "common.h"

// Each CTA's first warp issues `xfers` 2-D copies at once, each into its own
// shared-memory region at a different alignment, so several DXA workers write
// lines of different rows and row parts in the same cycles. The CTA then
// copies every region out for the host to check byte by byte.
constexpr uint32_t kDescSrc = 0;

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto out = reinterpret_cast<uint32_t*>(arg->out_addr);
  auto shmem = reinterpret_cast<uint8_t*>(__local_mem());
  const uint32_t xfers = arg->xfers;
  const uint32_t rounds = arg->rounds;
  const uint32_t region = arg->region;
  const uint32_t tile_words = arg->tile0 * arg->tile1;
  const uint32_t T = blockDim.x;
  const uint32_t t = threadIdx.x;
  const uint32_t b = blockIdx.x;

  vortex::barrier bar(0);
  const bool is_dxa_warp = (get_sub_group_id() == 0);

  for (uint32_t r = 0; r < rounds; ++r) {
    if (is_dxa_warp) {
      bar.expect_tx(xfers);
      for (uint32_t x = 0; x < xfers; ++x) {
        vx_dxa_issue_2d_wg(kDescSrc, bar.id(), shmem + x * region + rowmix_off(x),
                           rowmix_c0(b, r, x, arg->size0, arg->tile0), rowmix_c1(b, r, x, arg->size1));
      }
    }
    bar.arrive_and_wait();
    const uint32_t base = (b * rounds + r) * xfers * tile_words;
    for (uint32_t i = t; i < xfers * tile_words; i += T) {
      const uint32_t x = i / tile_words;
      const uint32_t k = i % tile_words;
      auto src = reinterpret_cast<const uint32_t*>(shmem + x * region + rowmix_off(x));
      out[base + i] = src[k];
    }
    // the next round's copies land only after this round's reads
    bar.arrive_and_wait();
  }
}
