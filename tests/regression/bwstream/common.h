#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#define BWSTREAM_COALESCED 0
#define BWSTREAM_ROWS      1

typedef struct {
  uint64_t src_addr;
  uint64_t out_addr;
  uint32_t steps;    // loads per thread
  uint32_t pattern;
  uint32_t unroll;   // 1, 2, 4 or 8
  uint32_t block_warps; // coalesced: warps (CTAs) sharing one slice, 1 = each its own
} kernel_arg_t;

// The timed kernel's argument: thread 0 of each CTA stores {core, start, end}
// cycles at timing_addr + 3 * blockIdx.x.
typedef struct {
  kernel_arg_t stream;
  uint64_t timing_addr;
} timed_arg_t;

#endif
