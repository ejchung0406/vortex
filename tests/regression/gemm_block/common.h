#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#ifndef VX_CFG_NUM_THREADS
#define VX_CFG_NUM_THREADS 4
#endif

#ifndef ITYPE
#define ITYPE fp16
#endif

#ifndef OTYPE
#define OTYPE fp32
#endif

// M tiles each warp computes from one read of its B tiles
#ifndef MTILES
#define MTILES 1
#endif

// how a CTA brings a chunk of B into shared memory
#define STAGE_LOAD 0  // its threads copy it with coalesced loads
#define STAGE_DXA  1  // the DXA copies it

#define MAX_DEPTH 8

typedef struct {
  uint32_t M, N, K;
  uint32_t chunk_k;  // K elements of B staged per chunk
  uint32_t stage;    // STAGE_LOAD or STAGE_DXA
  uint32_t depth;    // chunk buffers per CTA: chunks outstanding at once
  uint32_t bw_cores; // a CTA on a core below this multiplies bw_tiles A tiles,
  uint32_t bw_tiles; // one on any other core all MTILES
  uint64_t A_addr;
  uint64_t B_addr;
  uint64_t C_addr;
  uint64_t core_addr;   // uint32 per CTA: the core it ran on
  uint64_t cycles_addr; // uint32 per warp: the cycles of its K walk
  uint64_t begin_addr;  // uint32 per warp: the cycle its K walk began
} kernel_arg_t;

#endif
