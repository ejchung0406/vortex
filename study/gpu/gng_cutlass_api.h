// Type-erased interface to the CUTLASS GEMMs of gng_cutlass.cuh, so that the
// experiment drivers do not include CUTLASS (each kind is compiled in its own
// translation unit, gng_cutlass_kind.cu -DGNG_KIND=<kind>).
#pragma once
#include <cuda_runtime.h>
#include <string>
namespace gngc {
// Type-erased runner
struct Runner {
  virtual ~Runner() {}
  virtual bool init(const void* A, const void* B, void* D, int M, int N, int K, int sm_count) = 0;
  virtual bool launch(cudaStream_t st, int* smid_out) = 0;
  virtual int grid() const = 0;
  virtual int threads() const = 0;
  virtual int smem() const = 0;
  virtual int stages() const = 0;
  virtual int regs() const = 0;
};

Runner* make_runner_bf16();
Runner* make_runner_bf16m1();
Runner* make_runner_fp8();
Runner* make_runner_fp8m1();
inline Runner* make_runner(const std::string& k) {
  if (k == "bf16") return make_runner_bf16();
  if (k == "bf16m1") return make_runner_bf16m1();
  if (k == "fp8") return make_runner_fp8();
  if (k == "fp8m1") return make_runner_fp8m1();
  return nullptr;
}
}  // namespace gngc
