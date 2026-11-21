#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_tmem.h>
#include <vx_intrinsics.h>
#include <vx_barrier.h>

// The tensor-memory engine by itself: the CTA stages `slabs` K steps of A and
// B into shared memory once, and one warp then issues nothing but tc.mma, each
// a K step that reads its A and B from shared memory again and accumulates
// into the same TMEM columns. One tc.commit after the last op ends the timed
// phase.
namespace vt = vortex::tensor;
namespace tm = vortex::tmem;
using ctx = vt::wgmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, ROOF_NRC>;
using wg_cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, ROOF_NRC>;

// One K step: fedpK words per A row and per B column.
constexpr uint32_t kStepBytes = wg_cfg::fedpK * 4;

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t tid = threadIdx.x;
  const uint32_t num_threads = blockDim.x;
  const uint32_t warp_rank = tid / VX_CFG_NUM_THREADS;
  const uint32_t num_warps = num_threads / VX_CFG_NUM_THREADS;
  const uint32_t cta = blockIdx.x;
  const uint32_t N = arg->N;
  const uint32_t slabs = arg->slabs;
  const uint32_t mode = arg->mode;

  const uint32_t cta_M = num_warps * ctx::xtileM;
  const uint32_t a_bytes = cta_M * kStepBytes;  // one slab of A
  const uint32_t b_bytes = N * kStepBytes;      // one slab of B
  const uint32_t src_words = slabs * (a_bytes + b_bytes) / 4;

  auto lmem = reinterpret_cast<uint8_t*>(__local_mem());
  auto tmem_slot = reinterpret_cast<volatile uint32_t*>(lmem);
  auto smem = reinterpret_cast<uint32_t*>(lmem + TMEM_SLOT_BYTES);
  auto src = reinterpret_cast<const uint32_t*>(arg->src_addr) + cta * src_words;

  const uint32_t cta_id = get_local_group_id();
  const uint32_t sync_bar = cta_id | (0 << 8);
  const uint32_t done_bar = cta_id | (1 << 8);

  const uint32_t a_cols = (mode == MODE_A_TMEM) ? slabs * wg_cfg::fedpK : 0;
  const uint32_t tmem_cols = tm::alloc_cols(N + a_cols);
  if (warp_rank == 0) {
    uint32_t col = tm::alloc(tmem_cols);
    if (vx_thread_id() == 0) {
      *tmem_slot = col;
    }
  }
  for (uint32_t i = tid; i < src_words; i += num_threads) {
    smem[i] = src[i];
  }
  vx_barrier(sync_bar, num_warps);
  const uint32_t taddr = *tmem_slot;
  const uint32_t a_col = taddr + N;

  if (warp_rank == 0) {
    const uint32_t a_off = tm::smem_offset(smem);
    const uint32_t b_off = a_off + slabs * a_bytes;
    // MODE_B_CHUNK: a column holds its K steps back to back, so a step is
    // kStepBytes along the column and the columns are a chunk apart.
    const uint32_t ldb = (mode == MODE_B_BLOCK) ? 0 : (mode == MODE_B_CHUNK) ? slabs * kStepBytes : kStepBytes;
    const uint32_t b_step = (mode == MODE_B_CHUNK) ? kStepBytes : b_bytes;
    const uint32_t n_frag = N / ctx::tcN;
    const uint32_t iters = arg->iters;
    const uint32_t cp_batch = arg->cp_batch;
    __rdcycle_time t0 = vx_rdcycle_sync_begin();
    if (mode == MODE_A_TMEM) {
      for (uint32_t i = 0; i < iters; ++i) {
        uint32_t s = i & (slabs - 1);
        if ((s & (cp_batch - 1)) == 0) {
          tm::cp(a_col + s * wg_cfg::fedpK, reinterpret_cast<uint8_t*>(smem) + s * a_bytes,
                 cp_batch * wg_cfg::fedpK);
        }
        tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, i == 0),
                tm::tmem_col{a_col + s * wg_cfg::fedpK},
                tm::smem_desc(b_off + s * b_step, ldb));
      }
    } else if (arg->issue == ISSUE_UNROLL) {
      const auto acc = tm::make_idesc<vt::ITYPE>(n_frag, taddr, false);
      const auto a0 = tm::smem_desc(a_off, kStepBytes), a1 = tm::smem_desc(a_off + a_bytes, kStepBytes);
      const auto a2 = tm::smem_desc(a_off + 2 * a_bytes, kStepBytes), a3 = tm::smem_desc(a_off + 3 * a_bytes, kStepBytes);
      const auto b0 = tm::smem_desc(b_off, ldb), b1 = tm::smem_desc(b_off + b_step, ldb);
      const auto b2 = tm::smem_desc(b_off + 2 * b_step, ldb), b3 = tm::smem_desc(b_off + 3 * b_step, ldb);
      tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, true), a0, b0);
      tm::mma(acc, a1, b1);
      tm::mma(acc, a2, b2);
      tm::mma(acc, a3, b3);
      for (uint32_t i = 4; i < iters; i += 4) {
        tm::mma(acc, a0, b0);
        tm::mma(acc, a1, b1);
        tm::mma(acc, a2, b2);
        tm::mma(acc, a3, b3);
      }
    } else if (arg->issue == ISSUE_PAD) {
      for (uint32_t i = 0; i < iters; ++i) {
        uint32_t s = i & (slabs - 1);
        __asm__ volatile ("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop");
        tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, i == 0),
                tm::smem_desc(a_off + s * a_bytes, kStepBytes),
                tm::smem_desc(b_off + s * b_step, ldb));
      }
    } else if (arg->issue == ISSUE_WARPS) {
      tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, true),
              tm::smem_desc(a_off, kStepBytes), tm::smem_desc(b_off, ldb));
    } else {
      for (uint32_t i = 0; i < iters; ++i) {
        uint32_t s = i & (slabs - 1);
        tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, i == 0),
                tm::smem_desc(a_off + s * a_bytes, kStepBytes),
                tm::smem_desc(b_off + s * b_step, ldb));
      }
    }
    if (arg->issue != ISSUE_WARPS) {
      tm::commit(done_bar, num_warps + 1);
      vx_barrier(done_bar, num_warps + 1);
    } else {
      // The first op is queued; the warps issue the rest between them, in no
      // set order, and each commits what the core has queued by then.
      vx_barrier(sync_bar, num_warps);
      for (uint32_t i = 1; i < iters; i += num_warps) {
        uint32_t s = i & (slabs - 1);
        tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, false),
                tm::smem_desc(a_off + s * a_bytes, kStepBytes),
                tm::smem_desc(b_off + s * b_step, ldb));
      }
      tm::commit(done_bar, 2 * num_warps);
      vx_barrier(done_bar, 2 * num_warps);
    }
    __rdcycle_time t1 = vx_rdcycle_sync_end();
    auto pT = reinterpret_cast<uint64_t*>(arg->cycles_addr) + 2 * cta;
    pT[0] = ((uint64_t)t0.hi << 32) | t0.lo;
    pT[1] = ((uint64_t)t1.hi << 32) | t1.lo;
  } else if (arg->issue != ISSUE_WARPS) {
    vx_barrier(done_bar, num_warps + 1);
  } else {
    const uint32_t a_off = tm::smem_offset(smem);
    const uint32_t b_off = a_off + slabs * a_bytes;
    const uint32_t n_frag = N / ctx::tcN;
    vx_barrier(sync_bar, num_warps);
    for (uint32_t i = 1 + warp_rank; i < arg->iters; i += num_warps) {
      uint32_t s = i & (slabs - 1);
      tm::mma(tm::make_idesc<vt::ITYPE>(n_frag, taddr, false),
              tm::smem_desc(a_off + s * a_bytes, kStepBytes),
              tm::smem_desc(b_off + s * b_bytes, kStepBytes));
    }
    tm::commit(done_bar, 2 * num_warps);
    vx_barrier(done_bar, 2 * num_warps);
  }

  // Warp r drains its block's lanes, M rows [r*xtileM, (r+1)*xtileM).
  const uint32_t lane = tm::warp_lane();
  auto pC = reinterpret_cast<ctx::output_t*>(arg->C_addr) + (cta * cta_M + lane) * N;
  for (uint32_t c = 0; c < N; c += ctx::xtileN) {
    ctx::fragment_acc fragC;
    tm::load<tm::FRAGMENT>(fragC, tm::make_taddr(lane, taddr + c));
    ctx::store_matrix_sync(pC + c, fragC, N);
  }

  vx_barrier(sync_bar, num_warps);
  if (warp_rank == 0) {
    tm::dealloc(taddr, tmem_cols);
  }
}
