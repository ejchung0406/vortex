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

// A CTA past `dxa_ctas` streams plain loads beside the copies instead: each
// thread sums `load_steps` words of the CTA's own region, four loads at a time.
static float load_stream(const float* p, uint32_t steps, uint32_t T) {
  float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
  for (uint32_t i = 0; i < steps; i += 4) {
    s0 += p[(uint64_t)(i + 0) * T];
    s1 += p[(uint64_t)(i + 1) * T];
    s2 += p[(uint64_t)(i + 2) * T];
    s3 += p[(uint64_t)(i + 3) * T];
  }
  return (s0 + s1) + (s2 + s3);
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto out = reinterpret_cast<float*>(arg->out_addr);
  auto times = reinterpret_cast<uint64_t*>(arg->time_addr);
  const uint64_t t_start = vx_rdcycle();
  if (blockIdx.x >= arg->dxa_ctas) {
    const uint32_t T = blockDim.x;
    const uint32_t b = blockIdx.x - arg->dxa_ctas;
    auto src = reinterpret_cast<const float*>(arg->load_addr) + (uint64_t)b * arg->load_steps * T + threadIdx.x;
    const float s = load_stream(src, arg->load_steps, T);
    out[blockIdx.x * T + threadIdx.x] = s + (float)(blockIdx.x * T + threadIdx.x + 1);
    if (threadIdx.x == 0) {
      times[2 * blockIdx.x] = t_start;
      times[2 * blockIdx.x + 1] = vx_rdcycle();
    }
    return;
  }
  const uint32_t tile  = arg->tile_elems;
  const uint32_t tiles = arg->tiles;
  const uint32_t depth = arg->depth;
  const uint32_t consume = arg->consume;
  const uint32_t T = blockDim.x;
  const uint32_t t = threadIdx.x;
  const uint32_t gid = blockIdx.x * T + t;
  const uint32_t first = blockIdx.x * tiles * tile;  // element coordinate of the first tile

  auto shmem = reinterpret_cast<float*>(__local_mem());
  vortex::barrier bar[BWSTREAM_DXA_MAX_DEPTH] = {
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
  for (uint32_t j = 0; j + 1 < depth && j < tiles; ++j) {
    issue(j);
  }
  for (uint32_t i = 0; i < tiles; ++i) {
    const uint32_t ahead = i + depth - 1;
    if (ahead < tiles) {
      issue(ahead);
    }
    const uint32_t cur = i % depth;
    bar[cur].arrive_and_wait();
    const float* buf = shmem + cur * tile;
    if (consume) {
      for (uint32_t j = t; j < tile; j += T) {
        s += buf[j];
      }
    } else {
      s += buf[t];
    }
    // The slot is refilled next iteration: wait until the warp's reads of it are done.
    bar[cur].arrive_and_wait();
  }
  out[gid] = s + (float)(gid + 1);
  if (t == 0) {
    times[2 * blockIdx.x] = t_start;
    times[2 * blockIdx.x + 1] = vx_rdcycle();
  }
}
