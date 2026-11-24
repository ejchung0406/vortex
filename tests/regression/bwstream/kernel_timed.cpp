#include <vx_spawn2.h>

#pragma push_macro("__kernel")
#undef __kernel
#define __kernel static inline __attribute__((always_inline))
#define kernel_main stream
#include "kernel.cpp"
#undef kernel_main
#pragma pop_macro("__kernel")

// A separate entry in a binary of its own (kernel_timed.vxbin), so the untimed
// run's kernel.vxbin, and with it everything laid out after it in device
// memory, is the same with or without it.
__kernel void kernel_timed(timed_arg_t* __UNIFORM__ arg) {
  const uint64_t c0 = vx_rdcycle();
  stream(&arg->stream);
  if (threadIdx.x == 0) {
    auto tm = reinterpret_cast<uint64_t*>(arg->timing_addr) + 3 * blockIdx.x;
    tm[0] = vx_core_id();
    tm[1] = c0;
    tm[2] = vx_rdcycle();
  }
}
