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

// Inlined into both entries: a kernel keeps no callee-saved register, so one
// kernel cannot call another.
static inline __attribute__((always_inline)) void stream(kernel_arg_t* arg) {
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
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  stream(arg);
}

// A separate entry, so the untimed kernel carries no timing code.
__kernel void kernel_timed(timed_arg_t* arg) {
  const uint64_t c0 = vx_rdcycle();
  stream(&arg->stream);
  if (threadIdx.x == 0) {
    auto tm = reinterpret_cast<uint64_t*>(arg->timing_addr) + 3 * blockIdx.x;
    tm[0] = vx_core_id();
    tm[1] = c0;
    tm[2] = vx_rdcycle();
  }
}

// One copy at a time into slot 0: each tile cold, then the same tile again,
// which the caches now hold. Tiles `stride` tiles apart start on new DRAM rows.
__kernel void kernel_latency(latency_arg_t* arg) {
  auto out = reinterpret_cast<float*>(arg->stream.out_addr);
  auto lat = reinterpret_cast<uint64_t*>(arg->lat_addr);
  const uint32_t tile   = arg->stream.tile_elems;
  const uint32_t tiles  = arg->stream.tiles;
  const uint32_t stride = arg->stride;
  const uint32_t T = blockDim.x;
  const uint32_t t = threadIdx.x;
  const uint32_t gid = blockIdx.x * T + t;
  const uint32_t first = blockIdx.x * tiles * tile;

  auto shmem = reinterpret_cast<float*>(__local_mem());
  vortex::barrier bar(0);
  const bool is_dxa_warp = (get_sub_group_id() == 0);

  float s = 0.0f;
  for (uint32_t i = 0; i < tiles; ++i) {
    for (uint32_t rep = 0; rep < 2; ++rep) {
      const uint64_t c0 = vx_rdcycle();
      if (is_dxa_warp) {
        bar.expect_tx(1);
        vx_dxa_issue_1d_wg(kDescSrc, bar.id(), shmem, (first + i * tile) * stride);
      }
      bar.arrive_and_wait();
      const uint64_t c1 = vx_rdcycle();
      s += shmem[t];
      bar.arrive_and_wait();
      if (blockIdx.x == 0 && t == 0) {
        lat[2 * i + rep] = c1 - c0;
      }
    }
  }
  out[gid] = s + (float)(gid + 1);
}
