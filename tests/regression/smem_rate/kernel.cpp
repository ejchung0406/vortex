#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include <vx_barrier.h>
#include "common.h"

#define R2(x) x x
#define R4(x) R2(x) R2(x)
#define R8(x) R4(x) R4(x)
#define R16(x) R8(x) R8(x)
#define R32(x) R16(x) R16(x)
#define R64(x) R32(x) R32(x)
#define R128(x) R64(x) R64(x)

#define IADD4 "add %0, %0, %8\n\tadd %1, %1, %8\n\tadd %2, %2, %8\n\tadd %3, %3, %8\n\t"
#define IADD8 IADD4 "add %4, %4, %8\n\tadd %5, %5, %8\n\tadd %6, %6, %8\n\tadd %7, %7, %8\n\t"
#define FMA4 "fmadd.s %0, %0, %8, %9\n\tfmadd.s %1, %1, %8, %9\n\tfmadd.s %2, %2, %8, %9\n\tfmadd.s %3, %3, %8, %9\n\t"
#define FMA8 FMA4 "fmadd.s %4, %4, %8, %9\n\tfmadd.s %5, %5, %8, %9\n\tfmadd.s %6, %6, %8, %9\n\tfmadd.s %7, %7, %8, %9\n\t"
// taken branch over one never-executed add (%9 != 0 at run time)
#define BRSKIP "bnez %9, 1f\n\taddi %4, %4, 1\n1:\n\t"
#define A1 "add %1, %1, %8\n\t"
#define A2 "add %2, %2, %8\n\t"
#define A3 "add %3, %3, %8\n\t"
#define A0 "add %0, %0, %8\n\t"
#define BLK4 A1 A2 A3 BRSKIP
#define BLK16 A0 A1 A2 A3 A0 A1 A2 A3 A0 A1 A2 A3 A0 A1 A2 BRSKIP
#define LDS8 "lw %0, 0(%12)\n\tlw %1, 0(%12)\n\tlw %2, 0(%12)\n\tlw %3, 0(%12)\n\t" \
             "lw %4, 0(%12)\n\tlw %5, 0(%12)\n\tlw %6, 0(%12)\n\tlw %7, 0(%12)\n\t" \
             "xor %8, %8, %0\n\txor %8, %8, %1\n\txor %9, %9, %2\n\txor %9, %9, %3\n\t" \
             "xor %10, %10, %4\n\txor %10, %10, %5\n\txor %11, %11, %6\n\txor %11, %11, %7\n\t"
#if __riscv_xlen == 64
#define LDP "ld"
#define STP "sd"
#else
#define LDP "lw"
#define STP "sw"
#endif
#define LDN8 "lw %0, 0(%8)\n\tlw %1, 0(%8)\n\tlw %2, 0(%8)\n\tlw %3, 0(%8)\n\t" \
             "lw %4, 0(%8)\n\tlw %5, 0(%8)\n\tlw %6, 0(%8)\n\tlw %7, 0(%8)\n\t"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t kind = arg->kind, iters = arg->iters;
  const uint32_t t = threadIdx.x;
  auto out = reinterpret_cast<uint64_t*>(arg->out_addr);
  auto lmem = reinterpret_cast<uintptr_t*>(__local_mem());
  uintptr_t myword = (uintptr_t)(lmem + t);
  if (kind == WR_STS_OWN4 || kind == WR_LDS_NOUSE4 || kind == WR_LDS_OWN4) {
    auto l4 = reinterpret_cast<uint32_t*>(lmem);
    l4[t] = t;
    myword = (uintptr_t)(l4 + t);
  } else {
    lmem[t] = myword;   // self pointer for the chase
  }
  size_t y = 3 + (kind & 1), nz = 1 + (t & 1);
  size_t a0 = t, a1 = t + 1, a2 = t + 2, a3 = t + 3, a4 = t + 4, a5 = t + 5, a6 = t + 6, a7 = t + 7;
  size_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
  float f0 = 1.f + t, f1 = f0 + 1, f2 = f0 + 2, f3 = f0 + 3, f4 = f0 + 4, f5 = f0 + 5, f6 = f0 + 6, f7 = f0 + 7;
  float fa = 0.999f, fb = 1e-3f;
  if (kind == WR_STS_SAME) myword = (uintptr_t)(lmem + (t / vx_num_threads()) * 16);
  if (kind == WR_LDS_CHASE) a0 = myword;
  // opaque to the compiler, so it keeps them in registers instead of reloading them in the loop
  __asm__ volatile("" : "+f"(fa), "+f"(fb), "+r"(y), "+r"(nz));
  __syncthreads();

  uint64_t c0 = csr_read(VX_CSR_MCYCLE);
  switch (kind) {
  case WR_INT_DEP:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128("add %0, %0, %1\n\t") : "+r"(a0) : "r"(y));
    break;
  case WR_INT_IND4:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R32(IADD4) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y));
    break;
  case WR_INT_IND8:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R16(IADD8) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y));
    break;
  case WR_FMA_DEP:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128("fmadd.s %0, %0, %1, %2\n\t") : "+f"(f0) : "f"(fa), "f"(fb));
    break;
  case WR_FMA_IND4:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R32(FMA4) : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3), "+f"(f4), "+f"(f5), "+f"(f6), "+f"(f7) : "f"(fa), "f"(fb));
    break;
  case WR_FMA_IND8:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R16(FMA8) : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3), "+f"(f4), "+f"(f5), "+f"(f6), "+f"(f7) : "f"(fa), "f"(fb));
    break;
  case WR_BR1:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128(BRSKIP) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(nz));
    break;
  case WR_BR4:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R32(BLK4) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(nz));
    break;
  case WR_BR16:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R8(BLK16) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(nz));
    break;
  case WR_LDS_OWN4:
  case WR_LDS_OWN:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R16(LDS8) : "=&r"(a0), "=&r"(a1), "=&r"(a2), "=&r"(a3), "=&r"(a4), "=&r"(a5), "=&r"(a6), "=&r"(a7),
                       "+r"(b0), "+r"(b1), "+r"(b2), "+r"(b3) : "r"(myword) : "memory");
    break;
  case WR_LDS_CHASE:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128(LDP " %0, 0(%0)\n\t") : "+r"(a0) :: "memory");
    break;
  case WR_STS_SAME:
  case WR_STS_OWN4:
  case WR_STS_OWN:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128("sw %1, 0(%0)\n\t") :: "r"(myword), "r"(a1) : "memory");
    break;
  case WR_STS_OWN8:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128(STP " %1, 0(%0)\n\t") :: "r"(myword), "r"(a1) : "memory");
    break;
  case WR_CSR:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128("csrr %0, 0xcc0\n\t") : "+r"(a0));
    a0 = t;
    break;
  case WR_SPLITJOIN:
    for (uint32_t i = 0; i < iters; ++i) {
      R64({ int s_ = vx_split((int)nz); vx_join(s_); })
    }
    break;
  case WR_BAR: {
    vortex::barrier bar(0);
    for (uint32_t i = 0; i < iters; ++i) {
      R128(bar.arrive_and_wait();)
    }
    break;
  }
  case WR_DIV:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R128("remuw %0, %0, %1\n\t") : "+r"(a0) : "r"(y));
    break;
  case WR_LDS_NOUSE4:
  case WR_LDS_NOUSE:
    for (uint32_t i = 0; i < iters; ++i)
      __asm__ volatile(R16(LDN8) : "=&r"(a0), "=&r"(a1), "=&r"(a2), "=&r"(a3), "=&r"(a4), "=&r"(a5), "=&r"(a6), "=&r"(a7) : "r"(myword) : "memory");
    break;
  default:
    break;
  }
  uint64_t c1 = csr_read(VX_CSR_MCYCLE);

  // The loaded words are addresses in local memory: they are taken out, so the
  // result does not depend on where local memory is.
  if (kind == WR_LDS_CHASE) {
    a0 -= myword;
  }
  if (kind == WR_LDS_NOUSE4 || kind == WR_LDS_OWN4) {
    a0 -= t; a1 -= t; a2 -= t; a3 -= t; a4 -= t; a5 -= t; a6 -= t; a7 -= t;
  }
  if (kind == WR_LDS_OWN || kind == WR_LDS_NOUSE) {
    size_t v = (size_t)(intptr_t)(int32_t)(uint32_t)myword;
    a0 -= v; a1 -= v; a2 -= v; a3 -= v; a4 -= v; a5 -= v; a6 -= v; a7 -= v;
  }

  // every thread's final registers, which the host checks against its own
  // evaluation of the class: a run that executed a different instruction stream
  // gives a different value
  float fv = f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7;
  uint32_t fbits;
  __builtin_memcpy(&fbits, &fv, sizeof(fbits));
  uint32_t nw = blockDim.x / vx_num_threads();
  out[2 * nw + t] = (a0 ^ (a1 << 1) ^ (a2 << 2) ^ (a3 << 3) ^ (a4 << 4) ^ (a5 << 5) ^ (a6 << 6) ^ (a7 << 7)
                     ^ (b0 << 8) ^ (b1 << 9) ^ (b2 << 10) ^ (b3 << 11)) + fbits;
  if (t == 0) {
    out[2 * nw + blockDim.x] = sizeof(size_t);
  }
  if ((t % vx_num_threads()) == 0) {
    uint32_t w = t / vx_num_threads();
    out[2 * w] = c0;
    out[2 * w + 1] = c1;
  }
}
