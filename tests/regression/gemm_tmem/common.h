#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#ifndef ITYPE
#define ITYPE fp16
#endif

#ifndef OTYPE
#define OTYPE fp32
#endif

// Accumulator registers of an epilogue transfer.
#define GEMM_NRC 32

// Columns of one tc.mma (the N tile), at most 128 fragments.
#ifndef TILE_N
#define TILE_N 512
#endif

// 64-row M tiles a CTA multiplies with each staged B slab; their accumulators
// take MTILES * TILE_N TMEM columns.
#ifndef MTILES
#define MTILES 2
#endif

// tc.mma K steps per pipeline stage.
#ifndef KSTEPS
#define KSTEPS 2
#endif

// Pipeline stages: buffers of a stage's A and B slabs in shared memory.
#ifndef PIPE
#define PIPE 5
#endif

// Warps that drain the accumulators, from rank 4 (a multiple of the 4 TCU
// blocks: warp r reads block r % 4's lanes).
#ifndef EPI_WARPS
#define EPI_WARPS 8
#endif

// Shared memory starts with the CTA's TMEM column, padded to a bank row.
#define TMEM_SLOT_BYTES 128

typedef struct {
  uint32_t M, N, K;
  uint32_t b_wrap;      // tiles of N tile nt stage the B slabs of N tile nt % b_wrap
  uint64_t A_addr;      // packed [M/64][K/kstep][64][kstep] (PLAIN_LAYOUT: [M][K])
  uint64_t B_addr;      // packed [N/TILE_N][K/kstep][TILE_N][kstep] (PLAIN_LAYOUT: W[N][K])
  uint64_t C_addr;      // [M][N] fp32
  uint64_t times_addr;  // per tile: core, multiply begin, multiply end, epilogue end;
                        // then per CTA: begin, end
} kernel_arg_t;

#endif
