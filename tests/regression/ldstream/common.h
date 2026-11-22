#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

typedef struct {
  uint64_t src_addr;
  uint64_t sum_addr;     // per thread: the timed sum and the warm-up sum (uint64)
  uint64_t times_addr;   // per CTA: start, end cycle
  uint64_t region_steps; // steps of the region a CTA reads (a step is T * bytes)
  uint64_t base_steps;   // DRAM mode: CTA b starts at b * base_steps; L2 mode 0
  uint64_t stride_steps; // L2 mode: CTA b starts at step b * stride_steps of the window
  uint64_t count;        // steps read in the timed part
  uint32_t warm;         // L2 mode: one untimed pass over the window first
  uint32_t bytes;        // bytes per load: 4 or 8
  uint32_t unroll;       // independent loads per step: 1, 2, 4, 8, 16
  uint32_t lean;         // 1: pointer-advanced loop (see kernel.cpp)
} kernel_arg_t;

#endif
