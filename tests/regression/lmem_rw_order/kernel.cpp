#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// Loads and stores of one warp to the same shared-memory words, in the order
// the program gives them, and across warps through a barrier: each load must
// see the last store before it, whichever engine of the local memory unit
// carries either. Every access is volatile, so each is an instruction.
__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t T  = vx_num_threads();
  const uint32_t t  = threadIdx.x % T;
  const uint32_t w  = threadIdx.x / T;
  const uint32_t nw = blockDim.x / T;
  auto out = reinterpret_cast<uint32_t*>(arg->out_addr) + threadIdx.x * RW_NRES;
  auto reg = reinterpret_cast<uint8_t*>(__local_mem()) + w * RW_REGION;
  auto A = reinterpret_cast<volatile uint32_t*>(reg + 4 * t);
  auto B = reinterpret_cast<volatile uint32_t*>(reg + 256 + 4 * t);
  auto C = reinterpret_cast<volatile uint32_t*>(reg + 512 + 4 * t);
  auto D = reinterpret_cast<volatile uint32_t*>(reg + 768 + 4 * t);
  const uint32_t id = (w << 8) | t;
  uint32_t r[RW_NRES];

  // a load after a store of its word; a store after a load, then a load
  *A = 0x11000000u | id;
  r[0] = *A;
  *A = 0x22000000u | id;
  r[1] = *A;
  *A = r[1] + 7;
  r[2] = *A;
  // a byte of the word, then the word
  *reinterpret_cast<volatile uint8_t*>(A) = 0x5a;
  r[3] = *A;

  // an atomic between plain accesses of its word (the same accesses plain
  // where the device has no A extension)
  *B = 10 + id;
  if (arg->atomic) {
    __asm__ volatile ("amoadd.w %0, %2, (%1)" : "=&r"(r[5]) : "r"(B), "r"(5) : "memory");
  } else {
    r[5] = *B;
    *B = r[5] + 5;
  }
  r[6] = *B;
  *B = r[6] + 1;
  if (arg->atomic) {
    __asm__ volatile ("amoadd.w %0, %2, (%1)" : "=&r"(r[7]) : "r"(B), "r"(1) : "memory");
  } else {
    r[7] = *B;
    *B = r[7] + 1;
  }

  // some lanes only
  r[8] = 0;
  if ((t % 3) == 1) {
    *C = 0x33000000u | id;
    r[8] = *C;
    *C = r[8] ^ 0xff;
    r[8] += *C;
  }

  // another thread's half of the bank word, and another warp's word, after a barrier
  *D = 0x44000000u | id;
  __syncthreads();
  r[4] = *reinterpret_cast<volatile uint32_t*>(reg + 4 * (t ^ 1));
  auto next = reinterpret_cast<uint8_t*>(__local_mem()) + ((w + 1) % nw) * RW_REGION;
  r[9] = *reinterpret_cast<volatile uint32_t*>(next + 768 + 4 * t);

  for (uint32_t i = 0; i < RW_NRES; ++i) {
    out[i] = r[i];
  }
}
