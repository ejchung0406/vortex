#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_tmem.h>
#include <vx_intrinsics.h>
#include <vx_dxa.h>
#include <vx_barrier.h>

// Decode-shaped C = A W: a few rows of A (M <= 64, the 64 lanes of one
// tc.mma; lanes past M compute on whatever shared memory holds and are not
// stored) against a weight matrix W read once. A persistent CTA computes N
// tiles cta, cta + grid, ..., each TILE_N columns over all of K, into one of
// two TMEM accumulator buffers, so the epilogue of a tile overlaps the next
// tile's multiplies. Each pipeline stage holds KSTEPS K steps of A and of the
// tile's W, staged by the DXA (warp 0) or by loads (the loader warps); warp 1
// issues one tc.mma per K step. In shared memory a K step of A is M rows of
// 32 bytes and a K step of W is TILE_N rows of 32 bytes (LAYOUT_NK32: rows of
// 64 bytes holding two K steps). NO_MMA (a measurement of the staging
// alone) releases each stage without multiplying. CONSUME_WARPS > 0 replaces
// the tensor engine with that many warps that read each stage of W with loads
// and checksum it (READ_ONLY: the stages are not refilled, the warps only read).
namespace vt = vortex::tensor;
namespace tm = vortex::tmem;
using ctx = vt::wgmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, GEMM_NRC>;
using wg_cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, GEMM_NRC>;

[[maybe_unused]] constexpr uint32_t kDescA = 0;
constexpr uint32_t kDescW = 1;

constexpr uint32_t kBlocks   = VX_CFG_NUM_TCU_BLOCKS;
constexpr uint32_t kStepK    = wg_cfg::fedpK * wg_cfg::i_ratio;  // K elements of one tc.mma
constexpr uint32_t kStepRow  = wg_cfg::fedpK * 4;          // bytes of one row's K step
constexpr uint32_t kRowBytes = (LAYOUT == LAYOUT_NK32) ? 2 * kStepRow : kStepRow;
constexpr uint32_t kStageW   = KSTEPS * TILE_N * kStepRow;
[[maybe_unused]] constexpr uint32_t kXfers = kStageW / XFER;  // W transfers per stage
constexpr uint32_t kNFrag    = TILE_N / wg_cfg::tcN;

static_assert(kBlocks * wg_cfg::xtileM == 64, "a tc.mma covers 64 rows");
static_assert(kNFrag <= 128 && TILE_N % ctx::xtileN == 0, "TILE_N: at most 128 fragments, whole transfers");
static_assert(2 * TILE_N <= VX_CFG_TCU_TMEM_COLS, "two accumulator buffers exceed TMEM");
static_assert(2 * PIPE + 4 <= VX_CFG_NUM_BARRIERS, "not enough barriers for PIPE");
static_assert(kStageW % XFER == 0 && XFER % kRowBytes == 0, "XFER: whole rows, divides a stage");
static_assert(LAYOUT != LAYOUT_NK32 || KSTEPS % 2 == 0, "LAYOUT_NK32 stages K steps in pairs");
static_assert(!STAGE_LOAD || LAYOUT == LAYOUT_PACKED, "load staging copies the packed layout");
static_assert(!(STAGE_LOAD != 0 && CONSUME_WARPS != 0), "the loaders and the consumers share warp ranks");
static_assert(CONSUME_WARPS == 0 || LAYOUT == LAYOUT_PACKED, "the consumers checksum the packed layout");
static_assert(LEAN_ISSUE == 0 || LAYOUT == LAYOUT_PACKED, "the lean producer issues the packed layout");
static_assert(sizeof(ctx::output_t) == sizeof(float), "the epilogue stores fp32 lanes");

typedef uint64_t __attribute__((may_alias)) word_t;

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t warp_rank = threadIdx.x / VX_CFG_NUM_THREADS;
  const uint32_t num_warps = blockDim.x / VX_CFG_NUM_THREADS;
  const uint32_t M = arg->M, N = arg->N;
  const uint32_t k_steps = arg->K / kStepK;
  const uint32_t num_stages = k_steps / KSTEPS;
  const uint32_t num_tiles = N / TILE_N;
  const uint32_t total_tiles = num_tiles * arg->passes;
  const uint32_t cta = blockIdx.x;
  // The A region holds what the 64 lanes of the last K step read: lanes past
  // M must not read W, which the tile buffer fetches at the same time.
  const uint32_t stage_a = ((KSTEPS - 1) * M * kStepRow + 64 * kStepRow + 127) / 128 * 128;
  const uint32_t stage_bytes = stage_a + kStageW;
  // Epilogue warps with lanes below M, and the participants of the barriers
  // between the stagers and the MMA warp.
  const uint32_t epi = (M + wg_cfg::xtileM - 1) / wg_cfg::xtileM;
  const uint32_t stagers = STAGE_LOAD ? LOAD_WARPS : 1;
  // The other side of the stage barriers: the MMA warp, or the consumer warps.
  const uint32_t takers = CONSUME_WARPS ? CONSUME_WARPS : 1;

  auto lmem = reinterpret_cast<uint8_t*>(__local_mem());
  auto tmem_slot = reinterpret_cast<volatile uint32_t*>(lmem);
  uint8_t* stages = lmem + TMEM_SLOT_BYTES;

  const uint32_t cta_id = get_local_group_id();
  auto full_bar  = [&](uint32_t s) { return cta_id | (s << 8); };
  auto empty_bar = [&](uint32_t s) { return cta_id | ((PIPE + s) << 8); };
  auto acc_empty = [&](uint32_t b) { return cta_id | ((2 * PIPE + b) << 8); };
  const uint32_t acc_full = cta_id | ((2 * PIPE + 2) << 8);
  const uint32_t sync_bar = cta_id | ((2 * PIPE + 3) << 8);

  auto pT = reinterpret_cast<uint64_t*>(arg->times_addr);
  auto pCta = pT + 4 * total_tiles + 2 * cta;
  if (warp_rank == 0) {
    uint32_t col = tm::alloc(2 * TILE_N);
    if (vx_thread_id() == 0) {
      *tmem_slot = col;
      pCta[0] = csr_read(VX_CSR_MCYCLE);
    }
  }
  vx_barrier(sync_bar, num_warps);
  const uint32_t taddr = *tmem_slot;

#if LEAN_ISSUE
  if (warp_rank == 0 && !STAGE_LOAD) {
    // DXA producer with the ring unrolled by PIPE, so a stage's barriers and
    // buffer are constants and the source coordinate advances by one add: a
    // lone warp issues one instruction per schedule-to-decode round trip
    // (about 10 cycles), so the instructions per transfer set its rate.
    constexpr uint32_t kStageCoord = kStageW / XFER;
    uint32_t tile = cta, i = 0, ks0 = 0, g = 0;
    uint32_t coord = (tile % num_tiles) * num_stages * kStageCoord;
    bool wrapped = false;
    while (tile < total_tiles) {
      #pragma unroll
      for (uint32_t s = 0; s < PIPE; ++s) {
        if (tile >= total_tiles) break;
        if (wrapped) {
          int phase = vx_barrier_arrive(empty_bar(s), takers + 1);
          vx_barrier_wait(empty_bar(s), phase);
        }
        uint8_t* buf = stages + s * stage_bytes;
      #if defined(READ_ONLY)
        (void)buf;  // the stages keep what they hold
      #elif defined(NO_MMA) || CONSUME_WARPS
        vx_barrier_expect_tx(full_bar(s), kXfers);
      #else
        vx_barrier_expect_tx(full_bar(s), 1 + kXfers);
        vx_dxa_issue_2d_wg(kDescA, full_bar(s), buf, 0, ks0);
      #endif
      #ifndef READ_ONLY
        #pragma unroll
        for (uint32_t q = 0; q < kXfers; ++q) {
          vx_dxa_issue_2d_wg(kDescW, full_bar(s), buf + stage_a + q * XFER, 0, coord + q);
        }
      #endif
        vx_barrier_arrive(full_bar(s), takers + 1);
        ++g;
        coord += kStageCoord;
        ks0 += KSTEPS;
        if (++i == num_stages) {
          i = 0;
          ks0 = 0;
          tile += gridDim.x;
          coord = (tile % num_tiles) * num_stages * kStageCoord;
        }
      }
      wrapped = true;
    }
    (void)ks0;    // the A coordinate, unused when only W is staged
    (void)coord;  // and the W coordinate, when nothing is
    for (uint32_t i = (g > PIPE) ? (g - PIPE) : 0; i < g; ++i) {
      int phase = vx_barrier_arrive(empty_bar(i % PIPE), takers + 1);
      vx_barrier_wait(empty_bar(i % PIPE), phase);
    }
  } else
#endif
  if (warp_rank == 0 && !STAGE_LOAD) {
    // DXA producer: one transfer of A and kXfers of W per stage.
    uint32_t g = 0;
    for (uint32_t tile = cta; tile < total_tiles; tile += gridDim.x) {
      for (uint32_t i = 0; i < num_stages; ++i, ++g) {
        uint32_t s = g % PIPE;
        if (g >= PIPE) {
          int phase = vx_barrier_arrive(empty_bar(s), takers + 1);
          vx_barrier_wait(empty_bar(s), phase);
        }
        uint8_t* buf = stages + s * stage_bytes;
        const uint32_t ks0 = i * KSTEPS;
      #if defined(READ_ONLY)
        // The stages keep what they hold: the consumers measure reads alone.
      #elif defined(NO_MMA) || CONSUME_WARPS
        // Staging alone: the transfers of W only, as a copy of W.
        vx_barrier_expect_tx(full_bar(s), kXfers);
      #else
        vx_barrier_expect_tx(full_bar(s), 1 + kXfers);
        vx_dxa_issue_2d_wg(kDescA, full_bar(s), buf, 0, ks0);
      #endif
      #ifdef READ_ONLY
        for (uint32_t q = 0; q < 0; ++q) {
      #else
        for (uint32_t q = 0; q < kXfers; ++q) {
      #endif
          uint8_t* dst = buf + stage_a + q * XFER;
        #if LAYOUT == LAYOUT_PACKED
          vx_dxa_issue_2d_wg(kDescW, full_bar(s), dst, 0, (((tile % num_tiles) * k_steps + ks0) * TILE_N * kStepRow + q * XFER) / XFER);
        #else
          // Transfer q: rows p * (XFER / kRowBytes) on of its K step (pair) j.
          constexpr uint32_t kPer = TILE_N * kRowBytes / XFER;
          constexpr uint32_t kRows = XFER / kRowBytes;
          const uint32_t j = q / kPer, n0 = (tile % num_tiles) * TILE_N + (q % kPer) * kRows;
          #if LAYOUT == LAYOUT_NK16
          vx_dxa_issue_3d_wg(kDescW, full_bar(s), dst, 0, n0, ks0 + j);
          #elif LAYOUT == LAYOUT_NK32
          vx_dxa_issue_3d_wg(kDescW, full_bar(s), dst, 0, n0, ks0 / 2 + j);
          #else
          vx_dxa_issue_2d_wg(kDescW, full_bar(s), dst, n0, (ks0 + j) * kStepK);
          #endif
        #endif
        }
        vx_barrier_arrive(full_bar(s), takers + 1);
      }
    }
    for (uint32_t i = (g > PIPE) ? (g - PIPE) : 0; i < g; ++i) {
      int phase = vx_barrier_arrive(empty_bar(i % PIPE), takers + 1);
      vx_barrier_wait(empty_bar(i % PIPE), phase);
    }
  } else if (STAGE_LOAD && warp_rank >= LOAD_WARP0) {
    // Loaders: thread t copies words t, t + T, ... of the stage, every load
    // of the stage before its stores.
    constexpr uint32_t kT = LOAD_WARPS * VX_CFG_NUM_THREADS;
    constexpr uint32_t kWords = kStageW / sizeof(word_t) / kT;
    static_assert(kStageW % (sizeof(word_t) * kT) == 0, "a stage is whole words per loader thread");
    const uint32_t t = threadIdx.x - LOAD_WARP0 * VX_CFG_NUM_THREADS;
    const uint32_t a_words = KSTEPS * M * kStepRow / sizeof(word_t);
    auto pA = reinterpret_cast<const word_t*>(arg->A_addr);
    auto pW = reinterpret_cast<const word_t*>(arg->W_addr);
    uint32_t g = 0;
    for (uint32_t tile = cta; tile < total_tiles; tile += gridDim.x) {
      for (uint32_t i = 0; i < num_stages; ++i, ++g) {
        uint32_t s = g % PIPE;
        if (g >= PIPE) {
          int phase = vx_barrier_arrive(empty_bar(s), LOAD_WARPS + 1);
          vx_barrier_wait(empty_bar(s), phase);
        }
        auto dst = reinterpret_cast<word_t*>(stages + s * stage_bytes);
        const uint32_t ks0 = i * KSTEPS;
        auto src = pW + size_t((tile % num_tiles) * k_steps + ks0) * TILE_N * kStepRow / sizeof(word_t) + t;
        word_t v[kWords];
        #pragma unroll
        for (uint32_t w = 0; w < kWords; ++w) {
          v[w] = src[w * kT];
        }
        word_t a = (t < a_words) ? pA[size_t(ks0) * M * kStepRow / sizeof(word_t) + t] : 0;
        #pragma unroll
        for (uint32_t w = 0; w < kWords; ++w) {
          dst[stage_a / sizeof(word_t) + t + w * kT] = v[w];
        }
        if (t < a_words) {
          dst[t] = a;
        }
        vx_barrier_arrive(full_bar(s), LOAD_WARPS + 1);
      }
    }
    for (uint32_t i = (g > PIPE) ? (g - PIPE) : 0; i < g; ++i) {
      int phase = vx_barrier_arrive(empty_bar(i % PIPE), LOAD_WARPS + 1);
      vx_barrier_wait(empty_bar(i % PIPE), phase);
    }
  } else if (CONSUME_WARPS != 0 && warp_rank >= LOAD_WARP0) {
    // Consumers: thread t reads words t, t + T, ... of each stage of W and
    // XORs them; the checksum goes to C for the host to check.
    constexpr uint32_t kT = CONSUME_WARPS * VX_CFG_NUM_THREADS;
    const uint32_t t = threadIdx.x - LOAD_WARP0 * VX_CFG_NUM_THREADS;
    word_t sum = 0;
    uint32_t g = 0;
    for (uint32_t tile = cta; tile < total_tiles; tile += gridDim.x) {
      for (uint32_t i = 0; i < num_stages; ++i, ++g) {
        uint32_t s = g % PIPE;
        int phase = vx_barrier_arrive(full_bar(s), takers + 1);
        vx_barrier_wait(full_bar(s), phase);
        if (i == 0 && t == 0) {
          pT[4 * tile + 0] = vx_core_id();
          pT[4 * tile + 1] = csr_read(VX_CSR_MCYCLE);
        }
        auto w = reinterpret_cast<const volatile word_t*>(stages + s * stage_bytes + stage_a);
        #pragma unroll 4
        for (uint32_t j = t; j < kStageW / sizeof(word_t); j += kT) {
          sum ^= w[j];
        }
        vx_barrier_arrive(empty_bar(s), takers + 1);
      }
      if (t == 0) {
        pT[4 * tile + 2] = csr_read(VX_CSR_MCYCLE);
        pT[4 * tile + 3] = pT[4 * tile + 2];
      }
    }
    reinterpret_cast<word_t*>(arg->C_addr)[cta * kT + t] = sum;
#if LEAN_ISSUE
  } else if (warp_rank == 1 && CONSUME_WARPS == 0) {
    // MMA issuer, the ring unrolled by PIPE as in the producer.
    const uint32_t base = tm::smem_offset(stages);
    uint32_t tile = cta, i = 0, j = 0, d_col = taddr;
    while (tile < total_tiles) {
      #pragma unroll
      for (uint32_t s = 0; s < PIPE; ++s) {
        if (tile >= total_tiles) break;
        if (i == 0) {
          d_col = taddr + (j % 2) * TILE_N;
          if (j >= 2) {
            int phase = vx_barrier_arrive(acc_empty(j % 2), epi + 1);
            vx_barrier_wait(acc_empty(j % 2), phase);
          }
        }
        int phase = vx_barrier_arrive(full_bar(s), stagers + 1);
        vx_barrier_wait(full_bar(s), phase);
        if (i == 0 && vx_thread_id() == 0) {
          pT[4 * tile + 0] = vx_core_id();
          pT[4 * tile + 1] = csr_read(VX_CSR_MCYCLE);
        }
      #ifndef NO_MMA
        const uint32_t a_off = base + s * stage_bytes;
        const uint32_t w_off = a_off + stage_a;
        #pragma unroll
        for (uint32_t k = 0; k < KSTEPS; ++k) {
          const uint32_t w_k = (LAYOUT == LAYOUT_NK32) ? (k / 2) * TILE_N * kRowBytes + (k % 2) * kStepRow
                                                       : k * TILE_N * kStepRow;
          tm::mma(tm::make_idesc<vt::ITYPE>(kNFrag, d_col, i == 0 && k == 0),
                  tm::smem_desc(a_off + k * M * kStepRow, kStepRow), tm::smem_desc(w_off + w_k, kRowBytes));
        }
      #else
        (void)base; (void)d_col;
      #endif
        tm::commit(empty_bar(s), stagers + 1);
        if (++i == num_stages) {
          i = 0;
          tm::commit(acc_full, epi + 1);
          tile += gridDim.x;
          ++j;
        }
      }
    }
#endif
  } else if (warp_rank == 1 && CONSUME_WARPS == 0 && LEAN_ISSUE == 0) {
    // MMA issuer: one tc.mma per K step into the tile's accumulator buffer,
    // which the epilogue must have drained two tiles ago.
    const uint32_t base = tm::smem_offset(stages);
    uint32_t g = 0, j = 0;
    for (uint32_t tile = cta; tile < total_tiles; tile += gridDim.x, ++j) {
      const uint32_t d_col = taddr + (j % 2) * TILE_N;
      if (j >= 2) {
        int phase = vx_barrier_arrive(acc_empty(j % 2), epi + 1);
        vx_barrier_wait(acc_empty(j % 2), phase);
      }
      for (uint32_t i = 0; i < num_stages; ++i, ++g) {
        uint32_t s = g % PIPE;
        int phase = vx_barrier_arrive(full_bar(s), stagers + 1);
        vx_barrier_wait(full_bar(s), phase);
        if (i == 0 && vx_thread_id() == 0) {
          pT[4 * tile + 0] = vx_core_id();
          pT[4 * tile + 1] = csr_read(VX_CSR_MCYCLE);
        }
        const uint32_t a_off = base + s * stage_bytes;
        const uint32_t w_off = a_off + stage_a;
      #ifndef NO_MMA
        #pragma unroll
        for (uint32_t k = 0; k < KSTEPS; ++k) {
          const uint32_t w_k = (LAYOUT == LAYOUT_NK32) ? (k / 2) * TILE_N * kRowBytes + (k % 2) * kStepRow
                                                       : k * TILE_N * kStepRow;
          tm::mma(tm::make_idesc<vt::ITYPE>(kNFrag, d_col, i == 0 && k == 0),
                  tm::smem_desc(a_off + k * M * kStepRow, kStepRow), tm::smem_desc(w_off + w_k, kRowBytes));
        }
      #else
        (void)a_off; (void)w_off; (void)d_col;
      #endif
        tm::commit(empty_bar(s), stagers + 1);
      }
      tm::commit(acc_full, epi + 1);
    }
  } else if (CONSUME_WARPS == 0 && warp_rank >= EPI_WARP0 && warp_rank < EPI_WARP0 + epi) {
    // Epilogue: this warp's block's lanes of the tile's buffer, rows below M
    // only (C has rows up to the block's last lane).
    const uint32_t lane = tm::warp_lane();
    uint32_t j = 0;
    for (uint32_t tile = cta; tile < total_tiles; tile += gridDim.x, ++j) {
      vx_barrier(acc_full, epi + 1);
      if (warp_rank == EPI_WARP0 && vx_thread_id() == 0) {
        pT[4 * tile + 2] = csr_read(VX_CSR_MCYCLE);
      }
      auto pC = reinterpret_cast<ctx::output_t*>(arg->C_addr) + size_t(lane) * N + (tile % num_tiles) * TILE_N;
      for (uint32_t c = 0; c < TILE_N; c += ctx::xtileN) {
        ctx::fragment_acc frag;
        tm::load<tm::FRAGMENT>(frag, tm::make_taddr(lane, taddr + (j % 2) * TILE_N + c));
        ctx::store_matrix_sync(pC + c, frag, N);
      }
      vx_barrier_arrive(acc_empty(j % 2), epi + 1);
      if (warp_rank == EPI_WARP0 && vx_thread_id() == 0) {
        pT[4 * tile + 3] = csr_read(VX_CSR_MCYCLE);
      }
    }
  }

  vx_barrier(sync_bar, num_warps);
  if (warp_rank == 0) {
    tm::dealloc(taddr, 2 * TILE_N);
    if (vx_thread_id() == 0) {
      pCta[1] = csr_read(VX_CSR_MCYCLE);
    }
  }
}
