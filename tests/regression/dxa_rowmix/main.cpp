#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <vector>

#include <VX_types.h>
#include <vortex.h>
#include <dxa.h>

#include "common.h"

#define RT_CHECK(_expr)                                         \
  do {                                                          \
    int _ret = _expr;                                           \
    if (0 == _ret)                                              \
      break;                                                    \
    printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);   \
    cleanup();                                                  \
    exit(-1);                                                   \
  } while (false)

const char* kernel_file = "kernel.vxbin";
uint32_t xfers  = 4;
uint32_t rounds = 2;
uint32_t grid   = 2;

// Rows of 832 bytes: 13 lines, so a tile's rows start at every offset in a line.
constexpr uint32_t kSize0 = 208;
constexpr uint32_t kSize1 = 40;
constexpr uint32_t kTile0 = 24;
constexpr uint32_t kTile1 = 4;
constexpr uint32_t kRegion = 512;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Usage: [-x transfers per CTA 1..8] [-r rounds] [-g grid] [-k kernel] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "x:r:g:k:h")) != -1) {
    switch (c) {
    case 'x': xfers  = atoi(optarg); break;
    case 'r': rounds = atoi(optarg); break;
    case 'g': grid   = atoi(optarg); break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default: show_usage(); exit(-1);
    }
  }
  if (xfers < 1 || xfers > ROWMIX_MAX_XFERS || rounds < 1 || grid < 1) {
    show_usage();
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) vx_buffer_release(src_buffer);
    if (out_buffer) vx_buffer_release(out_buffer);
    if (kernel) vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue) vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

static uint32_t pattern(uint32_t i) {
  return (i + 1) * 2654435761u;
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  RT_CHECK(vx_device_open(0, &device));

  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t isa_flags = 0;
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  if ((isa_flags & VX_ISA_EXT_DXA) == 0) {
    std::cerr << "Error: DXA ISA extension is disabled." << std::endl;
    cleanup();
    return -1;
  }

  uint64_t num_threads;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));

  const uint32_t local_mem = xfers * kRegion;
  RT_CHECK(vx_check_occupancy(device, (uint32_t)num_threads, local_mem));

  const uint32_t tile_words = kTile0 * kTile1;
  const uint32_t src_words = kSize0 * kSize1;
  const uint32_t out_words = grid * rounds * xfers * tile_words;
  printf("transfers=%u rounds=%u grid=%u threads=%u source=%ux%u tile=%ux%u\n",
         xfers, rounds, grid, (unsigned)num_threads, kSize0, kSize1, kTile0, kTile1);

  uint64_t src_addr = 0;
  RT_CHECK(vx_buffer_create(device, src_words * sizeof(uint32_t), VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &src_addr));
  RT_CHECK(vx_buffer_create(device, out_words * sizeof(uint32_t), VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));

  std::vector<uint32_t> h_src(src_words);
  for (uint32_t i = 0; i < src_words; ++i) {
    h_src[i] = pattern(i);
  }
  RT_CHECK(vx_enqueue_write(queue, src_buffer, 0, h_src.data(), src_words * sizeof(uint32_t), 0, nullptr, nullptr));

  RT_CHECK(vortex::dxa::program_2d(device, 0, src_addr, kSize0, kSize1, kSize0 * sizeof(uint32_t),
                                   kTile0, kTile1, sizeof(uint32_t)));

  kernel_arg.xfers  = xfers;
  kernel_arg.rounds = rounds;
  kernel_arg.size0  = kSize0;
  kernel_arg.size1  = kSize1;
  kernel_arg.tile0  = kTile0;
  kernel_arg.tile1  = kTile1;
  kernel_arg.region = kRegion;

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
  li.lmem_size    = local_mem;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<uint32_t> h_out(out_words, 0);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_words * sizeof(uint32_t), 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  uint32_t errors = 0;
  for (uint32_t b = 0; b < grid; ++b) {
    for (uint32_t r = 0; r < rounds; ++r) {
      for (uint32_t x = 0; x < xfers; ++x) {
        const uint32_t c0 = rowmix_c0(b, r, x, kSize0, kTile0);
        const uint32_t c1 = rowmix_c1(b, r, x, kSize1);
        for (uint32_t k = 0; k < tile_words; ++k) {
          const uint32_t gx = c0 + k % kTile0;
          const uint32_t gy = c1 + k / kTile0;
          const uint32_t expected = (gx < kSize0 && gy < kSize1) ? h_src[gy * kSize0 + gx] : 0;
          const uint32_t got = h_out[((b * rounds + r) * xfers + x) * tile_words + k];
          if (got != expected) {
            if (errors < 10) {
              printf("*** error: cta=%u round=%u xfer=%u word=%u (x=%u, y=%u): got 0x%08x, expected 0x%08x\n",
                     b, r, x, k, gx, gy, got, expected);
            }
            ++errors;
          }
        }
      }
    }
  }

  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " errors!" << std::endl;
    std::cout << "FAILED!" << std::endl;
    return errors;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
