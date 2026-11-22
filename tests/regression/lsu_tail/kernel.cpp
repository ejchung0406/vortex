#include <vx_spawn2.h>
#include "common.h"

// One warp whose last instruction is a load into x0 split across LSU
// packets: the upper half of the threads read a line they have just
// loaded (hits), the lower half one cold line each (misses). With fewer LSU
// lanes than threads the end-of-packet beat hits and retires while the
// first packets' misses are still out, and the warp exits behind it.
__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
	auto src = reinterpret_cast<volatile uint32_t*>(arg->src_addr);
	auto dst = reinterpret_cast<uint32_t*>(arg->dst_addr);
	uint32_t tid = threadIdx.x;
	bool hot = (tid >= arg->num_threads / 2);
	if (hot) {
		dst[tid] = src[tid];
	} else {
		dst[tid] = tid;
	}
	const volatile uint32_t* addr = hot ? &src[tid] : &src[LSU_TAIL_LINE_WORDS * (1 + tid)];
	__asm__ volatile ("lw zero, 0(%0)" :: "r"(addr) : "memory");
}
