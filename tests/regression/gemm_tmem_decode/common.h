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

// Columns of one tc.mma (the N tile of a CTA), at most 128 fragments. A CTA
// keeps two tiles' accumulators in TMEM, so one drains while the next fills.
#ifndef TILE_N
#define TILE_N 512
#endif

// tc.mma K steps per pipeline stage.
#ifndef KSTEPS
#define KSTEPS 1
#endif

// Pipeline stages: buffers of a stage's A and W slabs in shared memory.
#ifndef PIPE
#define PIPE 5
#endif

// Bytes of W one staging transfer (or one load-staging pass) moves.
#ifndef XFER
#define XFER (TILE_N * 32)
#endif

// How W sits in global memory:
#define LAYOUT_PACKED 0  // [N/TILE_N][K/kstep][TILE_N][kstep]: a stage is one contiguous run
#define LAYOUT_NK16   1  // W[N][K]: rows of one K step (32 B), each 64 B line read twice
#define LAYOUT_NK32   2  // W[N][K]: rows of two K steps (one 64 B line), read once
#define LAYOUT_KN     3  // W[K][N]: transposed by the DXA's K-major scatter
#ifndef LAYOUT
#define LAYOUT LAYOUT_PACKED
#endif

// How a stage reaches shared memory: 0 the DXA, 1 loads by LOAD_WARPS warps.
#ifndef STAGE_LOAD
#define STAGE_LOAD 0
#endif

#ifndef LOAD_WARPS
#define LOAD_WARPS 16
#endif

// 1: the DXA producer and the MMA warp run the ring unrolled by PIPE (fewer
// instructions per stage); 0: the loop indexes the ring with g % PIPE.
#ifndef LEAN_ISSUE
#define LEAN_ISSUE 0
#endif

// Warps that read the stages with loads instead of the tensor engine (0: the engine).
#ifndef CONSUME_WARPS
#define CONSUME_WARPS 0
#endif

// Shared memory starts with the CTA's TMEM column, padded to a bank row.
#define TMEM_SLOT_BYTES 128

// Warps: 0 the DXA producer, 1 the MMA issuer, 4..7 the epilogue (warp r
// drains TCU block r % 4), from 8 the loaders when STAGE_LOAD.
#define EPI_WARP0  4
#define LOAD_WARP0 8

typedef struct {
  uint32_t M, N, K;
  uint32_t passes;       // times the CTAs walk the N tiles (a copy that re-reads W from the L2)
  uint64_t A_addr;      // [K/kstep][M][kstep]
  uint64_t W_addr;      // LAYOUT
  uint64_t C_addr;      // [16 * ceil(M / 16)][N] fp32
  uint64_t times_addr;  // per tile: core, multiply begin, multiply end, epilogue end;
                        // then per CTA: begin, end
} kernel_arg_t;

#endif
