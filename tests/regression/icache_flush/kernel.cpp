#include <vx_intrinsics.h>
#include "common.h"

// Two images are built from this file, linked at the same address: their code
// differs only in the transform.
static inline uint32_t transform(uint32_t x) {
#ifdef KERNEL_B
  return x * 3 + 0x12345u;
#else
  return x ^ 0x5a5a5a5au;
#endif
}

int main() {
  kernel_arg_t* __UNIFORM__ arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
  size_t count      = arg->count;
  uint32_t* src_ptr = (uint32_t*)arg->src_addr;
  uint32_t* dst_ptr = (uint32_t*)arg->dst_addr;

  size_t offset     = vx_core_id() * count;

  for (size_t i = 0; i < count; ++i) {
    dst_ptr[offset + i] = transform(src_ptr[offset + i]);
  }

  // fence.i: the core orders it as a fence on the data side
  __asm__ volatile (".insn i 0x0f, 1, x0, x0, 0" ::: "memory");

  return 0;
}
