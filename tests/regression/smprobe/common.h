#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

#define PROBE_SMEM_BAR  0
#define PROBE_BAR_ONLY  1
#define PROBE_SMEM_ONLY 2

#define NEIGH_IDLE      0
#define NEIGH_LOADS     1
#define NEIGH_LOADS_BAR 2

typedef struct {
  uint64_t src_addr;    // the neighbours' stream, nchunks chunks of 8 KB
  uint64_t flag_addr;   // the probe sets it when done; the neighbours stop
  uint64_t itcyc_addr;  // probe: cycles of each iteration
  uint64_t sums_addr;   // one word per thread
  uint32_t nchunks;
  uint32_t probe_kind;
  uint32_t neighbours;
  uint32_t probe_iters;
  uint32_t delay;       // cycles the probe waits before its first iteration
  uint32_t max_neigh;   // a neighbour stops after this many iterations even if the probe runs on
} kernel_arg_t;

#endif
