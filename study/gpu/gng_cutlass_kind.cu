// One CUTLASS GEMM kind per translation unit: nvcc -DGNG_KIND=bf16|bf16m1|fp8|fp8m1
#include "gng_cutlass.cuh"
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define DEFOF_bf16 DefBF16
#define DEFOF_bf16m1 DefBF16m1
#define DEFOF_fp8 DefFP8
#define DEFOF_fp8m1 DefFP8m1
namespace gngc {
Runner* CAT(make_runner_, GNG_KIND)() { return new RunnerT<CAT(DEFOF_, GNG_KIND)>(); }
}
