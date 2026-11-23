#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// error bits a thread of a checking CTA records
#define BAR_ERR_LMEM 0x1u // another warp's shared-memory word was not its store of the round
#define BAR_ERR_GMEM 0x2u // another warp's global word was not its store of the round
#define BAR_TEST_LINE_WORDS 16u

typedef struct {
  uint32_t rounds;       // store, barrier, read, barrier: this many times per checking CTA
  uint32_t delay_sweep;  // a warp waits ((round + warp) % delay_sweep) * delay_step steps before its stores
  uint32_t delay_step;
  uint32_t noise_loads;  // loads each thread of a loading CTA makes
  uint32_t noise_words;  // words in the buffer they walk, a line apart
  uint32_t split_load;
  uint64_t noise_addr;
  uint64_t sink_addr;    // a loading CTA's sums, so that its loads are kept
  uint64_t gbuf_addr;    // a checking CTA's global words, one per thread
  uint64_t err_addr;     // per thread, the BAR_ERR_* bits it saw
} kernel_arg_t;

#endif
