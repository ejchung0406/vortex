// Instruction cache flush at a kernel boundary: two different images run, one
// after the other, from the same device address.
//
// Each image is linked at the same fixed address and computes a different
// transform of its input. Every launch is followed by the cache flush, which
// also invalidates each core's instruction cache. A core that kept the
// previous image's lines would run the previous transform, or a mix of both
// images, and the round's check fails. The images alternate so each one also
// runs over lines the other one left behind.

#include <iostream>
#include <unistd.h>
#include <string.h>
#include <vortex2.h>
#include <vector>
#include "common.h"

// Bounded waits so a wedged device fails the run instead of hanging it.
#define WAIT_TIMEOUT_NS (60ull * 1000 * 1000 * 1000)

#define RT_CHECK(_expr)                                         \
   do {                                                         \
     int _ret = _expr;                                          \
     if (0 == _ret)                                             \
       break;                                                   \
     printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);   \
     cleanup();                                                 \
     exit(-1);                                                  \
   } while (false)

///////////////////////////////////////////////////////////////////////////////

const char* kernel_a_file = "kernel.vxbin";
const char* kernel_b_file = "kernel_b.vxbin";
uint32_t count = 0;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h dst_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;

static uint64_t num_cores = 0;
static uint64_t num_threads = 0;
static uint32_t num_points = 0;
static uint32_t buf_size = 0;
static kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex icache-flush test." << std::endl;
  std::cout << "Usage: [-n words][-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:h")) != -1) {
    switch (c) {
    case 'n':
      count = atoi(optarg);
      break;
    case 'h':
      show_usage();
      exit(0);
      break;
    default:
      show_usage();
      exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) {
      vx_buffer_release(src_buffer);
    }
    if (dst_buffer) {
      vx_buffer_release(dst_buffer);
    }
    if (kernel) {
      vx_kernel_release(kernel);
    }
    if (module_) {
      vx_module_release(module_);
    }
    if (queue) {
      vx_queue_release(queue);
    }
    vx_device_release(device);
  }
  device = nullptr;
  src_buffer = nullptr;
  dst_buffer = nullptr;
  kernel = nullptr;
  module_ = nullptr;
  queue = nullptr;
}

static uint32_t transform(bool is_b, uint32_t x) {
  return is_b ? (x * 3 + 0x12345u) : (x ^ 0x5a5a5a5au);
}

// Load one image, run it once over a nonce-derived pattern and release it.
static int load_and_run(const char* round, bool is_b, uint32_t nonce) {
  RT_CHECK(vx_module_load_file(device, is_b ? kernel_b_file : kernel_a_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  std::vector<uint32_t> h_src(num_points);
  std::vector<uint32_t> h_dst(num_points);
  for (uint32_t i = 0; i < num_points; ++i) {
    h_src[i] = nonce * (i + 1) + i;
    h_dst[i] = 0;
  }

  RT_CHECK(vx_enqueue_write(queue, src_buffer, 0, h_src.data(), buf_size, 0, nullptr, nullptr));

  vx_event_h launch_ev = nullptr, read_ev = nullptr;
  {
    vx_launch_info_t li = {};
    li.struct_size  = sizeof(li);
    li.kernel       = kernel;
    li.args_host    = &kernel_arg;
    li.args_size    = sizeof(kernel_arg);
    li.ndim         = 1;
    li.grid_dim[0]  = (uint32_t)num_cores;
    li.block_dim[0] = (uint32_t)num_threads;
    RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));
  }
  RT_CHECK(vx_event_wait_value(launch_ev, 1, WAIT_TIMEOUT_NS));

  RT_CHECK(vx_enqueue_read(queue, h_dst.data(), dst_buffer, 0, buf_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, WAIT_TIMEOUT_NS));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  vx_kernel_release(kernel);
  kernel = nullptr;
  vx_module_release(module_);
  module_ = nullptr;

  int errors = 0;
  for (uint32_t i = 0; i < num_points; ++i) {
    auto cur = h_dst[i];
    auto ref = transform(is_b, h_src[i]);
    if (cur != ref) {
      if (errors < 8) {
        printf("*** %s: [%d] expected=0x%x, actual=0x%x\n", round, i, ref, cur);
      }
      ++errors;
    }
  }
  std::cout << round << ": " << (errors ? "FAILED" : "verified") << std::endl;
  return errors;
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  if (count == 0) {
    count = 1;
  }

  RT_CHECK(vx_device_open(0, &device));

  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));

  num_points = count * num_cores;
  buf_size = num_points * sizeof(uint32_t);

  RT_CHECK(vx_buffer_create(device, buf_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_buffer_create(device, buf_size, VX_MEM_WRITE, &dst_buffer));
  RT_CHECK(vx_buffer_address(dst_buffer, &kernel_arg.dst_addr));
  kernel_arg.count = count;

  int errors = 0;
  errors += load_and_run("round 1 (image A)", false, 0x9e3779b9u);
  errors += load_and_run("round 2 (image B over A)", true, 0x7f4a7c15u);
  errors += load_and_run("round 3 (image A over B)", false, 0x85ebca6bu);
  errors += load_and_run("round 4 (image B over A)", true, 0xc2b2ae35u);

  cleanup();

  if (errors != 0) {
    std::cout << "Found " << std::dec << errors << " errors!" << std::endl;
    std::cout << "FAILED!" << std::endl;
    return errors != 0;
  }

  std::cout << "Test PASSED" << std::endl;
  return 0;
}
