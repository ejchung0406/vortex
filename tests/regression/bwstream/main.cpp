// Streaming-read bandwidth probe.
//
// Every thread sums `steps` floats of a buffer the host never writes, so each
// line is a memory read that no cache holds. The grid is `rounds` CTAs (one
// warp each) per warp slot of the device unless -g gives the CTA count; the
// bytes read are grid * threads * steps * 4. Read the bandwidth from the perf
// report (--perf=7): memory reads over cycles.

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
uint32_t steps   = 64;
uint32_t rounds  = 1;
uint32_t grid    = 0;
uint32_t pattern = BWSTREAM_COALESCED;
uint32_t unroll  = 1;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex streaming-read bandwidth probe." << std::endl;
  std::cout << "Usage: [-n steps] [-r rounds] [-g grid] [-p 0:coalesced|1:rows] "
               "[-u unroll] [-k kernel] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:r:g:p:u:k:h")) != -1) {
    switch (c) {
    case 'n': steps   = atoi(optarg); break;
    case 'r': rounds  = atoi(optarg); break;
    case 'g': grid    = atoi(optarg); break;
    case 'p': pattern = atoi(optarg); break;
    case 'u': unroll  = atoi(optarg); break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  bool unroll_ok = (unroll == 1 || unroll == 2 || unroll == 4 || unroll == 8);
  if (!unroll_ok || steps == 0 || steps % unroll != 0 || pattern > BWSTREAM_ROWS) {
    std::cerr << "unroll must be 1, 2, 4 or 8 and divide steps; pattern is 0 or 1" << std::endl;
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) vx_buffer_release(src_buffer);
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

  uint64_t num_threads, num_warps, num_cores;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  if (grid == 0)
    grid = (uint32_t)(num_cores * num_warps) * rounds;

  uint64_t total_threads = (uint64_t)grid * num_threads;
  uint64_t src_size = total_threads * steps * sizeof(float);
  uint64_t out_size = total_threads * sizeof(float);
  printf("cores=%u warps=%u threads=%u grid=%u steps=%u pattern=%s unroll=%u bytes=%llu\n",
         (unsigned)num_cores, (unsigned)num_warps, (unsigned)num_threads, grid, steps,
         pattern == BWSTREAM_ROWS ? "rows" : "coalesced", unroll,
         (unsigned long long)src_size);

  RT_CHECK(vx_buffer_create(device, src_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.steps   = steps;
  kernel_arg.pattern = pattern;
  kernel_arg.unroll  = unroll;

  // Unwritten device memory reads as one fill value; fetch it to predict the sums.
  float fill = 0.0f;
  {
    vx_event_h fill_ev = nullptr;
    RT_CHECK(vx_enqueue_read(queue, &fill, src_buffer, 0, sizeof(fill), 0, nullptr, &fill_ev));
    RT_CHECK(vx_event_wait_value(fill_ev, 1, VX_TIMEOUT_INFINITE));
    vx_event_release(fill_ev);
  }

  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  vx_event_h launch_ev = nullptr, read_ev = nullptr;
  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 1;
  li.grid_dim[0]  = grid;
  li.block_dim[0] = (uint32_t)num_threads;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<float> h_out(total_threads, 0.0f);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  // Every element reads as `fill`: redo the kernel's accumulation in its order.
  volatile float part = 0.0f;
  for (uint32_t i = 0; i < steps / unroll; ++i)
    part = part + fill;
  volatile float sum = 0.0f;
  for (uint32_t u = 0; u < unroll; ++u)
    sum = sum + part;

  uint64_t errors = 0;
  for (uint64_t g = 0; g < total_threads; ++g) {
    float expected = sum + (float)(g + 1);
    if (h_out[g] != expected) {
      if (errors < 10)
        printf("*** error: [%llu] expected=%f, actual=%f\n", (unsigned long long)g,
               expected, h_out[g]);
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
