#include "stream.h"

// Separate entries in a binary of their own (kernel_timed.vxbin), so the
// untimed run's kernel.vxbin, and with it everything laid out after it in
// device memory, is the same with or without them.
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
