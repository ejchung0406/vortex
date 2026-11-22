#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// what each iteration of the kernel adds to a thread's sum
#define FS_PER_ITER 0xF4C

typedef struct {
  uint32_t iters;
  uint64_t out_addr;  // per thread: its sum, then its error mask
} kernel_arg_t;

#endif
