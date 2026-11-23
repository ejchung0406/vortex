#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// Each warp-iteration owns a region of REGION 8-byte elements per 8 lanes.
// Pattern p maps lane l (of a group of 8) to element lane_elem[p][l] of the
// region: 64 bytes = 8 elements, so the patterns put a group's lanes in one,
// two, three or eight 64-byte words, in lane order or not.
#define NUM_PATTERNS 9
#define REGION       64
#define STORE_PATTERNS 8 // the last pattern repeats elements: loads only

static const uint8_t lane_elem[NUM_PATTERNS][8] = {
  { 0,  1,  2,  3,  4,  5,  6,  7}, // one word
  { 0,  1,  2,  3,  8,  9, 10, 11}, // lanes 0-3 one word, 4-7 the next
  { 8,  9, 10, 11,  0,  1,  2,  3}, // the same, words swapped
  { 0,  8,  1,  9,  2, 10,  3, 11}, // two words, alternating lanes
  { 0,  8, 16,  1,  9, 17,  2, 10}, // three words
  { 0,  9, 18, 27, 36, 45, 54, 63}, // eight words
  { 7,  6,  5,  4,  3,  2,  1,  0}, // one word, lanes reversed
  { 4,  5,  6,  7, 12, 13, 14, 15}, // upper halves of two words
  { 0,  0,  8,  8,  8,  0, 16, 16}, // shared elements in three words
};

static inline uint64_t src_value(uint64_t e) {
  return e * 0x9E3779B97F4A7C15ull + 1;
}

typedef struct {
  uint32_t num_iters;   // iterations per warp
  uint32_t num_threads; // total threads launched
  uint32_t warp_size;   // threads per warp
  uint32_t groups;      // groups of 8 lanes per warp (rounded up)
  uint64_t src_addr;    // in: src_value(e) at element e
  uint64_t dst_addr;    // in: zero; out: the stores below
  uint64_t err_addr;    // out: per-thread count of wrong loads
} kernel_arg_t;

#endif
