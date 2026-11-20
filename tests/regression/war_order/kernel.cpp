#include <vx_spawn2.h>
#include "common.h"

// Every thread owns one word per iteration and, in program order, issues to it
//
//   load, load - the line is cold: they head or join the line's chain
//   delay - swept per warp and iteration, so the store below lands at varying
//           points of the chain's fill and drain
//   store - younger than those loads (write after read)
//   load  - must see the store (read after write)
//   store, store - a pair (write after write)
//   load  - must see the second store of the pair
//   delay - swept per warp and iteration
//   load  - must still see it
//
// With layout 0 the words of an iteration are laid out lane-major, so the
// threads that share a cache line, and its words, belong to different warps:
// their chains, the fill's forward drain and the replays interleave with the
// stores of the other warps. With layout 1 a warp's words are contiguous, so
// its few requests per line follow each other closely through the bank. The
// sequence is inline assembly, so the compiler cannot reorder it.
__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto data = reinterpret_cast<uint32_t*>(arg->data_addr);
  auto errs = reinterpret_cast<uint32_t*>(arg->err_addr);
  const uint32_t T    = arg->num_threads;
  const uint32_t W    = arg->warp_size;
  const uint32_t tid  = blockIdx.x * blockDim.x + threadIdx.x;
  const uint32_t lane = tid % W;
  const uint32_t warp = tid / W;
  const uint32_t slot = arg->layout ? tid : (lane * arg->num_warps + warp);
  uint32_t k = 0;
  for (uint32_t base = 0; base + T <= arg->num_words; base += T, ++k) {
    uint32_t i = base + slot;
    uint32_t* p = data + i;
    uint32_t m1 = WAR_MARK1 + i, m2 = WAR_MARK2 + i, m3 = WAR_MARK3 + i;
    uint32_t delay = ((k + warp) % arg->delay_sweep) * arg->delay_step;
    uint32_t sdelay = ((3 * k + warp) % arg->store_sweep) * arg->store_step;
    uint32_t a, b, v1, v2, v3;
    __asm__ volatile(
      "lw   %[a], 0(%[p])\n\t"
      "lw   %[b], 0(%[p])\n\t"
      "beqz %[sn], 4f\n\t"
      "3:\n\t"
      "addi %[sn], %[sn], -1\n\t"
      "bnez %[sn], 3b\n\t"
      "4:\n\t"
      "sw   %[m1], 0(%[p])\n\t"
      "lw   %[v1], 0(%[p])\n\t"
      "sw   %[m2], 0(%[p])\n\t"
      "sw   %[m3], 0(%[p])\n\t"
      "lw   %[v2], 0(%[p])\n\t"
      "beqz %[n], 2f\n\t"
      "1:\n\t"
      "addi %[n], %[n], -1\n\t"
      "bnez %[n], 1b\n\t"
      "2:\n\t"
      "lw   %[v3], 0(%[p])\n\t"
      : [a] "=&r"(a), [b] "=&r"(b), [v1] "=&r"(v1), [v2] "=&r"(v2), [v3] "=&r"(v3),
        [n] "+r"(delay), [sn] "+r"(sdelay)
      : [p] "r"(p), [m1] "r"(m1), [m2] "r"(m2), [m3] "r"(m3)
      : "memory");
    uint32_t e = 0;
    e |= (a  != i * 7 + 1) ? WAR_ERR_L0 : 0;
    e |= (b  != i * 7 + 1) ? WAR_ERR_L0B : 0;
    e |= (v1 != m1) ? WAR_ERR_L1 : 0;
    e |= (v2 != m3) ? WAR_ERR_L2 : 0;
    e |= (v3 != m3) ? WAR_ERR_L3 : 0;
    errs[i] = e;
  }
}
