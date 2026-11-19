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
} kernel_arg_t;

#endif
