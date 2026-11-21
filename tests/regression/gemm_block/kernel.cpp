#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_intrinsics.h>
#include <vx_dxa.h>
#include <vx_barrier.h>

// C = A B for a small batch: A is M x K (the batch), B is K x N (the weights).
// A CTA of several warps owns warps * tileN columns of C and walks K once in
// chunks of chunk_k. It stages each chunk of its B columns into shared memory
// as [column][k], the layout B has in global memory, and every warp multiplies
// its tileN columns out of shared memory.
//   STAGE_LOAD: the CTA's threads copy the chunk a word each, thread t taking
//     words t, t + T, ... so that a warp reads one contiguous run of a column
//     and the CTA's warps read adjacent runs.
//   STAGE_DXA: the first warp has the DXA copy the chunk. `depth` chunk
//     buffers form a ring, each with its own barrier, and depth - 1 copies are
//     issued ahead.
// The core a CTA lands on decides how many A tiles it multiplies each chunk
// with: the first BW_TILES on a core below bw_cores, all MTILES on any other.
// CTA b stages the chunks of the columns of B of CTA b % wrap, where each kind
// has its own wrap: B holds as many columns as the larger one covers, and a
// small wrap makes the CTAs of that kind read the same few columns again and
// again. A CTA stores only the tiles it computed, into its own columns of C,
// and records its core, so the host knows which rows of its columns to expect
// and from which columns of B.
// With idle_role set, the CTAs of that role do no work: the other role runs
// alone on its cores.
namespace vt = vortex::tensor;
using ctx = vt::wmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, 8, 0, vt::ldst_matrix_enabled>;

constexpr uint32_t kDescB = 0;

typedef uint64_t __attribute__((may_alias)) word_t;

// the core is the same for every thread of a CTA
static inline __attribute__((always_inline)) uint32_t cta_core_id() {
  uint32_t __UNIFORM__ core = vx_core_id();
  return core;
}

// Tells the host where and when the CTA ran. A call, made last, so that what
// it needs is not held in registers over the K walk.
static __attribute__((noinline)) void record_cta(kernel_arg_t* __UNIFORM__ arg, uint32_t begin) {
  reinterpret_cast<uint32_t*>(arg->core_addr)[blockIdx.x] = cta_core_id();
#ifdef PROFILE_ENABLE
  const uint32_t warp = get_sub_group_id();
  reinterpret_cast<uint32_t*>(arg->begin_addr)[blockIdx.x * get_num_sub_groups() + warp] = begin;
#else
  (void)begin;
#endif
}

// A CTA of the idle role. It holds its slot until the grid's last CTA has
// recorded its core: were it to return at once, its core would take the whole
// grid from the cores that work. The host sizes the grid so that the last CTA
// goes to a working core.
static __attribute__((noinline)) void idle_cta(kernel_arg_t* __UNIFORM__ arg) {
  auto last = reinterpret_cast<uint32_t*>(arg->core_addr) + arg->last_cta;
  for (;;) {
    // A core's data cache does not see another core's store, so the record
    // is read with an atomic, which the cache passes on to the shared L2. A
    // fence would empty the cache instead, and its flush token holds every
    // core's requests at the L2, which disturbs the role being measured.
    long __UNIFORM__ record;
    __asm__ volatile("amoor.w %0, zero, (%1)" : "=r"(record) : "r"(last) : "memory");
    if (record != -1)
      break;
    // poll seldom: the idle core is to stay out of the L2
    for (uint32_t i = 0; i < 4096; ++i) {
      __asm__ volatile("nop");
    }
  }
#ifdef PROFILE_ENABLE
  reinterpret_cast<uint32_t*>(arg->cycles_addr)[blockIdx.x * get_num_sub_groups() + get_sub_group_id()] = 0;
#endif
  record_cta(arg, 0);
}

// The K walk of a CTA that multiplies each chunk with TILES tiles of A. The
// count is fixed at compile time so that each role has a walk of its own: the
// accumulators of TILES tiles are all the walk keeps, and a walk of one tile
// keeps them in registers.
template <uint32_t TILES>
static inline __attribute__((always_inline)) void walk(kernel_arg_t* __UNIFORM__ arg, bool bw_role) {
  auto pA = reinterpret_cast<ctx::input_t *>(arg->A_addr);
  auto pB = reinterpret_cast<ctx::input_t *>(arg->B_addr);
  auto pC = reinterpret_cast<ctx::output_t *>(arg->C_addr);

  size_t N = arg->N;
  size_t K = arg->K;
  uint32_t chunk = arg->chunk_k;
  uint32_t stage = arg->stage;
  uint32_t depth = arg->depth;

  const uint32_t warp = get_sub_group_id();
  const uint32_t num_warps = get_num_sub_groups();
  const uint32_t cta_cols = num_warps * ctx::tileN;
  const size_t cta_col = (size_t)blockIdx.x * cta_cols;
  const size_t tile_col = cta_col + warp * ctx::tileN;
  // the first of the CTA's columns of B
  const size_t b_col = (size_t)(blockIdx.x % (bw_role ? arg->bw_wrap : arg->cp_wrap)) * cta_cols;

  auto smem = reinterpret_cast<ctx::input_t *>(__local_mem());
  const size_t buf_elems = (size_t)cta_cols * chunk;
  const uint32_t bar0 = get_local_group_id();

  ctx::fragment_a   fragA;
  ctx::fragment_b   fragB;
  ctx::fragment_acc fragC[TILES];

  vt::unroll_for<TILES>([&](auto m) {
    ctx::fill_fragment(fragC[decltype(m)::value], 0);
  });

  const uint32_t chunks = K / chunk;

  // Multiplies the chunk in buffer `buf` with the A tiles at pChunkA, between
  // the barrier that says it is staged and the one that says every warp has
  // read it. Inlined, as a call would move the walk's state to the lanes'
  // stacks, which are far apart in global memory.
  auto multiply_chunk = [&](const ctx::input_t *pChunkA, const ctx::input_t *buf, uint32_t bar) __attribute__((always_inline)) {
    vx_barrier(bar, num_warps);
    auto pChunkB = buf + (size_t)warp * ctx::tileN * chunk;
    for (uint32_t k = 0; k < chunk; k += ctx::tileK) {
      ctx::load_matrix_sync<vt::col_major>(fragB, pChunkB + k, chunk);
      vt::unroll_for<TILES>([&](auto m) {
        constexpr uint32_t mt = decltype(m)::value;
        auto pTileA = pChunkA + (size_t)mt * ctx::tileM * K + k;
        ctx::load_matrix_sync(fragA, pTileA, K);
        ctx::mma_sync(fragC[mt], fragA, fragB, fragC[mt]);
      });
    }
    vx_barrier(bar, num_warps);
  };

#ifdef PROFILE_ENABLE
  __rdcycle_time t0 = vx_rdcycle_sync_begin();
#endif
  if (stage == STAGE_DXA) {
    // Step s issues chunk s and multiplies chunk s - (depth - 1), so depth - 1
    // copies run ahead of the chunk being read. Chunk j lives in buffer
    // j % depth, guarded by barrier j % depth.
    uint32_t issue_slot = 0;
    uint32_t slot = 0;
    for (uint32_t s = 0; s < chunks + depth - 1; ++s) {
      if (s < chunks) {
        if (warp == 0) {
          const uint32_t bar = (issue_slot << 8) + bar0;
          vx_barrier_expect_tx(bar, 1);
          vx_dxa_issue_2d_wg(kDescB, bar, smem + issue_slot * buf_elems, s * chunk, b_col);
        }
        issue_slot = (issue_slot + 1 == depth) ? 0 : issue_slot + 1;
      }
      if (s + 1 < depth)
        continue;
      multiply_chunk(pA, smem + slot * buf_elems, (slot << 8) + bar0);
      pA += chunk;
      slot = (slot + 1 == depth) ? 0 : slot + 1;
    }
  } else {
    // The copy walks the chunk's words in [column][k] order. A column's words
    // are contiguous in B and the columns are K apart, so a step of T words
    // in the chunk is a fixed step in B plus the rest of a column of B
    // whenever the walk wraps past the end of a column's chunk. The host
    // makes the chunk a whole number of words per thread.
    const uint32_t T = blockDim.x;
    const uint32_t t = threadIdx.x;
    const uint32_t col_words = chunk * sizeof(ctx::input_t) / sizeof(word_t);
    const uint32_t buf_words = cta_cols * col_words;
    const size_t col_stride = K * sizeof(ctx::input_t) / sizeof(word_t);
    const size_t src_step = (T / col_words) * col_stride + (T % col_words);
    const size_t src_wrap = col_stride - col_words;
    const uint32_t off_step = T % col_words;
    const uint32_t first_off = t % col_words;
    auto dst = reinterpret_cast<word_t *>(smem) + t;
    auto first = reinterpret_cast<const word_t *>(pB + b_col * K)
               + (t / col_words) * col_stride + first_off;
    for (uint32_t i = 0; i < chunks; ++i) {
      auto src = first;
      uint32_t off = first_off;
      for (uint32_t w = 0; w < buf_words; w += T) {
        dst[w] = *src;
        src += src_step;
        off += off_step;
        if (off >= col_words) {
          off -= col_words;
          src += src_wrap;
        }
      }
      first += col_words;
      multiply_chunk(pA, smem, bar0);
      pA += chunk;
    }
  }

#ifdef PROFILE_ENABLE
  // the K walk of this warp, without the launch and the host's uploads
  __rdcycle_time t1 = vx_rdcycle_sync_end();
  reinterpret_cast<uint32_t*>(arg->cycles_addr)[blockIdx.x * num_warps + warp] = vx_rdcycle_sync_diff(t0, t1);
#endif

  vt::unroll_for<TILES>([&](auto m) {
    constexpr uint32_t mt = decltype(m)::value;
    auto pTileC = pC + (size_t)mt * ctx::tileM * N + tile_col;
    ctx::store_matrix_sync(pTileC, fragC[mt], N);
  });

#ifdef PROFILE_ENABLE
  record_cta(arg, t0.lo);
#else
  record_cta(arg, 0);
#endif
}

// The compute-bound role's walk is a function of its own, so that its many
// accumulators do not take registers from the memory-bound role's walk, which
// stays in the kernel's entry as it is when both roles have the same tiles.
#if BW_TILES != MTILES
static __attribute__((noinline)) void walk_cp(kernel_arg_t* __UNIFORM__ arg) {
  walk<MTILES>(arg, false);
}
#endif

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const bool bw_role = cta_core_id() < arg->bw_cores;
  if (arg->idle_role == (bw_role ? ROLE_BW : ROLE_CP)) {
    idle_cta(arg);
    return;
  }
#if BW_TILES == MTILES
  walk<MTILES>(arg, bw_role);
#else
  if (bw_role) {
    walk<BW_TILES>(arg, true);
  } else {
    walk_cp(arg);
  }
#endif
}
