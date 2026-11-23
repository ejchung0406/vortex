// DXA copies beside a plain-load stream on one device.
//
// CTAs [0, x) are bwstream_dxa's: each has the DXA copy `tiles` tiles of
// `tile_elems` floats into its local memory, `depth` outstanding at once. The
// other L CTAs stream plain global loads (four per thread at a time, `s` per
// thread) from a buffer of their own. Every CTA records its first and last
// cycle; the host prints each role's span and bytes per clock, so a run with
// both roles can be set against a run with one (-x0 or -L0). Neither buffer is
// written by the host, so every line is a memory read and every value is the
// device's fill value.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <vortex2.h>
#include <dxa.h>
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
uint32_t tile_elems = 64;
uint32_t tiles   = 4;
uint32_t rounds  = 1;
uint32_t grid    = 0;
uint32_t depth   = 1;
uint32_t consume = 1;
int32_t  dxa_ctas = -1;
uint32_t load_ctas = 0;
uint32_t load_steps = 256;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h probe_buffer = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_buffer_h load_buffer = nullptr;
vx_buffer_h time_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex DXA streaming-read bandwidth probe." << std::endl;
  std::cout << "Usage: [-e tile elements] [-n tiles per CTA] [-r rounds] [-g grid] "
               "[-d buffers 1..8] [-c 0|1 consume] [-x copying CTAs] [-L load CTAs] [-s loads per thread] "
               "[-k kernel] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "e:n:r:g:d:c:x:L:s:k:h")) != -1) {
    switch (c) {
    case 'e': tile_elems = atoi(optarg); break;
    case 'n': tiles   = atoi(optarg); break;
    case 'r': rounds  = atoi(optarg); break;
    case 'g': grid    = atoi(optarg); break;
    case 'd': depth   = atoi(optarg); break;
    case 'c': consume = atoi(optarg); break;
    case 'x': dxa_ctas = atoi(optarg); break;
    case 'L': load_ctas = atoi(optarg); break;
    case 's': load_steps = atoi(optarg); break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  const uint32_t max_depth = std::min<uint32_t>(BWSTREAM_DXA_MAX_DEPTH, VX_CFG_NUM_BARRIERS);
  if (tile_elems == 0 || tile_elems > 0xffff || tiles == 0 || rounds == 0 || depth < 1 || depth > max_depth
      || consume > 1 || load_steps == 0 || (load_steps % 4) != 0) {
    std::cerr << "tile elements must be 1..65535, tiles and rounds nonzero, depth 1.." << max_depth
              << ", consume 0 or 1, loads per thread a nonzero multiple of 4" << std::endl;
    exit(-1);
  }
}

void cleanup() {
  if (device) {
    if (src_buffer) {
      vx_buffer_release(src_buffer);
    }
    if (probe_buffer) {
      vx_buffer_release(probe_buffer);
    }
    if (out_buffer) {
      vx_buffer_release(out_buffer);
    }
    if (load_buffer) {
      vx_buffer_release(load_buffer);
    }
    if (time_buffer) {
      vx_buffer_release(time_buffer);
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
  if ((isa_flags & VX_ISA_EXT_DXA) == 0) {
    std::cerr << "Error: DXA ISA extension is disabled." << std::endl;
    cleanup();
    return -1;
  }

  uint64_t num_threads, num_warps, num_cores, lmem_bytes;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &lmem_bytes));
  if (tile_elems % num_threads != 0) {
    std::cerr << "tile elements must be a multiple of the warp's threads" << std::endl;
    cleanup();
    return -1;
  }

  uint32_t local_mem = depth * tile_elems * sizeof(float);
  RT_CHECK(vx_check_occupancy(device, (uint32_t)num_threads, local_mem));
  uint64_t slots = std::min<uint64_t>(num_warps, lmem_bytes / local_mem);
  if (dxa_ctas < 0) {
    dxa_ctas = (grid != 0) ? (int32_t)grid : (int32_t)((num_cores * slots) * rounds - load_ctas);
  }
  grid = (uint32_t)dxa_ctas + load_ctas;
  if (grid == 0 || (uint64_t)grid > num_cores * slots) {
    std::cerr << "copying and load CTAs must fit the device at once (" << num_cores * slots << " slots)" << std::endl;
    cleanup();
    return -1;
  }

  uint64_t total_threads = (uint64_t)grid * num_threads;
  uint64_t total_elems = std::max<uint64_t>(1, (uint64_t)dxa_ctas * tiles * tile_elems);
  uint64_t load_elems = std::max<uint64_t>(1, (uint64_t)load_ctas * load_steps * num_threads);
  uint64_t src_size = total_elems * sizeof(float);
  uint64_t out_size = total_threads * sizeof(float);
  if (total_elems > 0xffffffffull) {
    std::cerr << "the source exceeds the descriptor's 32-bit element count" << std::endl;
    cleanup();
    return -1;
  }
  printf("cores=%u warps=%u threads=%u slots=%u copying=%d tile_elems=%u tiles=%u depth=%u consume=%u "
         "load_ctas=%u load_steps=%u\n",
         (unsigned)num_cores, (unsigned)num_warps, (unsigned)num_threads, (unsigned)slots, dxa_ctas,
         tile_elems, tiles, depth, consume, load_ctas, load_steps);

  uint64_t src_addr = 0;
  RT_CHECK(vx_buffer_create(device, src_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &src_addr));
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.tile_elems = tile_elems;
  kernel_arg.tiles      = tiles;
  kernel_arg.depth      = depth;
  kernel_arg.consume    = consume;
  kernel_arg.dxa_ctas   = (uint32_t)dxa_ctas;
  kernel_arg.load_steps = load_steps;
  RT_CHECK(vx_buffer_create(device, load_elems * sizeof(float), VX_MEM_READ, &load_buffer));
  RT_CHECK(vx_buffer_address(load_buffer, &kernel_arg.load_addr));
  RT_CHECK(vx_buffer_create(device, (uint64_t)grid * 2 * sizeof(uint64_t), VX_MEM_WRITE, &time_buffer));
  RT_CHECK(vx_buffer_address(time_buffer, &kernel_arg.time_addr));

  // Unwritten device memory reads as one fill value; fetch it from a buffer of
  // its own to predict the sums, so no line of the source enters a cache.
  float fill = 0.0f;
  {
    vx_event_h fill_ev = nullptr;
    RT_CHECK(vx_buffer_create(device, sizeof(fill), VX_MEM_READ, &probe_buffer));
    RT_CHECK(vx_enqueue_read(queue, &fill, probe_buffer, 0, sizeof(fill), 0, nullptr, &fill_ev));
    RT_CHECK(vx_event_wait_value(fill_ev, 1, VX_TIMEOUT_INFINITE));
    vx_event_release(fill_ev);
  }

  RT_CHECK(vortex::dxa::program_1d(device, 0, src_addr, (uint32_t)total_elems, tile_elems,
                                   sizeof(float)));

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

  std::vector<float> h_out(total_threads, 0.0f);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  std::vector<uint64_t> h_time((uint64_t)grid * 2, 0);
  RT_CHECK(vx_enqueue_read(queue, h_time.data(), time_buffer, 0, h_time.size() * sizeof(uint64_t), 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  // span of a role: its first CTA's start to its last CTA's end
  auto report = [&](const char* role, uint32_t first, uint32_t count, uint64_t bytes) {
    if (count == 0) {
      return;
    }
    uint64_t t0 = UINT64_MAX, t1 = 0;
    for (uint32_t b = first; b < first + count; ++b) {
      t0 = std::min(t0, h_time[2 * b]);
      t1 = std::max(t1, h_time[2 * b + 1]);
    }
    printf("ROLE %s: ctas=%u span=%llu bytes=%llu bytes_per_clock=%.2f\n", role, count,
           (unsigned long long)(t1 - t0), (unsigned long long)bytes, (double)bytes / (double)(t1 - t0));
  };
  report("copy", 0, (uint32_t)dxa_ctas, (uint64_t)dxa_ctas * tiles * tile_elems * sizeof(float));
  report("load", (uint32_t)dxa_ctas, load_ctas, (uint64_t)load_ctas * load_steps * num_threads * sizeof(float));

  // Every element reads as `fill`: redo the kernel's accumulation in its order.
  volatile float sum = 0.0f;
  for (uint64_t i = 0; i < (uint64_t)tiles * (consume ? tile_elems / num_threads : 1); ++i) {
    sum = sum + fill;
  }

  // load CTAs: four running sums of load_steps / 4 values each, paired
  volatile float part = 0.0f;
  for (uint32_t i = 0; i < load_steps / 4; ++i) {
    part = part + fill;
  }
  volatile float half = part + part;
  volatile float load_sum = half + half;

  uint64_t errors = 0;
  for (uint64_t g = 0; g < total_threads; ++g) {
    float expected = ((g / num_threads) < (uint64_t)dxa_ctas ? sum : load_sum) + (float)(g + 1);
    if (h_out[g] != expected) {
      if (errors < 10) {
        printf("*** error: [%llu] expected=%f, actual=%f\n", (unsigned long long)g,
               expected, h_out[g]);
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
