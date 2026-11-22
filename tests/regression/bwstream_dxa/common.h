#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// One barrier per tile buffer; the device must have at least this many per CTA.
#define BWSTREAM_DXA_MAX_DEPTH 8

typedef struct {
  uint64_t out_addr;
  uint32_t tile_elems;  // floats per DXA tile
  uint32_t tiles;       // tiles per CTA
  uint32_t depth;       // tile buffers per CTA: transfers outstanding at once (1..8)
  uint32_t consume;     // 1: sum the whole tile, 0: read one word of it
} kernel_arg_t;

// The timed kernel's argument: thread 0 of each CTA stores {core, start, end}
// cycles at timing_addr + 3 * blockIdx.x.
typedef struct {
  kernel_arg_t stream;
  uint64_t timing_addr;
} timed_arg_t;

// The latency kernel's argument: CTA 0 times each tile's copy, cold and then
// warm, into 2 * tiles cycle counts at lat_addr; tile i starts stride tiles
// after tile i - 1.
typedef struct {
  kernel_arg_t stream;
  uint32_t stride;
  uint64_t lat_addr;
} latency_arg_t;

#endif
