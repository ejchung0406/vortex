#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

typedef struct {
  uint64_t out_addr;  // per thread of warp 0: SC result, the word afterwards, the LR's value
  uint32_t writer;    // the warp that stores between warp 0's LR and SC
} kernel_arg_t;

#endif
