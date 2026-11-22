// CUTLASS 3.x (repo at /storage/project/r-hkim358-0/echung67/cutlass, v4.5)
// Hopper TMA + wgmma warp-specialised persistent cooperative GEMMs for the
// DiT shapes, D[M x N] = A[M x K] * B[K x N], A row-major (K-major), B
// column-major (K-major), D BF16 row-major, FP32 accumulation.
//   BF16: tile 128x256x64, cluster 2x1x1
//   FP8 (e4m3): tile 128x256x128, cluster 2x1x1, FP8 fast accumulation
// Stages: CUTLASS's automatic count (fills shared memory) and that count
// minus one (the "carve-out" variant that leaves room for a lane CTA).
// Every launch goes through k_cutlass_smid<GemmKernel>, a copy of CUTLASS's
// device_kernel that first writes each CTA's %smid to a buffer, so that the
// GEMM's SM placement can be read back while it co-runs.
#pragma once
#include "gng_cutlass_api.h"
#include <cstdlib>
#include "cutlass/cutlass.h"
#include "cute/tensor.hpp"
#include "cutlass/gemm/device/gemm_universal_adapter.h"
#include "cutlass/gemm/kernel/gemm_universal.hpp"
#include "cutlass/gemm/collective/collective_builder.hpp"
#include "cutlass/epilogue/collective/collective_builder.hpp"
#include "cutlass/util/packed_stride.hpp"
#include "cutlass/cluster_launch.hpp"

namespace gngc {
using namespace cute;

template <class GemmKernel>
__global__ void __launch_bounds__(GemmKernel::MaxThreadsPerBlock, GemmKernel::MinBlocksPerMultiprocessor)
k_cutlass_smid(CUTLASS_GRID_CONSTANT typename GemmKernel::Params const params, int* smid_out) {
  if (smid_out && threadIdx.x == 0) {
    uint32_t s; asm volatile("mov.u32 %0, %%smid;" : "=r"(s));
    smid_out[blockIdx.x + gridDim.x * (blockIdx.y + gridDim.y * blockIdx.z)] = (int)s;
  }
  extern __shared__ char smem[];
  GemmKernel op;
  op(params, smem);
}

template <class ElemAB, class TileShape, class KernelSchedule, int StagesMinus>
struct Def {
  using ElementA = ElemAB; using LayoutA = cutlass::layout::RowMajor;
  using ElementB = ElemAB; using LayoutB = cutlass::layout::ColumnMajor;
  using ElementC = cutlass::bfloat16_t; using LayoutC = cutlass::layout::RowMajor;
  static constexpr int AlignAB = 16 / sizeof(ElemAB), AlignC = 8;
  using ClusterShape = Shape<_2, _1, _1>;
  using Epi = typename cutlass::epilogue::collective::CollectiveBuilder<
      cutlass::arch::Sm90, cutlass::arch::OpClassTensorOp, TileShape, ClusterShape,
      cutlass::epilogue::collective::EpilogueTileAuto, float, float,
      ElementC, LayoutC, AlignC, ElementC, LayoutC, AlignC,
      cutlass::epilogue::TmaWarpSpecializedCooperative>::CollectiveOp;
  using AutoMain = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm90, cutlass::arch::OpClassTensorOp, ElementA, LayoutA, AlignAB, ElementB, LayoutB, AlignAB, float,
      TileShape, ClusterShape,
      cutlass::gemm::collective::StageCountAutoCarveout<static_cast<int>(sizeof(typename Epi::SharedStorage))>,
      KernelSchedule>::CollectiveOp;
  static constexpr int AutoStages = AutoMain::DispatchPolicy::Stages;
  static constexpr int Stages = AutoStages - StagesMinus;
  using Main = typename cutlass::gemm::collective::CollectiveBuilder<
      cutlass::arch::Sm90, cutlass::arch::OpClassTensorOp, ElementA, LayoutA, AlignAB, ElementB, LayoutB, AlignAB, float,
      TileShape, ClusterShape, cutlass::gemm::collective::StageCount<Stages>, KernelSchedule>::CollectiveOp;
  using GemmKernel = cutlass::gemm::kernel::GemmUniversal<Shape<int, int, int, int>, Main, Epi>;
  using Gemm = cutlass::gemm::device::GemmUniversalAdapter<GemmKernel>;
};

using BF16Tile = Shape<_128, _256, _64>;
using FP8Tile = Shape<_128, _256, _128>;
using DefBF16 = Def<cutlass::bfloat16_t, BF16Tile, cutlass::gemm::KernelTmaWarpSpecializedCooperative, 0>;
using DefBF16m1 = Def<cutlass::bfloat16_t, BF16Tile, cutlass::gemm::KernelTmaWarpSpecializedCooperative, 1>;
using DefFP8 = Def<cutlass::float_e4m3_t, FP8Tile, cutlass::gemm::KernelTmaWarpSpecializedCooperativeFP8FastAccum, 0>;
using DefFP8m1 = Def<cutlass::float_e4m3_t, FP8Tile, cutlass::gemm::KernelTmaWarpSpecializedCooperativeFP8FastAccum, 1>;

template <class D>
struct RunnerT : Runner {
  using Gemm = typename D::Gemm; using GK = typename D::GemmKernel;
  Gemm op; void* ws = nullptr; dim3 g, b, c; int sm = 0, nregs = 0;
  bool init(const void* A, const void* B, void* Dp, int M, int N, int K, int sm_count) override {
    using SA = typename GK::StrideA; using SB = typename GK::StrideB; using SC = typename GK::StrideC; using SD = typename GK::StrideD;
    SA sa = cutlass::make_cute_packed_stride(SA{}, cute::make_shape(M, K, 1));
    SB sb = cutlass::make_cute_packed_stride(SB{}, cute::make_shape(N, K, 1));
    SC sc = cutlass::make_cute_packed_stride(SC{}, cute::make_shape(M, N, 1));
    SD sd = cutlass::make_cute_packed_stride(SD{}, cute::make_shape(M, N, 1));
    cutlass::KernelHardwareInfo hw; hw.device_id = 0; hw.sm_count = sm_count;
    typename Gemm::Arguments args{cutlass::gemm::GemmUniversalMode::kGemm, {M, N, K, 1},
                                  {(typename D::ElementA const*)A, sa, (typename D::ElementB const*)B, sb},
                                  {{1.0f, 0.0f}, (typename D::ElementC const*)Dp, sc, (typename D::ElementC*)Dp, sd}, hw};
    if (op.can_implement(args) != cutlass::Status::kSuccess) { fprintf(stderr, "cutlass: cannot implement\n"); return false; }
    size_t wsz = Gemm::get_workspace_size(args);
    if (ws) cudaFree(ws);
    ws = nullptr;
    if (wsz) cudaMalloc(&ws, wsz);
    if (op.initialize(args, ws) != cutlass::Status::kSuccess) { fprintf(stderr, "cutlass: initialize failed\n"); return false; }
    g = GK::get_grid_shape(op.params()); b = GK::get_block_shape();
    c = dim3(cute::size<0>(typename D::ClusterShape{}), cute::size<1>(typename D::ClusterShape{}), cute::size<2>(typename D::ClusterShape{}));
    sm = GK::SharedStorageSize;
    cudaFuncSetAttribute(k_cutlass_smid<GK>, cudaFuncAttributeMaxDynamicSharedMemorySize, sm);
    // prefer the largest shared-memory carveout (228 KB): with the default the driver picks the smallest
    // configuration that holds the GEMM CTA and a 33 KB lane CTA no longer fits beside it
    if (getenv("GNG_CUTLASS_CARVEOUT")) cudaFuncSetAttribute(k_cutlass_smid<GK>, cudaFuncAttributePreferredSharedMemoryCarveout, atoi(getenv("GNG_CUTLASS_CARVEOUT")));
    cudaFuncAttributes fa; cudaFuncGetAttributes(&fa, k_cutlass_smid<GK>); nregs = fa.numRegs;
    return true;
  }
  bool launch(cudaStream_t st, int* smid_out) override {
    void* kp[] = {(void*)&op.params(), (void*)&smid_out};
    return cutlass::ClusterLauncher::launch(g, c, b, sm, st, (void const*)k_cutlass_smid<GK>, kp) == cutlass::Status::kSuccess;
  }
  int grid() const override { return g.x * g.y * g.z; }
  int threads() const override { return b.x * b.y * b.z; }
  int smem() const override { return sm; }
  int stages() const override { return D::Stages; }
  int regs() const override { return nregs; }
};
}  // namespace gngc
