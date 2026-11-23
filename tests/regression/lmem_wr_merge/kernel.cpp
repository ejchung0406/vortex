#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

// Stores of every size into shared memory, laid out so that the lanes of one
// warp request meet in one bank word: writing different bytes of it, the same
// bytes, or both in one request. The warp's region is then copied
// out for the host, which knows what each byte must hold: lanes of one store
// land in thread order.
__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  const uint32_t T = vx_num_threads();
  const uint32_t t = threadIdx.x % T;
  const uint32_t w = threadIdx.x / T;
  auto out = reinterpret_cast<uint64_t*>(arg->out_addr);
  auto reg = reinterpret_cast<uint8_t*>(__local_mem()) + w * WM_REGION;
  auto words = reinterpret_cast<volatile uint64_t*>(reg);
  const uint32_t nwords = WM_REGION / 8;

  for (uint32_t i = t; i < nwords; i += T) {
    words[i] = 0;
  }
  __syncthreads();

  const uint32_t cases = arg->cases;
  if (cases & 1)
    *reinterpret_cast<volatile uint8_t*>(reg + WM_BYTES + t) = uint8_t(0x80 | t);
  if (cases & 2)
    *reinterpret_cast<volatile uint16_t*>(reg + WM_HALVES + 2 * t) = uint16_t(0x4000 | (w << 8) | t);
  if (cases & 4)
    *reinterpret_cast<volatile uint32_t*>(reg + WM_WORDS + 4 * t) = 0x57000000u | (w << 8) | t;
  if (cases & 8)
    *reinterpret_cast<volatile uint32_t*>(reg + WM_SAME) = 0xc0de0000u | (w << 8) | t;

  // lanes that write the same bytes and lanes that write other bytes of the
  // word, interleaved
  if (cases & 16)
    *reinterpret_cast<volatile uint32_t*>(reg + WM_PAIRS + 8 * (t / 8) + 4 * (t % 2)) = 0xa5000000u | (w << 8) | t;
  if (cases & 32)
    *reinterpret_cast<volatile uint16_t*>(reg + WM_QUADS + 8 * (t / 8) + 2 * (t % 4)) = uint16_t(0x3300 | t);

  if (cases & 64) {
    auto rb = reinterpret_cast<volatile uint32_t*>(reg + WM_RDBACK + 4 * t);
    *rb = 0x12340000u | (w << 8) | t;
    const uint32_t got = *rb;
    *reinterpret_cast<volatile uint32_t*>(reg + WM_RESULT + 4 * t) = ~got;
  }
  if (cases & 128) {
    *reinterpret_cast<volatile uint32_t*>(reg + WM_SEQ + 4 * t) = 0x66000000u | (w << 8) | t;
    *reinterpret_cast<volatile uint8_t*>(reg + WM_SEQ + 4 * t + 1) = 0xee;
  }
  __syncthreads();

  for (uint32_t i = t; i < nwords; i += T) {
    out[w * nwords + i] = words[i];
  }
}
