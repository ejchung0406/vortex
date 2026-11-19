#include <vx_spawn2.h>
#include <vx_dxa.h>
#include <vx_barrier.h>

#include "common.h"

// Streams a float buffer through shared memory: each CTA has the DXA copy its
// `tiles` consecutive tiles into local memory and reads them. `depth` tile
// buffers form a ring, each with its own barrier, and depth - 1 copies are
// issued ahead, so `depth` transfers per CTA are outstanding while it waits.
// With `consume` the CTA sums each tile; without, each thread reads one word
// of it, which leaves the DXA's delivery as the only work per tile.
constexpr uint32_t kDescSrc = 0;
constexpr uint32_t kMaxDepth = 8;

__kernel void kernel_main(kernel_arg_t* arg) {
  auto out = reinterpret_cast<float*>(arg->out_addr);
  const uint32_t tile  = arg->tile_elems;
  const uint32_t tiles = arg->tiles;
  const uint32_t depth = arg->depth;
  const uint32_t consume = arg->consume;
  const uint32_t T = blockDim.x;
  const uint32_t t = threadIdx.x;
  const uint32_t gid = blockIdx.x * T + t;
  const uint32_t first = blockIdx.x * tiles * tile;  // element coordinate of the first tile

  auto shmem = reinterpret_cast<float*>(__local_mem());
  vortex::barrier bar[kMaxDepth] = {
    vortex::barrier(0), vortex::barrier(1), vortex::barrier(2), vortex::barrier(3),
    vortex::barrier(4), vortex::barrier(5), vortex::barrier(6), vortex::barrier(7) };
  const bool is_dxa_warp = (get_sub_group_id() == 0);

  // Tile j lives in slot j % depth.
  auto issue = [&](uint32_t j) {
    if (is_dxa_warp) {
      const uint32_t slot = j % depth;
      bar[slot].expect_tx(1);
      vx_dxa_issue_1d_wg(kDescSrc, bar[slot].id(), shmem + slot * tile, first + j * tile);
    }
  };

  float s = 0.0f;
  for (uint32_t j = 0; j + 1 < depth && j < tiles; ++j)
    issue(j);
  for (uint32_t i = 0; i < tiles; ++i) {
    const uint32_t ahead = i + depth - 1;
    if (ahead < tiles)
      issue(ahead);
    const uint32_t cur = i % depth;
    bar[cur].arrive_and_wait();
    const float* buf = shmem + cur * tile;
    if (consume) {
      for (uint32_t j = t; j < tile; j += T)
        s += buf[j];
    } else {
      s += buf[t];
    }
    bar[cur].arrive_and_wait();
  }
  out[gid] = s + (float)(gid + 1);
}
