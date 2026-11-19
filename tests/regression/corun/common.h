#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

typedef struct {
  uint64_t src_addr;
  uint64_t out_addr;   // float per thread: the role's result
  uint64_t core_addr;  // uint32 per thread: the core the thread ran on
  uint32_t bw_cores;   // cores [0, bw_cores) stream, the rest compute
  uint32_t steps;      // streaming role: loads per thread
  uint32_t unroll;     // streaming role: 1, 2, 4 or 8 loads outstanding
  uint32_t iters;      // compute role: multiply-adds per thread
  float    mul;        // compute role: a = a * mul + add
  float    add;
} kernel_arg_t;

#endif
