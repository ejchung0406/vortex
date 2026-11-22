// Shared-memory probe beside streaming neighbours (see kernel.cpp).
//
//   -k 0|1|2   probe: smem_bar, bar_only, smem_only
//   -n 0|1|2   neighbours: idle, loads, loads_bar
//   -i N       probe iterations (the first quarter is not counted)
//   -c CTAs    per SM, the probe included (default 16)
//   -d cycles  probe delay before its first iteration (default 20000)
//   -s MB      the neighbours' stream (default 32, past the 8.25 MB L2)
//   -N iters   a neighbour stops after this many iterations (default: when the probe is done)
// Reported: percentiles of the probe's cycles per iteration.

#include <algorithm>
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
uint32_t xk = 0, xn = 0, xi = 200, xc = 16, xd = 20000, xs = 32, xN = 0xffffffff;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr, flag_buffer = nullptr, it_buffer = nullptr, sum_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Usage: [-k probe 0..2] [-n neighbours 0..2] [-i iters] [-c CTAs] [-d delay] [-s stream MB] [-N neighbour iters] [-h]"
            << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "k:n:i:c:d:s:N:h")) != -1) {
    switch (c) {
    case 'k': xk = atoi(optarg); break;
    case 'n': xn = atoi(optarg); break;
    case 'i': xi = atoi(optarg); break;
    case 'c': xc = atoi(optarg); break;
    case 'd': xd = atoi(optarg); break;
    case 's': xs = atoi(optarg); break;
    case 'N': xN = atoi(optarg); break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  if (xk > 2 || xn > 2 || xi < 8 || xc < 1) {
    show_usage();
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) vx_buffer_release(src_buffer);
    if (flag_buffer) vx_buffer_release(flag_buffer);
    if (it_buffer) vx_buffer_release(it_buffer);
    if (sum_buffer) vx_buffer_release(sum_buffer);
    if (kernel)  vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue)   vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

static double pct(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * v.size()))];
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  const uint32_t T = 128;
  const uint64_t src_bytes = uint64_t(xs) << 20;
  // The stream is never written: its lines come from memory as one fill value.
  RT_CHECK(vx_buffer_create(device, src_bytes, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_buffer_create(device, 64, VX_MEM_READ | VX_MEM_WRITE, &flag_buffer));
  RT_CHECK(vx_buffer_address(flag_buffer, &kernel_arg.flag_addr));
  RT_CHECK(vx_buffer_create(device, xi * 4, VX_MEM_WRITE, &it_buffer));
  RT_CHECK(vx_buffer_address(it_buffer, &kernel_arg.itcyc_addr));
  RT_CHECK(vx_buffer_create(device, xc * T * 4, VX_MEM_WRITE, &sum_buffer));
  RT_CHECK(vx_buffer_address(sum_buffer, &kernel_arg.sums_addr));
  uint32_t zero[16] = {};
  RT_CHECK(vx_enqueue_write(queue, flag_buffer, 0, zero, sizeof(zero), 0, nullptr, nullptr));
  kernel_arg.nchunks = uint32_t(src_bytes / 8192);
  kernel_arg.probe_kind = xk;
  kernel_arg.neighbours = xn;
  kernel_arg.probe_iters = xi;
  kernel_arg.delay = xd;
  kernel_arg.max_neigh = xN;

  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  vx_event_h launch_ev = nullptr, read_ev = nullptr;
  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 1;
  li.grid_dim[0]  = xc;
  li.block_dim[0] = T;
  li.lmem_size    = 8192 + 64; // and the neighbours' stop word
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));
  std::vector<uint32_t> h_it(xi);
  RT_CHECK(vx_enqueue_read(queue, h_it.data(), it_buffer, 0, xi * 4, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  std::vector<double> v;
  for (uint32_t i = xi / 4; i < xi; ++i)
    v.push_back(h_it[i]);
  const char* kinds[] = {"smem_bar", "bar_only", "smem_only"};
  const char* neighs[] = {"idle", "loads", "loads_bar"};
  double total = 0;
  for (auto x : h_it) total += x;
  printf("SMPROBE: probe_kind=%s neighbours=%s ctas=%u iters=%u probe_iter_cycles: p25=%.0f p50=%.0f p75=%.0f p95=%.0f max=%.0f all_iters=%.0f\n",
         kinds[xk], neighs[xn], xc, xi, pct(v, .25), pct(v, .5), pct(v, .75), pct(v, .95), pct(v, 1.0), total);
  cleanup();
  std::cout << "PASSED!" << std::endl;
  return 0;
}
