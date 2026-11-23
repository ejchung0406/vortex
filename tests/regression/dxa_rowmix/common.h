#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#define ROWMIX_MAX_XFERS 8

typedef struct {
  uint64_t out_addr;
  uint32_t xfers;   // transfers in flight per CTA and round (1..8)
  uint32_t rounds;
  uint32_t size0;   // source extent in elements
  uint32_t size1;
  uint32_t tile0;
  uint32_t tile1;
  uint32_t region;  // shared-memory bytes per transfer
} kernel_arg_t;

// Destination of transfer x inside its region: line-aligned at either half of
// a 128-byte row, and 4-byte offsets whose lines straddle two row parts.
static inline uint32_t rowmix_off(uint32_t x) {
  switch (x & 7) {
  case 0: return 0;
  case 1: return 64;
  case 2: return 4;
  case 3: return 100;
  case 4: return 128;
  case 5: return 68;
  case 6: return 32;
  default: return 124;
  }
}

// Tile origin: tiles run past the source's bottom edge, so some lines are
// filled with the descriptor's zero. They stay inside its right edge: the RTL
// checks the outer dimensions only and reads the source past a row's end,
// where SimX writes the fill value.
static inline uint32_t rowmix_c0(uint32_t b, uint32_t r, uint32_t x, uint32_t size0, uint32_t tile0) {
  return (x * 37 + r * 53 + b * 11) % (size0 - tile0 + 1);
}
static inline uint32_t rowmix_c1(uint32_t b, uint32_t r, uint32_t x, uint32_t size1) {
  return (x * 5 + r * 3 + b * 7) % size1;
}

#endif
