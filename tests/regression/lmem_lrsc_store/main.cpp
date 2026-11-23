// A plain store from one warp between another warp's load-reserved and
// store-conditional on a shared-memory word must break the reservation,
// whichever engine of the local memory unit carries the store (see kernel.cpp).
// Needs the A extension.

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
uint32_t writer = 1;

vx_device_h device = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "w:s:h")) != -1) {
    switch (c) {
    case 'w': warps = atoi(optarg); break;
    case 's': writer = atoi(optarg); break;
    default:  std::cout << "Usage: [-w warps] [-s storing warp] [-h]" << std::endl; exit(c == 'h' ? 0 : -1);
    }
  }
  if (warps < 2 || writer == 0 || writer >= warps) {
    std::cout << "Error: needs two warps or more and a storing warp other than warp 0" << std::endl;
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
  if ((isa_flags & VX_ISA_STD_A) == 0) {
    std::cout << "Error: the A extension is disabled (build with -DVX_CFG_EXT_A_ENABLE)" << std::endl;
    cleanup();
    return -1;
  }

  uint64_t num_threads, num_warps;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  if (warps > num_warps) {
    std::cout << "Error: " << warps << " warps > " << num_warps << " per core" << std::endl;
    cleanup();
    return -1;
  }
  const uint32_t T = (uint32_t)num_threads;

  uint64_t out_size = 3ull * T * sizeof(uint32_t);
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.writer = writer;
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
  li.lmem_size    = T * 8;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<uint32_t> h_out(3 * T, 0);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  int errors = 0;
  for (uint32_t t = 0; t < T; ++t) {
    const bool stored = (t & 1) == 0;
    const uint32_t fail = h_out[t], word = h_out[T + t], old = h_out[2 * T + t];
    const uint32_t want = stored ? (0x2000 + t) : (0x3000 + t);
    if (old != 0x1000 + t || (stored ? (fail == 0) : (fail != 0)) || word != want) {
      if (errors < 16) {
        printf("*** error: thread %u (%s): lr=%x sc=%u word=%x, expected lr=%x sc %s word=%x\n", t,
               stored ? "stored to" : "left alone", old, fail, word, 0x1000 + t, stored ? "failing" : "0", want);
      }
      ++errors;
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
