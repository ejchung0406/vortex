#include "common.h"
#include <vx_spawn2.h>
#include <vx_tensor.h>
#include <vx_intrinsics.h>

// C = A B for a small batch: A is M x K (the batch), B is K x N (the weights).
// A CTA (one warp) owns tileN columns of C and walks K once. At each K step it
// reads its B tile once and multiplies it with each of the MTILES A tiles, so
// the weights are read once whatever the batch.
namespace vt = vortex::tensor;
using ctx = vt::wmma_context<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, false, 8, 0, vt::ldst_matrix_enabled>;

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto pA = reinterpret_cast<ctx::input_t *>(arg->A_addr);
  auto pB = reinterpret_cast<ctx::input_t *>(arg->B_addr);
  auto pC = reinterpret_cast<ctx::output_t *>(arg->C_addr);

  size_t N = arg->N;
  size_t K = arg->K;

  ctx::fragment_a   fragA;
  ctx::fragment_b   fragB;
  ctx::fragment_acc fragC[MTILES];

  size_t tile_col = blockIdx.x * ctx::tileN;

  vt::unroll_for<MTILES>([&](auto m) {
    ctx::fill_fragment(fragC[decltype(m)::value], 0);
  });

#ifdef PROFILE_ENABLE
  __rdcycle_time t0 = vx_rdcycle_sync_begin();
#endif
  for (size_t i = 0; i < K; i += ctx::tileK) {
    auto pTileB = pB + tile_col * K + i;
    ctx::load_matrix_sync<vt::col_major>(fragB, pTileB, K);
    vt::unroll_for<MTILES>([&](auto m) {
      constexpr uint32_t mt = decltype(m)::value;
      auto pTileA = pA + (size_t)mt * ctx::tileM * K + i;
      ctx::load_matrix_sync(fragA, pTileA, K);
      ctx::mma_sync(fragC[mt], fragA, fragB, fragC[mt]);
    });
  }

#ifdef PROFILE_ENABLE
  // the K walk of this block, without the launch and the host's uploads
  __rdcycle_time t1 = vx_rdcycle_sync_end();
  reinterpret_cast<uint32_t*>(arg->cycles_addr)[blockIdx.x] = vx_rdcycle_sync_diff(t0, t1);
#endif

  vt::unroll_for<MTILES>([&](auto m) {
    constexpr uint32_t mt = decltype(m)::value;
    auto pTileC = pC + (size_t)mt * ctx::tileM * N + tile_col;
    ctx::store_matrix_sync(pTileC, fragC[mt], N);
  });
}
