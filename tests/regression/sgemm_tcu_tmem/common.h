#ifndef _SGEMM_TCU_WG_DXA_COMMON_H_
#define _SGEMM_TCU_WG_DXA_COMMON_H_

#include <stdint.h>

#ifndef WGMMA_NRC
  #define WGMMA_NRC 8
#endif

#ifndef ITYPE
#define ITYPE fp16
#endif

#ifndef OTYPE
#define OTYPE fp32
#endif

// Software-pipeline depth for DEEP_BUF (N-stage generalization of
// DOUBLE_BUF). Shared between kernel.cpp (loop structure) and main.cpp
// (SMEM sizing) so they can't drift out of sync.
#ifndef PIPE_N
#define PIPE_N 4
#endif

// Accumulator chunks per CTA tile: an MMA covers TMEM_NCHUNK register-file
// tiles along N, drained through the registers one chunk at a time.
#ifndef TMEM_NCHUNK
  #define TMEM_NCHUNK 1
#endif

// K-tiles per DEEP_BUF pipeline stage (kernel.cpp: KB). 1 = one K-tile per
// stage (original behavior).
#ifndef KB
  #define KB 1
#endif

// Shared memory starts with the CTA's TMEM column, padded to keep the stages
// on an LMEM bank row (128 B with 16 XLEN=64 banks), 64 B at least.
#ifndef TMEM_SLOT_BYTES
  #define TMEM_BANK_ROW_BYTES (VX_CFG_LMEM_NUM_BANKS * (VX_CFG_XLEN / 8))
  #define TMEM_SLOT_BYTES ((TMEM_BANK_ROW_BYTES > 64) ? TMEM_BANK_ROW_BYTES : 64)
#endif

typedef struct {
  uint32_t M, N, K;
  uint64_t A_addr;
  uint64_t B_addr;
  uint64_t C_addr;
  uint64_t meta_addr;
} kernel_arg_t;

#endif
