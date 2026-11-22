#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// Instructions that follow a control transfer in memory and must not execute,
// and instructions that follow one that does not redirect and must execute
// once. A front end that fetches ahead of decode has the former in flight when
// the transfer is decoded; each would add 0x100000 to the sum.
#define POISON4(p) "add %0, %0, " p "\n\t" "add %0, %0, " p "\n\t" "add %0, %0, " p "\n\t" "add %0, %0, " p "\n\t"
#define POISON8(p) POISON4(p) POISON4(p)

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t iters = arg->iters;
  const uint32_t t = threadIdx.x;
  auto out = reinterpret_cast<uint64_t*>(arg->out_addr);
  size_t sum = t, one = 1, zero = 0, poison = 0x100000, err = 0;
  __asm__ volatile("" : "+r"(one), "+r"(zero), "+r"(poison));

  for (uint32_t i = 0; i < iters; ++i) {
    // taken branch, jal, jalr over poison; not-taken branch into counted adds
    __asm__ volatile(
      "bnez %1, 1f\n\t" POISON8("%3") "1:\n\t"
      "addi %0, %0, 0x1\n\t"
      "j 2f\n\t" POISON8("%3") "2:\n\t"
      "addi %0, %0, 0x2\n\t"
      "la t1, 3f\n\t"
      "jr t1\n\t" POISON8("%3") "3:\n\t"
      "addi %0, %0, 0x4\n\t"
      "bnez %2, 4f\n\t"
      "addi %0, %0, 0x8\n\t"
      "addi %0, %0, 0x10\n\t"
      "addi %0, %0, 0x20\n\t"
      "4:\n\t"
      // a branch as the next instruction after a branch, taken then not
      "bnez %1, 5f\n\t" "bnez %1, 6f\n\t" POISON4("%3") "5:\n\t"
      "bnez %2, 6f\n\t" "bnez %2, 6f\n\t"
      "addi %0, %0, 0x40\n\t"
      "6:\n\t"
      : "+r"(sum) : "r"(one), "r"(zero), "r"(poison) : "t1");

    // A fetch that overtakes an older one. Line A's last word is entered cold
    // while line B behind it is cached: the fetch ahead of B's first word
    // returns before the fetch of A's last. The two pad lines keep the fetches
    // ahead of the jumps out of line A.
    if (i == 0) {
      __asm__ volatile(
        "la t1, 9f\n\t"
        "j 8f\n\t"
        ".balign 64\n\t"
        ".fill 31, 4, 0x00000013\n\t"
        "7:\n\t"
        "addi %0, %0, 0x80\n\t"    // last word of line A
        "8:\n\t"
        "addi %0, %0, 0x100\n\t"   // first word of line B
        "jr t1\n\t" POISON8("%1")
        ".balign 64\n\t"
        ".fill 16, 4, 0x00000013\n\t"
        "9:\n\t"
        "la t1, 10f\n\t"
        "j 7b\n\t" POISON8("%1")
        "10:\n\t"
        : "+r"(sum) : "r"(poison) : "t1");
    } else {
      sum += 0x280;
    }

    // The FP flags read between FP operations: inexact set by the divide,
    // cleared, not set by exact adds, set again by the second divide. The
    // scoreboard orders a flag read after older writers only, so the FP
    // operation after each read takes an operand derived from the value read.
    {
      float a = 1.f + t, b = 3.f, c = 0.25f, q, s, z;
      size_t f0, f1, f2;
      __asm__ volatile("" : "+f"(a), "+f"(b), "+f"(c));
      __asm__ volatile(
        "csrw fflags, %9\n\t"
        "fdiv.s %3, %0, %1\n\t"
        "frflags %5\n\t"
        "and t0, %5, %9\n\t"
        "fcvt.s.w %8, t0\n\t"
        "fadd.s %4, %2, %8\n\t"
        "fadd.s %4, %4, %2\n\t"
        "csrw fflags, %9\n\t"
        "fadd.s %4, %4, %2\n\t"
        "frflags %6\n\t"
        "and t0, %6, %9\n\t"
        "fcvt.s.w %8, t0\n\t"
        "fadd.s %3, %3, %8\n\t"
        "fdiv.s %3, %3, %1\n\t"
        "fadd.s %4, %4, %2\n\t"
        "frcsr %7\n\t"
        : "+f"(a), "+f"(b), "+f"(c), "=&f"(q), "=&f"(s), "=&r"(f0), "=&r"(f1), "=&r"(f2), "=&f"(z) : "r"(zero) : "t0");
      // fflags is one register per warp: the divides are inexact for some
      // thread of every warp
      err |= ((f0 & 1) ^ 1) | ((f1 != 0) << 1) | (((f2 & 1) ^ 1) << 2) | ((size_t)(s != 1.0f) << 3);
      sum += 0x400;
    }

    // a barrier and a warp-wide vote between counted adds
    sum += 0x800;
    __syncthreads();
    sum += 0x4D;
  }

  out[2 * t] = sum;
  out[2 * t + 1] = err;
}
