#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_tmem.h>
#include <vx_dxa.h>
#include <vx_intrinsics.h>
#include <vx_barrier.h>

// The tensor engine's shared-memory row reads against everything else that
// uses the local-memory banks. Warp 0 issues the tensor ops; the other warps
// of the CTA write the rows being read (race) or load beside them (starve).

namespace vt = vortex::tensor;
namespace tm = vortex::tmem;

using ctx = vt::wgmma_context<VX_CFG_NUM_THREADS, vt::fp16, vt::fp32, false, 32>;
using wg_cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::fp16, vt::fp32, 32>;

constexpr uint32_t kNT = VX_CFG_NUM_THREADS;
constexpr uint32_t kStepBytes = wg_cfg::fedpK * 4;
constexpr uint32_t kRowBytes = VX_CFG_LMEM_NUM_BANKS * (VX_CFG_XLEN / 8);

struct frag8_t {
  static constexpr uint32_t NR = 8;
  float data[NR];
};
// columns one tmem.ld of eight registers covers
constexpr uint32_t kLdCols = frag8_t::NR * kNT / tm::block_lanes();

constexpr uint32_t kSyncBar = 0;
constexpr uint32_t kOpBar   = 1;
constexpr uint32_t kDxaBar  = 2;

static inline float enc(uint32_t gen, uint32_t idx) {
  union { uint32_t u; float f; } v;
  v.u = VAL_BASE | (gen << GEN_SHIFT) | idx;
  return v.f;
}

// Copy the region into TMEM `reps` times and bring the calling warp's lanes of
// the last copy out.
static inline __attribute__((always_inline))
void copy_out(float* out, uint32_t taddr, const float* region, uint32_t cols, uint32_t reps,
              uint32_t op_bar, uint32_t t) {
  for (uint32_t r = 0; r < reps; ++r) {
    tm::cp(taddr, region, cols);
  }
  tm::commit(op_bar, 2);
  vx_barrier(op_bar, 2);
  const uint32_t lane = tm::warp_lane();
  for (uint32_t c = 0; c < cols; c += kLdCols) {
    frag8_t frag;
    tm::load<tm::LANE_PER_THREAD>(frag, tm::make_taddr(lane, taddr + c));
    float* p = out + (c / kLdCols) * frag8_t::NR * kNT;
    for (uint32_t r = 0; r < frag8_t::NR; ++r) {
      p[r * kNT + t] = frag.data[r];
    }
  }
}

static inline __attribute__((always_inline)) void kernel_race(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t tid = threadIdx.x;
  const uint32_t num_threads = blockDim.x;
  const uint32_t t = tid % kNT;
  const uint32_t warp_rank = tid / kNT;
  const uint32_t num_warps = num_threads / kNT;
  const uint32_t cta_id = get_local_group_id();
  const uint32_t sync_bar = cta_id | (kSyncBar << 8);
  const uint32_t op_bar = cta_id | (kOpBar << 8);
  const uint32_t sub_words = arg->sub_words;
  const uint32_t words = NUM_SUBS * sub_words;
  const uint32_t cols = arg->cols;
  // lanes of the warp's block by the copy's columns
  const uint32_t copy_words = cols * tm::block_lanes();

  auto lmem = reinterpret_cast<uint8_t*>(__local_mem());
  auto tmem_slot = reinterpret_cast<volatile uint32_t*>(lmem);
  auto region = reinterpret_cast<float*>(lmem + SLOT_BYTES);
  auto out = reinterpret_cast<float*>(arg->out_addr);

  const uint32_t tmem_cols = tm::alloc_cols(cols);
  if (warp_rank == 0) {
    uint32_t col = tm::alloc(tmem_cols);
    if (vx_thread_id() == 0) {
      *tmem_slot = col;
    }
  }
  for (uint32_t i = tid; i < words; i += num_threads) {
    region[i] = enc(0, i);
  }
  vx_barrier(sync_bar, num_warps);
  const uint32_t taddr = *tmem_slot;

  // copy 0, before any writer starts, tells the host which word each lane
  // and column of a copy holds
  if (warp_rank == 0) {
    copy_out(out, taddr, region, cols, 1, op_bar, t);
  }
  vx_barrier(sync_bar, num_warps);

  if (warp_rank == 0) {
    for (uint32_t c = 1; c <= arg->copies; ++c) {
      copy_out(out + c * copy_words, taddr, region, cols, arg->reps, op_bar, t);
    }
  } else if (warp_rank == 1) {
    // one transfer at a time, so a row's generations land in order
    vortex::barrier dxa_bar(kDxaBar, 1);
    for (uint32_t g = 1; g <= arg->dxa_gens; ++g) {
      dxa_bar.expect_tx(1);
      vx_dxa_issue_1d_wg(0, dxa_bar.id(), region + SUB_DXA * sub_words, (g - 1) * sub_words);
      dxa_bar.arrive_and_wait();
    }
  } else if (warp_rank == 2) {
    float* sub = region + SUB_STORE * sub_words;
    for (uint32_t g = 1; g <= arg->store_gens; ++g) {
      for (uint32_t i = t; i < sub_words; i += kNT) {
        sub[i] = enc(g, SUB_STORE * sub_words + i);
      }
    }
  } else if (warp_rank == 3) {
#ifdef VX_CFG_EXT_A_ENABLE
    auto sub = reinterpret_cast<uint32_t*>(region + SUB_AMO * sub_words);
    for (uint32_t g = 1; g <= arg->amo_gens; ++g) {
      for (uint32_t i = t; i < sub_words; i += kNT) {
        __atomic_fetch_add(sub + i, 1u << GEN_SHIFT, __ATOMIC_SEQ_CST);
      }
    }
#endif
  }
  vx_barrier(sync_bar, num_warps);

  // the last copy is ordered behind every writer
  if (warp_rank == 0) {
    copy_out(out + (arg->copies + 1) * copy_words, taddr, region, cols, 1, op_bar, t);
  }
  vx_barrier(sync_bar, num_warps);
  if (warp_rank == 0) {
    tm::dealloc(taddr, tmem_cols);
  }
}

// One sweep of a warp's loads over its window, eight loads to a branch so
// that the warp is waiting on local memory most of the time.
static inline __attribute__((always_inline))
uint32_t sweep(const volatile uint32_t* base, uint32_t span) {
  uint32_t s = 0;
  for (uint32_t j = 0; j < span; j += 8) {
    s += base[j] + base[j + 1] + base[j + 2] + base[j + 3]
       + base[j + 4] + base[j + 5] + base[j + 6] + base[j + 7];
  }
  return s;
}

static inline __attribute__((always_inline)) void kernel_starve(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t tid = threadIdx.x;
  const uint32_t num_threads = blockDim.x;
  const uint32_t t = tid % kNT;
  const uint32_t warp_rank = tid / kNT;
  const uint32_t num_warps = num_threads / kNT;
  const uint32_t cta_id = get_local_group_id();
  const uint32_t sync_bar = cta_id | (kSyncBar << 8);
  const uint32_t op_bar = cta_id | (kOpBar << 8);
  const uint32_t N = arg->N;
  const uint32_t span = arg->span;
  const uint32_t use_cp = arg->use_cp;
  const uint32_t a_bytes = VX_CFG_NUM_TCU_BLOCKS * ctx::xtileM * kStepBytes;
  const uint32_t b_bytes = N * kStepBytes;
  // tc.cp reads N columns of every lane: a column group is every block's
  // M steps of a warp's words
  const uint32_t cp_bytes = (N / tm::frag_cols()) * VX_CFG_NUM_TCU_BLOCKS * wg_cfg::m_steps * kNT * 4;
  const uint32_t opnd_words = (use_cp ? cp_bytes : (a_bytes + b_bytes)) / 4;
  // a thread's window: the lanes side by side, or a bank row apart so that
  // they meet in one bank and take a pass each
  const uint32_t lane_words = arg->conflict ? (kRowBytes / 4) : 1;
  const uint32_t ld_words = kNT * lane_words + span;

  auto lmem = reinterpret_cast<uint8_t*>(__local_mem());
  auto tmem_slot = reinterpret_cast<volatile uint32_t*>(lmem);
  auto opnd = reinterpret_cast<uint32_t*>(lmem + SLOT_BYTES);
  auto ld = opnd + opnd_words;
  auto t_stat = reinterpret_cast<uint64_t*>(arg->stat_addr);
  auto l_stat = t_stat + ST_WORDS * warp_rank;

  const uint32_t tmem_cols = tm::alloc_cols(N);
  if (warp_rank == 0) {
    uint32_t col = tm::alloc(tmem_cols);
    if (vx_thread_id() == 0) {
      *tmem_slot = col;
    }
  }
  // zero operands: the ops only have to read them
  for (uint32_t i = tid; i < opnd_words + ld_words; i += num_threads) {
    opnd[i] = 0;
  }
  vx_barrier(sync_bar, num_warps);
  const uint32_t taddr = *tmem_slot;
  const uint32_t a_off = tm::smem_offset(opnd);
  const auto idesc = tm::make_idesc<vt::fp16>(N / ctx::tcN, taddr, false);
  const auto a_desc = tm::smem_desc(a_off, kStepBytes);
  const auto b_desc = tm::smem_desc(a_off + a_bytes, kStepBytes);
  const volatile uint32_t* win = ld + t * lane_words;
  uint32_t sum = 0;

  // The op queue is a few ops deep, so the issuing warp advances at the
  // engine's rate: the time to issue the ops is the time the engine takes,
  // and no barrier, which would wait for the other warps' loads, is in it.
  auto tensor_work = [&](uint32_t ops) -> uint64_t {
    for (uint32_t i = 0; i < ops; ++i) {
      if (use_cp) {
        tm::cp(taddr, opnd, N);
      } else {
        tm::mma(idesc, a_desc, b_desc);
      }
    }
    return vx_rdcycle();
  };
  auto tensor_done = [&]() {
    tm::commit(op_bar, 2);
    vx_barrier(op_bar, 2);
  };
  const bool hold = (arg->mode == MODE_HOLD);
  // As tensor_work, and the longest an op waited to be issued.
  auto tensor_work_worst = [&](uint32_t ops, uint64_t& worst) -> uint64_t {
    uint64_t prev = vx_rdcycle();
    worst = 0;
    for (uint32_t i = 0; i < ops; ++i) {
      if (use_cp) {
        tm::cp(taddr, opnd, N);
      } else {
        tm::mma(idesc, a_desc, b_desc);
      }
      uint64_t now = vx_rdcycle();
      worst = (now - prev > worst) ? (now - prev) : worst;
      prev = now;
    }
    return prev;
  };
  // The other warps' stream: loads, or what holds a row read up: atomics in
  // the warp's window (a zero is added, so the value stays), or stores of the
  // zeros already there over the operands' rows.
  auto load_work = [&](uint32_t sweeps) -> uint64_t {
    for (uint32_t s = 0; s < sweeps; ++s) {
      if (!hold) {
        sum += sweep(win, span);
      } else if (arg->stores) {
        for (uint32_t i = t; i < opnd_words; i += kNT) {
          opnd[i] = 0;
        }
      } else {
#ifdef VX_CFG_EXT_A_ENABLE
        auto w = const_cast<uint32_t*>(win);
        for (uint32_t j = 0; j < span; ++j) {
          __atomic_fetch_add(w + j, 0u, __ATOMIC_SEQ_CST);
        }
#endif
      }
    }
    return vx_rdcycle();
  };

  if (warp_rank == 0 && !use_cp) {
    tm::mma(tm::make_idesc<vt::fp16>(N / ctx::tcN, taddr, true), a_desc, b_desc);
    tensor_done();
  }
  vx_barrier(sync_bar, num_warps);

  // each side's fixed work alone
  if (warp_rank == 0) {
    uint64_t worst = 0;
    uint64_t c0 = vx_rdcycle();
    uint64_t c1 = hold ? tensor_work_worst(arg->ops, worst) : tensor_work(arg->ops);
    tensor_done();
    if (t == 0) {
      t_stat[ST_SOLO] = c1 - c0;
      t_stat[ST_WORST_SOLO] = worst;
    }
  }
  vx_barrier(sync_bar, num_warps);
  if (warp_rank != 0 && !hold) {
    uint64_t c0 = vx_rdcycle();
    uint64_t c1 = load_work(arg->sweeps);
    if (t == 0) {
      l_stat[ST_SOLO] = c1 - c0;
    }
  }
  vx_barrier(sync_bar, num_warps);

  // the fixed tensor work beside loads that outlast it
  if (warp_rank == 0) {
    uint64_t worst = 0;
    uint64_t c0 = vx_rdcycle();
    uint64_t c1 = hold ? tensor_work_worst(arg->ops, worst) : tensor_work(arg->ops);
    tensor_done();
    if (t == 0) {
      t_stat[ST_CONT] = c1 - c0;
      t_stat[ST_END_T] = c1;
      t_stat[ST_WORST_CONT] = worst;
    }
  } else {
    uint64_t c1 = load_work(arg->sweeps * arg->outlast);
    if (t == 0) {
      l_stat[ST_END_T] = c1;
    }
  }
  vx_barrier(sync_bar, num_warps);

  // the fixed loads beside tensor work that outlasts them
  if (hold) {
  } else if (warp_rank == 0) {
    uint64_t c1 = tensor_work(arg->ops * arg->outlast);
    tensor_done();
    if (t == 0) {
      t_stat[ST_END_L] = c1;
    }
  } else {
    uint64_t c0 = vx_rdcycle();
    uint64_t c1 = load_work(arg->sweeps);
    if (t == 0) {
      l_stat[ST_CONT] = c1 - c0;
      l_stat[ST_END_L] = c1;
    }
  }
  vx_barrier(sync_bar, num_warps);
  if (warp_rank == 0) {
    tm::dealloc(taddr, tmem_cols);
  } else if (t == 0) {
    reinterpret_cast<uint32_t*>(arg->out_addr)[warp_rank] = sum;
  }
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  if (arg->mode == MODE_RACE) {
    kernel_race(arg);
  } else {
    kernel_starve(arg);
  }
}
