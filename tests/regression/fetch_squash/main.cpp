// Wrong-path fetches: every thread runs control transfers followed in memory
// by instructions that must not execute, a fetch that overtakes an older one,
// FP flag reads between FP operations and a barrier, and the host checks the
// sum each thread accumulated.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
uint32_t warps = 1;
uint32_t iters = 8;

vx_device_h device = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Usage: [-w warps] [-i iterations] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "w:i:h")) != -1) {
    switch (c) {
    case 'w': warps = atoi(optarg); break;
    case 'i': iters = atoi(optarg); break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  if (warps == 0 || iters == 0) {
    show_usage();
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

  uint64_t num_threads, num_warps;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  if (warps > num_warps) {
    std::cout << "Error: " << warps << " warps > " << num_warps << " per core" << std::endl;
    return -1;
  }

  uint64_t nthreads = warps * num_threads;
  uint64_t out_size = nthreads * 2 * sizeof(uint64_t);
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.iters = iters;

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
  li.block_dim[0] = warps * (uint32_t)num_threads;
  li.lmem_size    = 0;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<uint64_t> h_out(nthreads * 2, 0);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  int errors = 0;
  for (uint64_t t = 0; t < nthreads; ++t) {
    uint64_t sum = h_out[2 * t], err = h_out[2 * t + 1];
    uint64_t ref = t + uint64_t(iters) * FS_PER_ITER;
    if (sum != ref || err != 0) {
      if (errors < 16) {
        printf("*** error: thread %llu sum=0x%llx, expected=0x%llx, flag errors=0x%llx\n", (unsigned long long)t,
               (unsigned long long)sum, (unsigned long long)ref, (unsigned long long)err);
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
