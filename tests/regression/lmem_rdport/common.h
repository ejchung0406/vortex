#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#define MODE_RACE   0 // writers overwrite the rows the tensor engine is copying
#define MODE_STARVE 1 // the tensor engine's row reads beside the warps' loads
#define MODE_HOLD   2 // the row reads beside a stream that holds them up: atomics, or stores to their rows

// Shared memory starts with the CTA's TMEM column and the warps' flags,
// padded to a bank row.
#define SLOT_BYTES 128

// A word of the raced region: a generation above the word's index, in the
// mantissa of a normal float so it passes through FP registers unchanged.
#define VAL_BASE   0x40000000u
#define GEN_SHIFT  14
#define IDX_MASK   ((1u << GEN_SHIFT) - 1)
#define MAX_GENS   511

// The race region is three sub-regions, each with its own writer.
#define SUB_DXA    0
#define SUB_STORE  1
#define SUB_AMO    2
#define NUM_SUBS   3

// Cycle counts the starve mode reports, as 64-bit words per warp: warp 0
// does the tensor work, the others load.
#define ST_SOLO   0  // the warp's fixed work alone
#define ST_CONT   1  // the same work beside the other side
#define ST_END_T  2  // the cycle the warp's work ended in the phase that times the tensor work
#define ST_END_L  3  // and in the phase that times the loads
#define ST_WORST_SOLO 4  // tensor warp, MODE_HOLD: the longest wait to issue an op, alone
#define ST_WORST_CONT 5  // and beside the stream
#define ST_WORDS  6

typedef struct {
  uint32_t mode;
  // race
  uint32_t sub_words;   // 32-bit words of a sub-region
  uint32_t cols;        // TMEM columns one copy of the region fills
  uint32_t copies;      // copies taken while the writers run
  uint32_t reps;        // tc.cp of the region per copy
  uint32_t dxa_gens;
  uint32_t store_gens;
  uint32_t amo_gens;
  // starve
  uint32_t N;           // tile width of a tc.mma
  uint32_t ops;         // tensor ops of the fixed tensor work
  uint32_t sweeps;      // sweeps of a warp's fixed loads
  uint32_t outlast;     // how many times the fixed work the other side does beside it
  uint32_t span;        // loads per thread in a sweep
  uint32_t conflict;    // 1: a warp's lanes read one bank, a pass each
  uint32_t stores;      // MODE_HOLD: the stream stores to the operands' rows instead of atomics elsewhere
  uint32_t use_cp;      // the tensor ops are tc.cp of N columns instead of tc.mma
  uint64_t out_addr;
  uint64_t stat_addr;
} kernel_arg_t;

#endif
