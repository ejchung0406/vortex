#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

typedef struct {
  uint64_t out_addr;
  uint32_t tile_elems;  // floats per DXA tile
  uint32_t tiles;       // tiles per CTA
  uint32_t depth;       // tile buffers per CTA: transfers outstanding at once (1..8)
  uint32_t consume;     // 1: sum the whole tile, 0: read one word of it
} kernel_arg_t;

#endif
