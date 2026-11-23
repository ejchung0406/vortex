#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// bytes of shared memory each warp works in
#define WM_REGION 2048

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
#define WM_MASKED  784  // sw at +4t from two threads of every three: a partial thread mask
#define WM_XROW    1024 // per 8 threads: sw at +0, sw one bank row further (+128), sw at +4:
                        // a lane of another word of the bank between two that could share one
#define WM_XWARP   1280 // sw at +4t, read by the next warp after a barrier
#define WM_XWRES   1408 // what a warp read from the next one's WM_XWARP, sw at +4t
#define WM_ROW     128  // bytes of a bank row at the 16-bank, 8-byte shapes the case is laid out for

// one bit per case, in the order above (bit 6: read back and result, bit 7:
// sequence, bit 8: masked, bit 9: other row, bit 10: across warps)
#define WM_ALL 0x7ff

typedef struct {
  uint64_t out_addr;  // WM_REGION / 8 words per warp
  uint32_t cases;     // the cases to run
} kernel_arg_t;

#endif
