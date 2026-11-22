#include "stream.h"

// A separate entry, so the untimed kernel carries no timing code; in a file of
// its own, so it does not move kernel_main's helpers in the image.
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
