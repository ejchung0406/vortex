// Loads and stores to the same shared-memory words in program order, an atomic
// between them, masked lanes, and a read of another warp's store after a
// barrier (see kernel.cpp).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <vortex2.h>
#include "common.h"

#define RT_CHECK(_expr)                                         \
   do {                                                         \
     int _ret = _expr;                                          \
     if (0 == _ret)                                             \
       break;                                                   \
     printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);   \
     cleanup();                                                 \
     exit(-1);                                                  \
   } while (false)

const char* kernel_file = "kernel.vxbin";
uint32_t warps = 2;

vx_device_h device = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "w:h")) != -1) {
    switch (c) {
    case 'w': warps = atoi(optarg); break;
    default:  std::cout << "Usage: [-w warps] [-h]" << std::endl; exit(c == 'h' ? 0 : -1);
    }
  }
  if (warps == 0) {
    std::cout << "Error: needs a warp" << std::endl;
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (out_buffer) vx_buffer_release(out_buffer);
    if (kernel)  vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue)   vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t isa_flags = 0;
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  kernel_arg.atomic = (isa_flags & VX_ISA_STD_A) ? 1 : 0;

  uint64_t num_threads, num_warps;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  if (warps > num_warps || (num_threads % 2) != 0 || 4 * num_threads > 256) {
    std::cout << "Error: needs warps <= " << num_warps << " and an even thread count up to 64" << std::endl;
    cleanup();
    return -1;
  }
  const uint32_t T = (uint32_t)num_threads;

  uint64_t out_size = uint64_t(RW_NRES) * warps * T * sizeof(uint32_t);
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  vx_event_h launch_ev = nullptr, read_ev = nullptr;
  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 1;
  li.grid_dim[0]  = 1;
  li.block_dim[0] = warps * T;
  li.lmem_size    = warps * RW_REGION;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<uint32_t> h_out(RW_NRES * warps * T, 0);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  int errors = 0;
  for (uint32_t w = 0; w < warps; ++w) {
    for (uint32_t t = 0; t < T; ++t) {
      const uint32_t id = (w << 8) | t;
      const uint32_t idn = (w << 8) | (t ^ 1);
      const uint32_t a3 = (((0x22000000u | id) + 7) & ~0xffu) | 0x5a;
      uint32_t want[RW_NRES];
      want[0] = 0x11000000u | id;
      want[1] = 0x22000000u | id;
      want[2] = want[1] + 7;
      want[3] = a3;
      want[4] = (((0x22000000u | idn) + 7) & ~0xffu) | 0x5a;
      want[5] = 10 + id;
      want[6] = 15 + id;
      want[7] = 16 + id;
      want[8] = ((t % 3) == 1) ? ((0x33000000u | id) + ((0x33000000u | id) ^ 0xff)) : 0;
      want[9] = 0x44000000u | (((w + 1) % warps) << 8) | t;
      for (uint32_t i = 0; i < RW_NRES; ++i) {
        uint32_t got = h_out[(w * T + t) * RW_NRES + i];
        if (got != want[i]) {
          if (errors < 16) {
            printf("*** error: warp %u thread %u result %u: got %x, expected %x\n", w, t, i, got, want[i]);
          }
          ++errors;
        }
      }
    }
  }
  cleanup();
  if (errors != 0) {
    std::cout << "Found " << errors << " errors!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
