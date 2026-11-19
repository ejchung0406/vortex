#include <vx_spawn2.h>
#include <vx_dxa.h>
#include <vx_barrier.h>

#include "common.h"

// Streams a float buffer through shared memory: each CTA has the DXA copy its
// `tiles` consecutive tiles into local memory, one at a time, and sums them.
// With depth 2 the next tile's copy is issued before the current one is
// consumed, so two transfers per CTA are outstanding.
constexpr uint32_t kDescSrc = 0;

static inline float sum_tile(float s, const float* buf, uint32_t tile, uint32_t t, uint32_t T) {
  for (uint32_t j = t; j < tile; j += T)
    s += buf[j];
  return s;
}

__kernel void kernel_main(kernel_arg_t* arg) {
  auto out = reinterpret_cast<float*>(arg->out_addr);
  const uint32_t tile  = arg->tile_elems;
  const uint32_t tiles = arg->tiles;
  const uint32_t T = blockDim.x;
  const uint32_t t = threadIdx.x;
  const uint32_t gid = blockIdx.x * T + t;
  const uint32_t first = blockIdx.x * tiles * tile;  // element coordinate of the first tile

  auto shmem = reinterpret_cast<float*>(__local_mem());
  float* buf[2] = { shmem, shmem + tile };
  vortex::barrier bar[2] = { vortex::barrier(0), vortex::barrier(1) };
  const bool is_dxa_warp = (get_sub_group_id() == 0);

  float s = 0.0f;
  if (arg->depth == 2) {
    uint32_t cur = 0;
    if (is_dxa_warp) {
      bar[0].expect_tx(1);
      vx_dxa_issue_1d_wg(kDescSrc, bar[0].id(), buf[0], first);
    }
    for (uint32_t i = 0; i < tiles; ++i) {
      const uint32_t nxt = cur ^ 1u;
      if (i + 1 < tiles && is_dxa_warp) {
        bar[nxt].expect_tx(1);
        vx_dxa_issue_1d_wg(kDescSrc, bar[nxt].id(), buf[nxt], first + (i + 1) * tile);
      }
      bar[cur].arrive_and_wait();
      s = sum_tile(s, buf[cur], tile, t, T);
      bar[cur].arrive_and_wait();
      cur = nxt;
    }
  } else {
    for (uint32_t i = 0; i < tiles; ++i) {
      if (is_dxa_warp) {
        bar[0].expect_tx(1);
        vx_dxa_issue_1d_wg(kDescSrc, bar[0].id(), buf[0], first + i * tile);
      }
      bar[0].arrive_and_wait();
      s = sum_tile(s, buf[0], tile, t, T);
      bar[0].arrive_and_wait();
    }
  }
  out[gid] = s + (float)(gid + 1);
}
