#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#ifndef ITYPE
#define ITYPE fp16
#endif

#ifndef OTYPE
#define OTYPE fp32
#endif

// Accumulator registers of an epilogue transfer: 32 registers drain
// 32 * NT / M columns per tmem.ld.
#define ROOF_NRC 32

#define MODE_A_SMEM   0 // A row-major in shared memory, B K-major (prefetched)
#define MODE_B_BLOCK  1 // A row-major in shared memory, B block-major (no prefetch)
#define MODE_A_TMEM   2 // A copied into TMEM by tc.cp, B K-major
#define MODE_B_CHUNK  3 // A row-major in shared memory, B as [column][k] over all slabs

#define ISSUE_LOOP    0 // one warp, a loop iteration per tc.mma
#define ISSUE_UNROLL  1 // one warp, four tc.mma per iteration, descriptors in registers (4 slabs)
#define ISSUE_WARPS   2 // every warp of the CTA issues a share of the ops
#define ISSUE_PAD     3 // ISSUE_LOOP with eight more ALU instructions per iteration

// Shared memory starts with the CTA's TMEM column, padded to a bank row.
#define TMEM_SLOT_BYTES 128

typedef struct {
  uint32_t mode;
  uint32_t N;        // tile width, columns of one tc.mma
  uint32_t iters;    // tc.mma ops per CTA, a multiple of slabs
  uint32_t slabs;    // distinct K steps staged, a power of two
  uint32_t cp_batch; // K steps of A one tc.cp copies (MODE_A_TMEM), divides slabs
  uint32_t issue;    // ISSUE_LOOP, ISSUE_UNROLL or ISSUE_WARPS (MODE_A_SMEM)
  uint64_t src_addr; // per CTA: A of every slab, then B of every slab
  uint64_t C_addr;
  uint64_t cycles_addr; // per CTA: the cycle its multiplies began and ended
} kernel_arg_t;

#endif
