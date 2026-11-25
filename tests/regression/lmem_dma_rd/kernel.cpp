#include <vx_spawn2.h>
#include <vx_barrier.h>
#include <vx_dxa.h>
#include <vx_tmem.h>

#include "common.h"

namespace tm = vortex::tmem;

static_assert(VX_CFG_NUM_THREADS == kThreads);
static_assert(VX_CFG_NUM_TCU_BLOCKS == kWarps);
static_assert(VX_CFG_LMEM_NUM_BANKS * (VX_CFG_XLEN / 8) == kRowWords * sizeof(uint32_t));

struct fragment_t {
  static constexpr uint32_t NR = 8;
  float data[NR];
};

static void snapshot(float* out, const uint32_t* region, uint32_t column, uint32_t barrier, uint32_t lane) {
  tm::cp(column, region, kTmemCols);
  tm::commit(barrier, 2);
  vx_barrier(barrier, 2);
  constexpr uint32_t cols_per_load = fragment_t::NR * kThreads / tm::block_lanes();
  for (uint32_t c = 0; c < kTmemCols; c += cols_per_load) {
    fragment_t fragment;
    tm::load<tm::LANE_PER_THREAD>(fragment, tm::make_taddr(tm::warp_lane(), column + c));
    for (uint32_t r = 0; r < fragment_t::NR; ++r) {
      out[c * kThreads + r * kThreads + lane] = fragment.data[r];
    }
  }
}

static void sample(uint32_t* out, const volatile uint32_t* region, uint32_t lane) {
  // One lane per bank in each half-row probe.
  out[lane] = region[kDmaLowProbe + 2 * lane];
  out[kThreads + lane] = region[kDmaHigh + 2 * lane];
  out[2 * kThreads + lane] = region[kOtherLoad + 2 * lane];
}

static void update(uint32_t* out, volatile uint32_t* region, uint32_t word, uint32_t generation, uint32_t lane) {
  auto ptr = region + word;
  *ptr = value(2 * generation - 1, word);
  uint32_t old;
  __asm__ volatile ("amoadd.w %0, %2, (%1)" : "=&r"(old) : "r"(ptr), "r"(1u << kGenShift) : "memory");
  out[lane] = old;
  out[kThreads + lane] = *ptr;
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto region = reinterpret_cast<uint32_t*>(__local_mem());
  auto out = reinterpret_cast<uint32_t*>(arg->out_addr);
  auto tcu_out = reinterpret_cast<float*>(out + kTcuOut);
  const uint32_t tid = threadIdx.x;
  const uint32_t lane = tid % kThreads;
  const uint32_t warp = tid / kThreads;
  const uint32_t tcu_barrier = (1u << 8) | get_local_group_id();
  constexpr uint32_t allocated_cols = tm::alloc_cols(kTmemCols);
  uint32_t column = 0;

  for (uint32_t i = tid; i < kRegionWords; i += blockDim.x) {
    region[i] = value(0, i);
  }
  __syncthreads();
  if (warp == 0) {
    column = tm::alloc(allocated_cols);
    snapshot(tcu_out, region, column, tcu_barrier, lane);
  }
  __syncthreads();

  if (warp == 0) {
    for (uint32_t s = 1; s <= kSnapshots; ++s) {
      snapshot(tcu_out + s * kTmemWords, region, column, tcu_barrier, lane);
    }
  } else if (warp == 1) {
    vortex::barrier bar(2, 1);
    for (uint32_t g = 0; g < kDmaGens; ++g) {
      bar.expect_tx(2);
      vx_dxa_issue_1d_wg(0, bar.id(), region + kDmaLow, (2 * g) * kTileWords);
      vx_dxa_issue_1d_wg(0, bar.id(), region + kDmaHigh, (2 * g + 1) * kTileWords);
      bar.arrive_and_wait();
    }
  } else if (warp == 2) {
    for (uint32_t s = 0; s < kSamples; ++s) {
      sample(out + kLoadOut + s * 3 * kThreads, region, lane);
    }
  } else {
    for (uint32_t g = 1; g <= kWriterGens; ++g) {
      auto dst = out + kStoreOut + (g - 1) * 4 * kThreads;
      update(dst, region, kSameStore + 2 * lane, g, lane);
      update(dst + 2 * kThreads, region, kOtherStore + 2 * lane, g, lane);
    }
  }

  __syncthreads();
  if (warp == 0) {
    snapshot(tcu_out + (kSnapshots + 1) * kTmemWords, region, column, tcu_barrier, lane);
    tm::dealloc(column, allocated_cols);
  } else if (warp == 2) {
    sample(out + kLoadOut + kSamples * 3 * kThreads, region, lane);
  }
  for (uint32_t i = tid; i < kRegionWords; i += blockDim.x) {
    out[kFinalOut + i] = region[i];
  }
}
