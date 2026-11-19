// Two workloads on one device, split by core.
//
// A CTA (one warp) that lands on a core below `bw_cores` streams its slice of a
// buffer the host never writes; one on any other core runs a register-only
// multiply-add loop. CTAs go to whichever core has a free slot, so with several
// rounds both roles stay busy until the grid runs out, and the share of CTAs
// each role completed gives its rate over the run's cycles. The report line
// gives the bytes the streaming role read and the multiply-adds the compute
// role ran.

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
uint32_t steps    = 64;
uint32_t iters    = 256;
uint32_t rounds   = 2;
uint32_t grid     = 0;
uint32_t bw_cores = 1;
uint32_t unroll   = 1;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_buffer_h core_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex two-workload probe: streaming cores and compute cores." << std::endl;
  std::cout << "Usage: [-b streaming cores] [-n steps] [-u unroll] [-i multiply-adds] "
               "[-r rounds] [-g grid] [-k kernel] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "b:n:u:i:r:g:k:h")) != -1) {
    switch (c) {
    case 'b': bw_cores = atoi(optarg); break;
    case 'n': steps    = atoi(optarg); break;
    case 'u': unroll   = atoi(optarg); break;
    case 'i': iters    = atoi(optarg); break;
    case 'r': rounds   = atoi(optarg); break;
    case 'g': grid     = atoi(optarg); break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  bool unroll_ok = (unroll == 1 || unroll == 2 || unroll == 4 || unroll == 8);
  if (!unroll_ok || steps == 0 || steps % unroll != 0 || iters == 0 || iters % 4 != 0
   || iters > (1u << 24)) {
    std::cerr << "unroll must be 1, 2, 4 or 8 and divide steps; multiply-adds a multiple of 4, "
                 "at most 2^24" << std::endl;
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) vx_buffer_release(src_buffer);
    if (out_buffer) vx_buffer_release(out_buffer);
    if (core_buffer) vx_buffer_release(core_buffer);
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
  uint64_t src_size  = total_threads * steps * sizeof(float);
  uint64_t out_size  = total_threads * sizeof(float);
  uint64_t core_size = total_threads * sizeof(uint32_t);
  printf("cores=%u warps=%u threads=%u grid=%u bw_cores=%u steps=%u unroll=%u iters=%u\n",
         (unsigned)num_cores, (unsigned)num_warps, (unsigned)num_threads, grid, bw_cores, steps,
         unroll, iters);

  RT_CHECK(vx_buffer_create(device, src_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  RT_CHECK(vx_buffer_create(device, core_size, VX_MEM_WRITE, &core_buffer));
  RT_CHECK(vx_buffer_address(core_buffer, &kernel_arg.core_addr));
  kernel_arg.bw_cores = bw_cores;
  kernel_arg.steps    = steps;
  kernel_arg.unroll   = unroll;
  kernel_arg.iters    = iters;
  kernel_arg.mul      = 1.0f;
  kernel_arg.add      = 1.0f;

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
  std::vector<uint32_t> h_core(total_threads, 0);
  vx_event_h core_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_enqueue_read(queue, h_core.data(), core_buffer, 0, core_size, 1, &read_ev, &core_ev));
  RT_CHECK(vx_event_wait_value(core_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(core_ev);
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  // Streaming role: every element reads as `fill`; redo the kernel's accumulation
  // in its order. Compute role: mul = add = 1 counts the multiply-adds exactly.
  volatile float part = 0.0f;
  for (uint32_t i = 0; i < steps / unroll; ++i)
    part = part + fill;
  volatile float bw_expected = 0.0f;
  for (uint32_t u = 0; u < unroll; ++u)
    bw_expected = bw_expected + part;
  float cp_expected = (float)iters;

  uint64_t errors = 0, bw_threads = 0, cp_threads = 0;
  for (uint64_t g = 0; g < total_threads; ++g) {
    bool is_bw = h_core[g] < bw_cores;
    float expected = is_bw ? (float)bw_expected : cp_expected;
    (is_bw ? bw_threads : cp_threads) += 1;
    if (h_core[g] >= num_cores || h_out[g] != expected) {
      if (errors < 10)
        printf("*** error: [%llu] core=%u expected=%f, actual=%f\n", (unsigned long long)g,
               h_core[g], expected, h_out[g]);
      ++errors;
    }
  }
  printf("roles: bw_threads=%llu bw_bytes=%llu cp_threads=%llu cp_muladds=%llu\n",
         (unsigned long long)bw_threads, (unsigned long long)(bw_threads * steps * sizeof(float)),
         (unsigned long long)cp_threads, (unsigned long long)(cp_threads * iters));

  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " errors!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
