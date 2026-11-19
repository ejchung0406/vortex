#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

typedef struct {
  uint64_t out_addr;
  uint32_t tile_elems;  // floats per DXA tile
  uint32_t tiles;       // tiles per CTA
  uint32_t depth;       // 1: one tile buffer, 2: double-buffered
} kernel_arg_t;

#endif
