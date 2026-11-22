// Plain-load streaming bandwidth of one SM, in the shape of the H200 probe
// gpu/borrow_load.cu (see kernel.cpp).
//
//   -t T  threads per CTA (one CTA per core unless -g)
//   -b B  bytes per load, 4 or 8 (the part's kernel loads 16)
//   -u U  independent loads per thread per step: 1, 2, 4, 8, 16
//   -m X  DRAM mode: MB each CTA reads once (default)
//   -w W  L2 mode: every CTA reads the same W-MB window -p times after one
//         untimed pass
//   -g G  CTAs (default: one per core)
//   -l    the lean loop (see kernel.cpp)
// DRAM mode leaves the buffer unwritten: an upload would leave it in the L2,
// and unwritten memory reads as one fill value from memory. L2 mode writes
// a[e] = e.
//   -a    place the buffer past the heap's first 2 MB, which the superpage
//         heap maps with 4 KB pages
// Bytes in flight per SM: U * T * B. Reported: bytes over the cycles between
// the two CTA barriers around the timed loop, per CTA, as GB/s at 1.98 GHz.

#include <algorithm>
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
uint32_t xT = 1024, xB = 8, xU = 4, xP = 2, xG = 0;
double xM = 1, xW = 0;
bool xa = false, xl = false;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr, sum_buffer = nullptr, times_buffer = nullptr, pad_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Plain-load streaming bandwidth." << std::endl;
  std::cout << "Usage: [-t threads] [-b 4|8] [-u unroll] [-m MB per CTA] [-w window MB] [-p passes] "
               "[-g CTAs] [-l] [-a] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "t:b:u:m:w:p:g:lah")) != -1) {
    switch (c) {
    case 't': xT = atoi(optarg); break;
    case 'b': xB = atoi(optarg); break;
    case 'u': xU = atoi(optarg); break;
    case 'm': xM = atof(optarg); break;
    case 'w': xW = atof(optarg); break;
    case 'p': xP = atoi(optarg); break;
    case 'g': xG = atoi(optarg); break;
    case 'a': xa = true; break;
    case 'l': xl = true; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  bool u_ok = (xU == 1 || xU == 2 || xU == 4 || xU == 8 || xU == 16);
  if (!u_ok || (xB != 4 && xB != 8) || xT == 0) {
    show_usage();
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) vx_buffer_release(src_buffer);
    if (sum_buffer) vx_buffer_release(sum_buffer);
    if (times_buffer) vx_buffer_release(times_buffer);
    if (pad_buffer) vx_buffer_release(pad_buffer);
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

  uint64_t num_cores;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  const uint32_t G = xG ? xG : (uint32_t)num_cores;
  const uint64_t step_bytes = uint64_t(xT) * xB;
  const bool l2 = xW > 0;
  uint64_t region, base_steps, stride_steps, count, buf_bytes;
  if (l2) {
    region = uint64_t(xW * (1 << 20)) / step_bytes / xU * xU;
    base_steps = 0;
    stride_steps = region / G / xU * xU;
    count = region * xP;
    buf_bytes = region * step_bytes;
  } else {
    region = uint64_t(xM * (1 << 20)) / step_bytes / xU * xU;
    base_steps = region;
    stride_steps = 0;
    count = region;
    buf_bytes = region * step_bytes * G;
  }
  count -= count % xU;
  printf("LDSTREAM: ctas=%u threads=%u bytes=%u unroll=%u kb_in_flight=%llu mode=%s lean=%d region_steps=%llu "
         "count=%llu buffer=%llu\n", G, xT, xB, xU, (unsigned long long)(xU * step_bytes / 1024),
         l2 ? "l2" : "dram", xl ? 1 : 0, (unsigned long long)region, (unsigned long long)count,
         (unsigned long long)buf_bytes);

  if (xa) {
    RT_CHECK(vx_buffer_create(device, 2u << 20, VX_MEM_READ, &pad_buffer));
  }
  std::vector<uint32_t> h_src(buf_bytes / 4);
  for (size_t e = 0; e < h_src.size(); ++e)
    h_src[e] = (uint32_t)e;
  const uint64_t threads = uint64_t(G) * xT;
  RT_CHECK(vx_buffer_create(device, buf_bytes, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_buffer_create(device, threads * 16, VX_MEM_WRITE, &sum_buffer));
  RT_CHECK(vx_buffer_address(sum_buffer, &kernel_arg.sum_addr));
  RT_CHECK(vx_buffer_create(device, G * 16, VX_MEM_WRITE, &times_buffer));
  RT_CHECK(vx_buffer_address(times_buffer, &kernel_arg.times_addr));
  if (l2) {
    RT_CHECK(vx_enqueue_write(queue, src_buffer, 0, h_src.data(), buf_bytes, 0, nullptr, nullptr));
  } else {
    uint32_t fill = 0;
    vx_event_h fill_ev = nullptr;
    RT_CHECK(vx_enqueue_read(queue, &fill, src_buffer, 0, sizeof(fill), 0, nullptr, &fill_ev));
    RT_CHECK(vx_event_wait_value(fill_ev, 1, VX_TIMEOUT_INFINITE));
    vx_event_release(fill_ev);
    for (auto& w : h_src) w = fill;
  }

  kernel_arg.region_steps = region;
  kernel_arg.base_steps = base_steps;
  kernel_arg.stride_steps = stride_steps;
  kernel_arg.count = count;
  kernel_arg.warm = l2 ? 1 : 0;
  kernel_arg.bytes = xB;
  kernel_arg.unroll = xU;
  kernel_arg.lean = xl ? 1 : 0;

  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  vx_event_h launch_ev = nullptr, read_ev = nullptr, tread_ev = nullptr;
  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 1;
  li.grid_dim[0]  = G;
  li.block_dim[0] = xT;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));
  std::vector<uint64_t> h_sum(threads * 2);
  std::vector<uint64_t> h_t(G * 2);
  RT_CHECK(vx_enqueue_read(queue, h_sum.data(), sum_buffer, 0, threads * 16, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_enqueue_read(queue, h_t.data(), times_buffer, 0, G * 16, 1, &read_ev, &tread_ev));
  RT_CHECK(vx_event_wait_value(tread_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(tread_ev);
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  // Each thread's words: element (base + st) * T + t of B bytes, st over its steps.
  uint64_t errors = 0;
  const uint32_t wpl = xB / 4;
  auto word = [&](uint64_t e) -> uint64_t {
    return (wpl == 2) ? (uint64_t(h_src[e + 1]) << 32 | h_src[e]) : h_src[e];
  };
  for (uint32_t b = 0; b < G; ++b) {
    for (uint32_t t = 0; t < xT; ++t) {
      uint64_t s = 0;
      uint64_t st = (uint64_t(b) * stride_steps) % region;
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t e = ((uint64_t(b) * base_steps + st) * xT + t) * wpl;
        s += word(e);
        if (++st == region) st = 0;
      }
      uint64_t got = h_sum[2 * (uint64_t(b) * xT + t)];
      if (got != s) {
        if (errors < 10)
          printf("*** error: cta=%u thread=%u expected=%llu actual=%llu\n", b, t,
                 (unsigned long long)s, (unsigned long long)got);
        ++errors;
      }
    }
  }

  double sum_gbs = 0, min_cyc = 1e30, max_cyc = 0;
  const double bytes_cta = double(count) * step_bytes;
  for (uint32_t b = 0; b < G; ++b) {
    double cyc = double(h_t[2 * b + 1] - h_t[2 * b]);
    sum_gbs += bytes_cta / cyc * 1.98;
    min_cyc = std::min(min_cyc, cyc);
    max_cyc = std::max(max_cyc, cyc);
  }
  printf("LDSTREAM_CYCLES: min=%.0f max=%.0f bytes_per_cta=%.0f\n", min_cyc, max_cyc, bytes_cta);
  printf("LDSTREAM_B_PER_CLOCK_PER_SM: %.2f\n", sum_gbs / G / 1.98);
  printf("LDSTREAM_GBS_PER_SM: %.1f (at 1980 MHz) aggregate=%.1f\n", sum_gbs / G, sum_gbs);

  cleanup();
  if (errors != 0) {
    std::cout << "Found " << errors << " errors!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
