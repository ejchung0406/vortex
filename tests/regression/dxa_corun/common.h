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
  uint32_t dxa_ctas;    // CTAs [0, dxa_ctas) copy; the rest stream plain loads
  uint32_t load_steps;  // loads per thread of a load CTA
  uint64_t load_addr;   // load CTAs' source
  uint64_t time_addr;   // per CTA: first and last cycle
} kernel_arg_t;

#endif
