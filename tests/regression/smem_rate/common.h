#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

// Instruction classes; each loop iteration is one inline-asm block whose
// instruction count is checked in the kernel disassembly.
#define WR_INT_DEP     0  // 128 dependent adds
#define WR_INT_IND4    1  // 128 adds on 4 independent chains
#define WR_INT_IND8    2  // 128 adds on 8 independent chains
#define WR_FMA_DEP     3  // 128 dependent fmadd.s
#define WR_FMA_IND4    4  // 128 fmadd.s on 4 chains
#define WR_FMA_IND8    5  // 128 fmadd.s on 8 chains
#define WR_BR1         6  // 128 taken branches
#define WR_BR4         7  // 32 x (3 adds + taken branch)
#define WR_BR16        8  // 8 x (15 adds + taken branch)
#define WR_LDS_OWN     9  // 16 x (8 loads of the thread's own shared word + 8 xors)
#define WR_LDS_CHASE  10  // 128 dependent shared loads (self pointer)
#define WR_STS_OWN    11  // 128 stores of the thread's own shared word
#define WR_LDS_NOUSE  12  // 128 shared loads into 8 rotating registers, never read
#define WR_STS_OWN4   13  // 128 4-byte stores of the thread's own word in a 4-byte-spaced array
#define WR_LDS_NOUSE4 14  // 128 4-byte loads from a 4-byte-spaced array, never read
#define WR_STS_OWN8   15  // 128 word-wide stores of the thread's own shared word
#define WR_CSR        16  // 128 reads of a constant CSR
#define WR_SPLITJOIN  17  // 64 x (split on an all-true predicate + join)
#define WR_BAR        18  // 128 barriers the warps of the CTA all reach (no wait when W = 1)
#define WR_DIV        19  // 128 dependent remuw
#define WR_LDS_OWN4   20  // 16 x (8 loads of the thread's own 4-byte-spaced word + 8 xors)
#define WR_STS_SAME   21  // 128 stores of one word per warp from every thread: one bank, a pass per lane
#define WR_NKINDS     22

typedef struct {
  uint32_t kind;
  uint32_t iters;
  uint64_t out_addr;  // per warp: start cycle, end cycle; per thread: its registers; the word size
} kernel_arg_t;

#endif
