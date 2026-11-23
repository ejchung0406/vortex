// Byte-level check of shared-memory stores whose lanes meet in one bank word
// (see kernel.cpp). The result does not depend on how the local memory unit
// groups the lanes into bank accesses: it must be the same with and without
// VX_CFG_LMEM_WR_MERGE and VX_CFG_LMEM_WR_ROW.

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
uint32_t warps = 2;
uint32_t cases = WM_ALL;

vx_device_h device = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "w:c:h")) != -1) {
    switch (c) {
    case 'w': warps = atoi(optarg); break;
    case 'c': cases = strtoul(optarg, nullptr, 0); break;
    default:  std::cout << "Usage: [-w warps] [-c case mask] [-h]" << std::endl; exit(c == 'h' ? 0 : -1);
    }
  }
  if (warps == 0) {
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

template <typename V>
static void put(std::vector<uint8_t>& r, uint32_t off, V v) {
  memcpy(r.data() + off, &v, sizeof(V));
}

// a warp's region after the kernel: the stores applied in thread order
static std::vector<uint8_t> reference(uint32_t w, uint32_t T, uint32_t nw) {
  std::vector<uint8_t> r(WM_REGION, 0);
  for (uint32_t t = 0; t < T; ++t) {
    if (cases & 1)  put<uint8_t>(r, WM_BYTES + t, uint8_t(0x80 | t));
    if (cases & 2)  put<uint16_t>(r, WM_HALVES + 2 * t, uint16_t(0x4000 | (w << 8) | t));
    if (cases & 4)  put<uint32_t>(r, WM_WORDS + 4 * t, 0x57000000u | (w << 8) | t);
    if (cases & 8)  put<uint32_t>(r, WM_SAME, 0xc0de0000u | (w << 8) | t);
    if (cases & 16) put<uint32_t>(r, WM_PAIRS + 8 * (t / 8) + 4 * (t % 2), 0xa5000000u | (w << 8) | t);
    if (cases & 32) put<uint16_t>(r, WM_QUADS + 8 * (t / 8) + 2 * (t % 4), uint16_t(0x3300 | t));
    if (cases & 64) {
      uint32_t v = 0x12340000u | (w << 8) | t;
      put<uint32_t>(r, WM_RDBACK + 4 * t, v);
      put<uint32_t>(r, WM_RESULT + 4 * t, ~v);
    }
    if (cases & 128) {
      put<uint32_t>(r, WM_SEQ + 4 * t, (0x66000000u | (w << 8) | t));
      put<uint8_t>(r, WM_SEQ + 4 * t + 1, uint8_t(0xee));
    }
    if ((cases & 256) && (t % 3) != 0) {
      put<uint32_t>(r, WM_MASKED + 4 * t, 0x3a000000u | (w << 8) | t);
    }
    if ((cases & 512) && (t % 8) < 3) {
      uint32_t off = ((t % 8) == 0) ? WM_ROW : (((t % 8) == 2) ? 4 : 0);
      put<uint32_t>(r, WM_XROW + 8 * (t / 8) + off, 0x5c000000u | (w << 8) | t);
    }
    if (cases & 1024) {
      put<uint32_t>(r, WM_XWARP + 4 * t, 0x77000000u | (w << 8) | t);
      put<uint32_t>(r, WM_XWRES + 4 * t, 0x77000000u | (((w + 1) % nw) << 8) | t);
    }
  }
  return r;
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t num_threads, num_warps;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  if (warps > num_warps || 4 * num_threads > 128) {
    std::cout << "Error: needs warps <= " << num_warps << " and at most 32 threads" << std::endl;
    return -1;
  }

  uint64_t out_size = (uint64_t)warps * WM_REGION;
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.cases = cases;
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
  li.lmem_size    = warps * WM_REGION;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<uint8_t> h_out(out_size, 0);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  int errors = 0;
  for (uint32_t w = 0; w < warps; ++w) {
    auto ref = reference(w, (uint32_t)num_threads, warps);
    for (uint32_t b = 0; b < WM_REGION; ++b) {
      uint8_t got = h_out[(uint64_t)w * WM_REGION + b];
      if (got != ref[b]) {
        if (errors < 16) {
          printf("*** error: warp %u byte %u: got %02x, expected %02x\n", w, b, got, ref[b]);
        }
        ++errors;
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
