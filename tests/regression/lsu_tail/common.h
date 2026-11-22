#ifndef _COMMON_H_
#define _COMMON_H_

// words per cache line, and the line stride of the cold addresses
#define LSU_TAIL_LINE_WORDS 16

typedef struct {
  uint32_t num_threads;
  uint64_t src_addr;
  uint64_t dst_addr;
} kernel_arg_t;

#endif
