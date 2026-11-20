#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#ifndef VX_CFG_NUM_THREADS
#define VX_CFG_NUM_THREADS 4
#endif

#ifndef ITYPE
#define ITYPE fp16
#endif

#ifndef OTYPE
#define OTYPE fp32
#endif

// accumulator tiles a warp can cycle through; each one is stored to C
#define MAX_ACC 4

#define MODE_WMMA  0 // one warp per CTA, operands in fragment registers
#define MODE_WGMMA 1 // a warpgroup per CTA, operands in shared memory

typedef struct {
  uint32_t mode;
  uint32_t iters; // multiplies per warp, a multiple of acc
  uint32_t acc;   // accumulators cycled: 1, 2 or MAX_ACC
  uint64_t A_addr;
  uint64_t B_addr;
  uint64_t C_addr;
  uint64_t cycles_addr; // per warp: the cycle its loop began and ended
} kernel_arg_t;

#endif
