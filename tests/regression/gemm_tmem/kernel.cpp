#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_tmem.h>
#include <vx_intrinsics.h>
#include <vx_dxa.h>
#include <vx_barrier.h>

// C = A B with the tensor-memory engine. A tile of C is MTILES * 64 rows by
// TILE_N columns, its accumulators in TMEM; the CTAs are persistent, CTA c
// computing tiles c, c + grid, ... (N tile fastest, so the CTAs running at
// once share their A rows). Warp 0 has the DXA stage each slab of A and B into
// a ring of PIPE stage buffers, running on into the next tile while the last
// one drains; warp 1 issues the multiplies, every M tile against the one
// staged B slab; the EPI_WARPS warps from rank 4 drain the accumulators,
// warp r reading its TCU block's lanes. A and B are packed by K step in global
// memory, so a stage's slab of an M tile, and of the B tile, is one
// contiguous run, and the engine reads each K step's rows 32 bytes apart.
// PLAIN_LAYOUT: A row-major and B as W[N][K] instead, staged in the same
// order by 3D transfers that read each 64-byte line twice.
namespace vt = vortex::tensor;
namespace tm = vortex::tmem;
using ctx = vt::wgmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, GEMM_NRC>;
using wg_cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, GEMM_NRC>;

constexpr uint32_t kDescA = 0;
constexpr uint32_t kDescB = 1;

constexpr uint32_t kBlocks   = VX_CFG_NUM_TCU_BLOCKS;
constexpr uint32_t kTileM    = kBlocks * wg_cfg::xtileM;   // rows of one tc.mma
constexpr uint32_t kStepK    = wg_cfg::fedpK * wg_cfg::i_ratio;  // K elements of one tc.mma
constexpr uint32_t kStepRow  = wg_cfg::fedpK * 4;          // bytes of one row's K step
constexpr uint32_t kABlock   = kTileM * kStepRow;          // one M tile's K step
constexpr uint32_t kBBlock   = TILE_N * kStepRow;          // the B tile's K step
constexpr uint32_t kStageA   = MTILES * KSTEPS * kABlock;
constexpr uint32_t kStage    = kStageA + KSTEPS * kBBlock;
constexpr uint32_t kTmemCols = MTILES * TILE_N;
constexpr uint32_t kNFrag    = TILE_N / wg_cfg::tcN;
// Columns of each M tile an epilogue warp drains.
constexpr uint32_t kEpiCols  = TILE_N / (EPI_WARPS / kBlocks);

static_assert(kTileM == 64, "a tc.mma covers 64 rows");
static_assert(kNFrag <= 128, "TILE_N: at most 128 fragments");
static_assert(EPI_WARPS % kBlocks == 0 && kEpiCols % ctx::xtileN == 0, "EPI_WARPS: whole transfers per warp");
static_assert(kTmemCols <= VX_CFG_TCU_TMEM_COLS, "the accumulators exceed TMEM");
static_assert(2 * PIPE + 3 <= VX_CFG_NUM_BARRIERS, "not enough barriers for PIPE");
static_assert(sizeof(ctx::output_t) == sizeof(float), "the epilogue stores fp32 lanes");

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t warp_rank = threadIdx.x / VX_CFG_NUM_THREADS;
  const uint32_t N = arg->N;
  const uint32_t k_steps = arg->K / kStepK;
  const uint32_t num_stages = k_steps / KSTEPS;
  const uint32_t n_tiles = N / TILE_N;
  const uint32_t num_tiles = n_tiles * (arg->M / (MTILES * kTileM));
  const uint32_t cta = blockIdx.x;

  auto lmem = reinterpret_cast<uint8_t*>(__local_mem());
  auto tmem_slot = reinterpret_cast<volatile uint32_t*>(lmem);
  uint8_t* stages = lmem + TMEM_SLOT_BYTES;

  const uint32_t cta_id = get_local_group_id();
  auto full_bar  = [&](uint32_t s) { return cta_id | (s << 8); };
  auto empty_bar = [&](uint32_t s) { return cta_id | ((PIPE + s) << 8); };
  const uint32_t acc_full  = cta_id | ((2 * PIPE) << 8);      // the engine, and the epilogue warps
  const uint32_t acc_empty = cta_id | ((2 * PIPE + 1) << 8);  // the epilogue warps, and the MMA warp
  const uint32_t sync_bar  = cta_id | ((2 * PIPE + 2) << 8);

  auto pT = reinterpret_cast<uint64_t*>(arg->times_addr);
  auto pCta = pT + 4 * num_tiles + 2 * cta;
  if (warp_rank == 0) {
    uint32_t col = tm::alloc(kTmemCols);
    if (vx_thread_id() == 0) {
      *tmem_slot = col;
      pCta[0] = csr_read(VX_CSR_MCYCLE);
    }
  }
  vx_barrier(sync_bar, 4 + EPI_WARPS);
  const uint32_t taddr = *tmem_slot;

  if (warp_rank == 0) {
    // Producer: this warp and the MMA warp are the two participants of every
    // full and empty barrier.
    uint32_t g = 0;
    for (uint32_t tile = cta; tile < num_tiles; tile += gridDim.x) {
      const uint32_t mt = tile / n_tiles, nt = (tile % n_tiles) % arg->b_wrap;
      for (uint32_t i = 0; i < num_stages; ++i, ++g) {
        uint32_t s = g % PIPE;
        if (g >= PIPE) {
          int phase = vx_barrier_arrive(empty_bar(s), 2);
          vx_barrier_wait(empty_bar(s), phase);
        }
        uint8_t* buf = stages + s * kStage;
        vx_barrier_expect_tx(full_bar(s), MTILES + KSTEPS);
      #ifdef PLAIN_LAYOUT
        // A [M][K] and B as W[N][K]: a 3D tile (a K step's elements, rows, K
        // steps) lands in the same K-step-major order.
        for (uint32_t t = 0; t < MTILES; ++t) {
          vx_dxa_issue_3d_wg(kDescA, full_bar(s), buf + t * KSTEPS * kABlock, 0, (mt * MTILES + t) * kTileM, i * KSTEPS);
        }
        for (uint32_t j = 0; j < KSTEPS; ++j) {
          vx_dxa_issue_3d_wg(kDescB, full_bar(s), buf + kStageA + j * kBBlock, 0, nt * TILE_N, i * KSTEPS + j);
        }
      #else
        for (uint32_t t = 0; t < MTILES; ++t) {
          vx_dxa_issue_2d_wg(kDescA, full_bar(s), buf + t * KSTEPS * kABlock, 0, (mt * MTILES + t) * k_steps + i * KSTEPS);
        }
        for (uint32_t j = 0; j < KSTEPS; ++j) {
          vx_dxa_issue_2d_wg(kDescB, full_bar(s), buf + kStageA + j * kBBlock, 0, nt * k_steps + i * KSTEPS + j);
        }
      #endif
        vx_barrier_arrive(full_bar(s), 2);
      }
    }
    // Close the last generation of every empty barrier the MMA warp committed.
    for (uint32_t i = (g > PIPE) ? (g - PIPE) : 0; i < g; ++i) {
      int phase = vx_barrier_arrive(empty_bar(i % PIPE), 2);
      vx_barrier_wait(empty_bar(i % PIPE), phase);
    }
  } else if (warp_rank == 1) {
    // Per stage: wait for it, multiply every M tile with each K step of the
    // B slab, and commit the stage back to the producer. A tile's first
    // multiply waits until the epilogue has read the previous accumulators.
    const uint32_t base = tm::smem_offset(stages);
    uint32_t g = 0;
    for (uint32_t tile = cta; tile < num_tiles; tile += gridDim.x) {
      if (tile != cta) {
        int phase = vx_barrier_arrive(acc_empty, EPI_WARPS + 1);
        vx_barrier_wait(acc_empty, phase);
      }
      for (uint32_t i = 0; i < num_stages; ++i, ++g) {
        uint32_t s = g % PIPE;
        int phase = vx_barrier_arrive(full_bar(s), 2);
        vx_barrier_wait(full_bar(s), phase);
        if (i == 0 && vx_thread_id() == 0) {
          pT[4 * tile + 0] = vx_core_id();
          pT[4 * tile + 1] = csr_read(VX_CSR_MCYCLE);
        }
        const uint32_t a_off = base + s * kStage;
        const uint32_t b_off = a_off + kStageA;
        #pragma unroll
        for (uint32_t j = 0; j < KSTEPS; ++j) {
          const auto b = tm::smem_desc(b_off + j * kBBlock, kStepRow);
          #pragma unroll
          for (uint32_t t = 0; t < MTILES; ++t) {
            tm::mma(tm::make_idesc<vt::ITYPE>(kNFrag, taddr + t * TILE_N, i == 0 && j == 0),
                    tm::smem_desc(a_off + (t * KSTEPS + j) * kABlock, kStepRow), b);
          }
        }
        tm::commit(empty_bar(s), 2);
      }
      tm::commit(acc_full, EPI_WARPS + 1);
    }
  } else if (warp_rank >= 4) {
    // Epilogue warp e: its block's lanes, columns [part * kEpiCols, ...) of each M tile.
    const uint32_t e = warp_rank - 4;
    const uint32_t lane = tm::warp_lane();
    const uint32_t col0 = (e / kBlocks) * kEpiCols;
    for (uint32_t tile = cta; tile < num_tiles; tile += gridDim.x) {
      vx_barrier(acc_full, EPI_WARPS + 1);
      if (e == 0 && vx_thread_id() == 0) {
        pT[4 * tile + 2] = csr_read(VX_CSR_MCYCLE);
      }
      const uint32_t mt = tile / n_tiles, nt = tile % n_tiles;
      for (uint32_t t = 0; t < MTILES; ++t) {
        auto pC = reinterpret_cast<ctx::output_t*>(arg->C_addr)
                + size_t((mt * MTILES + t) * kTileM + lane) * N + nt * TILE_N;
        for (uint32_t c = col0; c < col0 + kEpiCols; c += ctx::xtileN) {
          ctx::fragment_acc frag;
          tm::load<tm::FRAGMENT>(frag, tm::make_taddr(lane, taddr + t * TILE_N + c));
          ctx::store_matrix_sync(pC + c, frag, N);
        }
      }
      // The loads are done: the next tile's multiplies may overwrite the columns.
      vx_barrier_arrive(acc_empty, EPI_WARPS + 1);
      if (e == 0 && vx_thread_id() == 0) {
        pT[4 * tile + 3] = csr_read(VX_CSR_MCYCLE);
      }
    }
  }

  vx_barrier(sync_bar, 4 + EPI_WARPS);
  if (warp_rank == 0) {
    tm::dealloc(taddr, kTmemCols);
    if (vx_thread_id() == 0) {
      pCta[1] = csr_read(VX_CSR_MCYCLE);
    }
  }
}
