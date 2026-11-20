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

// the two roles a CTA takes from its core, and kernel_arg_t::idle_role's values
#define ROLE_NONE 0
#define ROLE_BW   1  // memory-bound: a core below bw_cores
#define ROLE_CP   2  // compute-bound: any other core

typedef struct {
  uint32_t M, N, K;
  uint32_t chunk_k;  // K elements of B staged per chunk
  uint32_t stage;    // STAGE_LOAD or STAGE_DXA
  uint32_t depth;    // chunk buffers per CTA: chunks outstanding at once
  uint32_t bw_cores; // a CTA on a core below this multiplies bw_tiles A tiles,
  uint32_t bw_tiles; // one on any other core all MTILES
  uint32_t bw_wrap;  // CTA b stages the B columns of CTA b % bw_wrap in the
  uint32_t cp_wrap;  // memory-bound role, of CTA b % cp_wrap in the other
  uint32_t idle_role;  // the role whose CTAs do no work, or ROLE_NONE
  uint32_t last_cta;   // the grid's last CTA: a CTA of the idle role waits for its record
  uint64_t A_addr;
  uint64_t B_addr;
  uint64_t C_addr;
  uint64_t core_addr;   // uint32 per CTA: the core it ran on
  uint64_t cycles_addr; // uint32 per warp: the cycles of its K walk
  uint64_t begin_addr;  // uint32 per warp: the cycle its K walk began
} kernel_arg_t;

#endif
