#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// bytes of shared memory each warp works in
#define WM_REGION 1024

// offsets of the cases in a warp's region; t is the thread within the warp
#define WM_BYTES   0    // sb at +t: 8 lanes per bank word, all different bytes
#define WM_HALVES  64   // sh at +2t
#define WM_WORDS   128  // sw at +4t
#define WM_SAME    256  // every thread's sw to one word: the highest thread's stays
#define WM_PAIRS   272  // 8-byte slot per 8 threads: sw at +4 * (t % 2), four lanes on each half
#define WM_QUADS   336  // 8-byte slot per 8 threads: sh at +2 * (t % 4), two lanes on each quarter
#define WM_RDBACK  400  // sw at +4t, read back by the same thread
#define WM_RESULT  528  // what it read back, complemented, sw at +4t
#define WM_SEQ     656  // sw at +4t, then sb at +4t+1: the later store's byte stays

// one bit per case, in the order above (bit 6: read back and result, bit 7: sequence)
#define WM_ALL 0xff

typedef struct {
  uint64_t out_addr;  // WM_REGION / 8 words per warp
  uint32_t cases;     // the cases to run
} kernel_arg_t;

#endif
