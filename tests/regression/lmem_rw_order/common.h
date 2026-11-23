#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#define RW_REGION 1024  // shared-memory bytes per warp
#define RW_NRES   10    // result words per thread

typedef struct {
  uint64_t out_addr;  // RW_NRES words per thread of the CTA
  uint32_t atomic;    // the device has the A extension: the middle access is an amoadd
} kernel_arg_t;

#endif
