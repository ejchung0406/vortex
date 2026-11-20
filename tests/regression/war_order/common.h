#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// the three values each thread stores to its word, in program order
#define WAR_MARK1 0x40000000u
#define WAR_MARK2 0x80000000u
#define WAR_MARK3 0xC0000000u

// error bits a thread records for its word
#define WAR_ERR_L0 0x1u // first load did not return the initial value
#define WAR_ERR_L1 0x2u // load right after the first store did not return it
#define WAR_ERR_L2 0x4u // load right after the store pair did not return the second
#define WAR_ERR_L3 0x8u // delayed load did not return the second of the pair
#define WAR_ERR_L0B 0x10u // second load of the initial value did not return it

typedef struct {
  uint32_t num_words;   // words in the data buffer
  uint32_t num_threads; // threads launched
  uint32_t num_warps;   // warps launched
  uint32_t warp_size;   // threads per warp
  uint32_t delay_sweep; // the delayed load waits ((iteration + warp) % delay_sweep) * delay_step steps
  uint32_t delay_step;
  uint32_t store_sweep; // the first store waits ((3 * iteration + warp) % store_sweep) * store_step steps
  uint32_t store_step;
  uint32_t layout;      // 0: a line's words belong to different warps; 1: a warp's words are contiguous
  uint64_t data_addr;   // in: initial words; out: each word's last store
  uint64_t err_addr;    // out: per word, the WAR_ERR_* bits its thread saw
} kernel_arg_t;

#endif
