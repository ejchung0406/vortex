#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// A plain store between another warp's load-reserved and store-conditional on
// the same shared word. Warp 0 reserves one word per thread; the writer warp
// stores to the words of its even threads; warp 0's store-conditional must
// then fail on the even words and leave the writer's value, and succeed on
// the odd ones.
__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t T = vx_num_threads();
  const uint32_t t = threadIdx.x % T;
  const uint32_t w = threadIdx.x / T;
  auto out = reinterpret_cast<uint32_t*>(arg->out_addr);
  // one bank word per thread, the same for every warp
  auto word = reinterpret_cast<uint32_t*>(__local_mem()) + 2 * t;
  uint32_t old = 0, fail = 0xffffffffu;

  if (w == 0) {
    *reinterpret_cast<volatile uint32_t*>(word) = 0x1000 + t;
  }
  __syncthreads();
#ifdef __riscv_atomic
  if (w == 0) {
    __asm__ volatile ("lr.w %0, (%1)" : "=r"(old) : "r"(word) : "memory");
  }
#endif
  __syncthreads();
  if (w == arg->writer && (t & 1) == 0) {
    *reinterpret_cast<volatile uint32_t*>(word) = 0x2000 + t;
  }
  __syncthreads();
#ifdef __riscv_atomic
  if (w == 0) {
    __asm__ volatile ("sc.w %0, %2, (%1)" : "=&r"(fail) : "r"(word), "r"(0x3000 + t) : "memory");
  }
#endif
  __syncthreads();
  if (w == 0) {
    out[t] = fail;
    out[T + t] = *reinterpret_cast<volatile uint32_t*>(word);
    out[2 * T + t] = old;
  }
}
