#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_intrinsics.h>

// The tensor core by itself: a warp loads its operands once and then does
// nothing but multiply them, so the loop holds no loads and its rate is the
// unit's. Each multiply adds A B to one of `acc` accumulator tiles, taken in
// turn; all MAX_ACC tiles are stored, the unused ones as zeros.
//
// Only one accumulator keeps the loop free of other work: the multiply takes
// and returns its accumulator in f0-f7, so a second one is moved in and out
// of them (fmv) and four no longer fit the registers and go through the stack.
namespace vt = vortex::tensor;
using wm = vt::wmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, 8, 0, vt::ldst_matrix_enabled>;
#ifdef VX_CFG_TCU_WGMMA_ENABLE
using wg = vt::wgmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, 8, vt::ldst_matrix_enabled>;
#endif

template <uint32_t NACC, typename F>
static inline __attribute__((always_inline)) void mma_loop(uint32_t iters, F&& mma) {
  for (uint32_t i = 0; i < iters; i += NACC) {
    vt::unroll_for<NACC>(mma);
  }
}

// The timed loop of one warp, recorded in its slot of the cycles buffer.
template <typename F>
static inline __attribute__((always_inline)) void timed_loop(kernel_arg_t* __UNIFORM__ arg, uint32_t slot, F&& mma) {
#ifdef PROFILE_ENABLE
  __rdcycle_time t0 = vx_rdcycle_sync_begin();
#endif
  switch (arg->acc) {
  case 1:  mma_loop<1>(arg->iters, mma); break;
  case 2:  mma_loop<2>(arg->iters, mma); break;
  default: mma_loop<MAX_ACC>(arg->iters, mma); break;
  }
#ifdef PROFILE_ENABLE
  __rdcycle_time t1 = vx_rdcycle_sync_end();
  auto pT = reinterpret_cast<uint64_t*>(arg->cycles_addr) + 2 * slot;
  pT[0] = ((uint64_t)t0.hi << 32) | t0.lo;
  pT[1] = ((uint64_t)t1.hi << 32) | t1.lo;
#else
  (void)slot;
#endif
}

// One warp per CTA: A (tileM x tileK, row-major) and B (tileK x tileN,
// column-major) are read into fragments once.
static inline __attribute__((always_inline)) void run_wmma(kernel_arg_t* __UNIFORM__ arg) {
  uint32_t cta = blockIdx.x;
  auto pA = reinterpret_cast<wm::input_t *>(arg->A_addr) + cta * wm::tileM * wm::tileK;
  auto pB = reinterpret_cast<wm::input_t *>(arg->B_addr) + cta * wm::tileN * wm::tileK;
  auto pC = reinterpret_cast<wm::output_t *>(arg->C_addr) + cta * MAX_ACC * wm::tileM * wm::tileN;

  wm::fragment_a   fragA;
  wm::fragment_b   fragB;
  wm::fragment_acc fragC[MAX_ACC];

  vt::unroll_for<MAX_ACC>([&](auto j) {
    wm::fill_fragment(fragC[decltype(j)::value], 0);
  });
  wm::load_matrix_sync(fragA, pA, wm::tileK);
  wm::load_matrix_sync<vt::col_major>(fragB, pB, wm::tileK);

  timed_loop(arg, cta, [&](auto j) {
    constexpr uint32_t a = decltype(j)::value;
    wm::mma_sync(fragC[a], fragA, fragB, fragC[a]);
  });

  vt::unroll_for<MAX_ACC>([&](auto j) {
    constexpr uint32_t a = decltype(j)::value;
    wm::store_matrix_sync(pC + a * wm::tileM * wm::tileN, fragC[a], wm::tileN);
  });
}

#ifdef VX_CFG_TCU_WGMMA_ENABLE
// A warpgroup per CTA: the warps stage A (xtileM rows each) and the B they
// share (tileK x xtileN) into shared memory once, block-major, and every
// multiply reads both from there through the same two descriptors.
static inline __attribute__((always_inline)) void run_wgmma(kernel_arg_t* __UNIFORM__ arg) {
  uint32_t tid = threadIdx.x;
  uint32_t num_threads = blockDim.x;
  uint32_t warp_rank = tid / VX_CFG_NUM_THREADS;
  uint32_t num_warps = num_threads / VX_CFG_NUM_THREADS;
  uint32_t cta = blockIdx.x;
  uint32_t cta_M = num_warps * wg::xtileM;

  auto pA = reinterpret_cast<wg::input_t *>(arg->A_addr) + cta * cta_M * wg::tileK;
  auto pB = reinterpret_cast<wg::input_t *>(arg->B_addr) + cta * wg::tileK * wg::xtileN;
  auto pC = reinterpret_cast<wg::output_t *>(arg->C_addr)
          + (cta * MAX_ACC * cta_M + warp_rank * wg::xtileM) * wg::xtileN;

  auto A_smem = reinterpret_cast<wg::input_t *>(__local_mem());
  auto B_smem = A_smem + cta_M * wg::tileK;

  wg::fragment_acc fragC[MAX_ACC];
  vt::unroll_for<MAX_ACC>([&](auto j) {
    wg::fill_fragment(fragC[decltype(j)::value], 0);
  });

  for (uint32_t i = tid; i < cta_M * wg::tileK; i += num_threads) {
    A_smem[wg::a_blockmajor_idx(i / wg::tileK, i % wg::tileK)] = pA[i];
  }
  for (uint32_t i = tid; i < wg::tileK * wg::xtileN; i += num_threads) {
    B_smem[wg::b_blockmajor_idx(i / wg::xtileN, i % wg::xtileN)] = pB[i];
  }
  __syncthreads();

  auto desc_a = vt::vx_make_smem_desc(A_smem + warp_rank * wg::a_warp_elems, 0);
  auto desc_b = vt::vx_make_smem_desc(B_smem, 0);

  timed_loop(arg, cta * num_warps + warp_rank, [&](auto j) {
    constexpr uint32_t a = decltype(j)::value;
    wg::wgmma_sync(fragC[a], desc_a, desc_b, fragC[a]);
  });

  vt::unroll_for<MAX_ACC>([&](auto j) {
    constexpr uint32_t a = decltype(j)::value;
    wg::store_matrix_sync(pC + a * cta_M * wg::xtileN, fragC[a], wg::xtileN);
  });
}
#endif

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
#ifdef VX_CFG_TCU_WGMMA_ENABLE
  if (arg->mode == MODE_WGMMA) {
    run_wgmma(arg);
    return;
  }
#endif
  run_wmma(arg);
}
