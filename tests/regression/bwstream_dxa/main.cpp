// Streaming-read bandwidth probe through the DXA.
//
// Each CTA (one warp) has the DXA copy `tiles` tiles of `tile_elems` floats
// into its local memory, `depth` of them outstanding at once, and sums them
// (or, with -c0, reads one word of each). The host never writes the buffer,
// so every line is a memory read that no cache holds (an upload would leave
// the lines in the last-level cache). The grid is `rounds` CTAs per CTA slot
// of the device (warps, or what local memory holds) unless -g gives the CTA
// count; the bytes read are grid * tiles * tile_elems * 4. Read the bandwidth
// from the perf report (--perf=7): memory reads over cycles.
//
// -M launches on the listed cores only: the others are masked off in the KMU
// and the grid is sized by the cores left. -T times each CTA and prints each
// core's bytes over the span from its first CTA's start to its last's end,
// and each enabled core's CTA count.
// -l <stride> times single copies instead: one CTA copies its tiles one at a
// time, `stride` tiles apart, each once cold and once warm.
//
// Unwritten memory reads as one value in the simulators, so the host check
// proves that every tile was delivered, not that it came from the right address.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <vortex2.h>
#include <dxa.h>
#include <VX_types.h>
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
const char* timed_file  = "kernel_timed.vxbin";
uint32_t tile_elems = 64;
uint32_t tiles   = 4;
uint32_t rounds  = 1;
uint32_t grid    = 0;
uint32_t depth   = 1;
uint32_t consume = 1;
uint32_t latency = 0;
const char* core_list = nullptr;
bool timing = false;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h probe_buffer = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_buffer_h timing_buffer = nullptr;
vx_buffer_h lat_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex DXA streaming-read bandwidth probe." << std::endl;
  std::cout << "Usage: [-e tile elements] [-n tiles per CTA] [-r rounds] [-g grid] "
               "[-d buffers 1..8] [-c 0|1 consume] [-l stride] [-M cores] [-T] [-k kernel] [-h]" << std::endl;
  std::cout << "  -l S: time single copies, S tiles apart. -M 0,2,8-11 (or 0+2+8-11): launch only on these"
               " cores. -T: per-core rates." << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "e:n:r:g:d:c:l:M:Tk:h")) != -1) {
    switch (c) {
    case 'e': tile_elems = atoi(optarg); break;
    case 'n': tiles   = atoi(optarg); break;
    case 'r': rounds  = atoi(optarg); break;
    case 'g': grid    = atoi(optarg); break;
    case 'd': depth   = atoi(optarg); break;
    case 'c': consume = atoi(optarg); break;
    case 'l': latency = atoi(optarg); break;
    case 'M': core_list = optarg; break;
    case 'T': timing = true; break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  const uint32_t max_depth = std::min<uint32_t>(BWSTREAM_DXA_MAX_DEPTH, VX_CFG_NUM_BARRIERS);
  if (tile_elems == 0 || tile_elems > 0xffff || tiles == 0 || rounds == 0 || depth < 1 || depth > max_depth
      || consume > 1 || (latency != 0 && timing)) {
    std::cerr << "tile elements must be 1..65535, tiles and rounds nonzero, depth 1.." << max_depth
              << ", consume 0 or 1; -l and -T exclude each other" << std::endl;
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
    if (timing_buffer) {
      vx_buffer_release(timing_buffer);
    }
    if (lat_buffer) {
      vx_buffer_release(lat_buffer);
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

// Cores named by a list such as "0,2,8-11" or "0+2+8-11" (the form that
// survives comma-separated environment lists); every core when there is none.
// False on a malformed list or a core the device does not have.
static bool parse_cores(const char* list, uint32_t num_cores, std::vector<bool>* on) {
  on->assign(num_cores, list == nullptr);
  for (const char* p = list; p != nullptr;) {
    char* e;
    unsigned long long a = strtoull(p, &e, 10);
    if (e == p) {
      return false;
    }
    unsigned long long b = a;
    if (*e == '-') {
      p = e + 1;
      b = strtoull(p, &e, 10);
      if (e == p) {
        return false;
      }
    }
    if (a > b || b >= num_cores) {
      return false;
    }
    for (uint64_t c = a; c <= b; ++c) {
      (*on)[c] = true;
    }
    if (*e == 0) {
      break;
    }
    if (*e != ',' && *e != '+') {
      return false;
    }
    p = e + 1;
  }
  return true;
}

// Mask off the cores not in `on`; the mask stays with the device for later launches.
static void program_core_mask(const std::vector<bool>& on) {
  for (uint32_t w = 0; w < VX_DCR_KMU_CORE_DISABLE_WORDS; ++w) {
    uint32_t bits = 0;
    for (uint32_t b = 0; b < 32; ++b) {
      uint32_t c = 32 * w + b;
      if (c < on.size() && !on[c]) {
        bits |= 1u << b;
      }
    }
    RT_CHECK(vx_enqueue_dcr_write(queue, VX_DCR_KMU_CORE_DISABLE0 + w, bits, 0, nullptr, nullptr));
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
  std::vector<bool> cores_on;
  if (!parse_cores(core_list, (uint32_t)num_cores, &cores_on)
      || (core_list != nullptr && num_cores > 32 * VX_DCR_KMU_CORE_DISABLE_WORDS)) {
    std::cerr << "Error: -M must list cores 0.." << (num_cores - 1) << std::endl;
    cleanup();
    return -1;
  }
  uint32_t active_cores = (uint32_t)std::count(cores_on.begin(), cores_on.end(), true);
  if (active_cores == 0) {
    // With every core masked off the KMU never dispatches.
    std::cerr << "Error: -M leaves no core to launch on" << std::endl;
    cleanup();
    return -1;
  }
  if (core_list != nullptr) {
    program_core_mask(cores_on);
  }
  if (grid == 0) {
    grid = latency ? 1 : (uint32_t)(active_cores * slots) * rounds;
  }

  uint64_t total_threads = (uint64_t)grid * num_threads;
  uint64_t total_elems = (uint64_t)grid * tiles * tile_elems * (latency ? latency : 1);
  uint64_t src_size = total_elems * sizeof(float);
  uint64_t out_size = total_threads * sizeof(float);
  if (total_elems > 0xffffffffull) {
    std::cerr << "the source exceeds the descriptor's 32-bit element count" << std::endl;
    cleanup();
    return -1;
  }
  printf("cores=%u warps=%u threads=%u slots=%u grid=%u tile_elems=%u tiles=%u depth=%u consume=%u "
         "pattern=dxa unroll=%u bytes=%llu active_cores=%u latency=%u\n",
         (unsigned)num_cores, (unsigned)num_warps, (unsigned)num_threads, (unsigned)slots, grid,
         tile_elems, tiles, depth, consume, depth, (unsigned long long)src_size, active_cores, latency);

  uint64_t src_addr = 0;
  RT_CHECK(vx_buffer_create(device, src_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &src_addr));
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.tile_elems = tile_elems;
  kernel_arg.tiles      = tiles;
  kernel_arg.depth      = depth;
  kernel_arg.consume    = consume;

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

  RT_CHECK(vx_module_load_file(device, (timing || latency) ? timed_file : kernel_file, &module_));
  timed_arg_t timed_arg = {};
  latency_arg_t latency_arg = {};
  const void* args = &kernel_arg;
  size_t args_size = sizeof(kernel_arg);
  if (timing) {
    RT_CHECK(vx_module_get_kernel(module_, "kernel_timed", &kernel));
    RT_CHECK(vx_buffer_create(device, 3 * (uint64_t)grid * sizeof(uint64_t), VX_MEM_WRITE, &timing_buffer));
    RT_CHECK(vx_buffer_address(timing_buffer, &timed_arg.timing_addr));
    timed_arg.stream = kernel_arg;
    args = &timed_arg;
    args_size = sizeof(timed_arg);
  } else if (latency) {
    RT_CHECK(vx_module_get_kernel(module_, "kernel_latency", &kernel));
    RT_CHECK(vx_buffer_create(device, 2 * (uint64_t)tiles * sizeof(uint64_t), VX_MEM_WRITE, &lat_buffer));
    RT_CHECK(vx_buffer_address(lat_buffer, &latency_arg.lat_addr));
    latency_arg.stream = kernel_arg;
    latency_arg.stride = latency;
    args = &latency_arg;
    args_size = sizeof(latency_arg);
  } else {
    RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));
  }

  vx_event_h launch_ev = nullptr, read_ev = nullptr;
  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = args;
  li.args_size    = args_size;
  li.ndim         = 1;
  li.grid_dim[0]  = grid;
  li.block_dim[0] = (uint32_t)num_threads;
  li.lmem_size    = local_mem;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<float> h_out(total_threads, 0.0f);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  // With -M every CTA must report one of the listed cores. Within one wave a
  // core takes its CTAs without finishing any, so the KMU alone places them
  // and the enabled cores' counts must differ by one at most; past one wave a
  // core that drains sooner rightly takes more.
  uint64_t misplaced = 0;
  bool uneven = false;
  if (timing) {
    std::vector<uint64_t> tm(3 * (uint64_t)grid, 0);
    vx_event_h tm_ev = nullptr;
    RT_CHECK(vx_enqueue_read(queue, tm.data(), timing_buffer, 0, tm.size() * sizeof(uint64_t), 0, nullptr, &tm_ev));
    RT_CHECK(vx_event_wait_value(tm_ev, 1, VX_TIMEOUT_INFINITE));
    vx_event_release(tm_ev);
    std::vector<uint64_t> first(num_cores, ~0ull), last(num_cores, 0), bytes(num_cores, 0), ctas(num_cores, 0);
    for (uint64_t g = 0; g < grid; ++g) {
      uint64_t c = tm[3 * g];
      if (c >= num_cores || !cores_on[c]) {
        if (misplaced < 10) {
          printf("*** error: CTA %llu ran on core %llu, not in -M\n", (unsigned long long)g,
                 (unsigned long long)c);
        }
        ++misplaced;
        continue;
      }
      first[c] = std::min(first[c], tm[3 * g + 1]);
      last[c] = std::max(last[c], tm[3 * g + 2]);
      bytes[c] += (uint64_t)tiles * tile_elems * sizeof(float);
      ++ctas[c];
    }
    for (uint64_t c = 0; c < num_cores; ++c) {
      if (bytes[c] != 0) {
        printf("core_rate: core=%llu bytes=%llu start=%llu end=%llu B_per_cycle=%.2f\n",
               (unsigned long long)c, (unsigned long long)bytes[c], (unsigned long long)first[c],
               (unsigned long long)last[c], double(bytes[c]) / double(last[c] - first[c]));
      }
    }
    uint64_t min_ctas = ~0ull, max_ctas = 0;
    for (uint64_t c = 0; c < num_cores; ++c) {
      if (cores_on[c]) {
        printf("core_ctas: core=%llu ctas=%llu\n", (unsigned long long)c, (unsigned long long)ctas[c]);
        min_ctas = std::min(min_ctas, ctas[c]);
        max_ctas = std::max(max_ctas, ctas[c]);
      }
    }
    uneven = (grid % active_cores == 0) && (grid <= active_cores * slots) && (max_ctas - min_ctas > 1);
  }

  if (latency) {
    std::vector<uint64_t> lat(2 * (uint64_t)tiles, 0);
    vx_event_h lat_ev = nullptr;
    RT_CHECK(vx_enqueue_read(queue, lat.data(), lat_buffer, 0, lat.size() * sizeof(uint64_t), 0, nullptr, &lat_ev));
    RT_CHECK(vx_event_wait_value(lat_ev, 1, VX_TIMEOUT_INFINITE));
    vx_event_release(lat_ev);
    for (uint32_t i = 0; i < tiles; ++i) {
      printf("latency: tile=%u bytes=%u cold=%llu warm=%llu\n", i, tile_elems * 4,
             (unsigned long long)lat[2 * i], (unsigned long long)lat[2 * i + 1]);
    }
  }

  // Every element reads as `fill`: redo the kernel's accumulation in its order.
  volatile float sum = 0.0f;
  const uint64_t adds = latency ? 2 : (consume ? tile_elems / num_threads : 1);
  for (uint64_t i = 0; i < (uint64_t)tiles * adds; ++i) {
    sum = sum + fill;
  }

  uint64_t errors = 0;
  for (uint64_t g = 0; g < total_threads; ++g) {
    float expected = sum + (float)(g + 1);
    if (h_out[g] != expected) {
      if (errors < 10) {
        printf("*** error: [%llu] expected=%f, actual=%f\n", (unsigned long long)g,
               expected, h_out[g]);
      }
      ++errors;
    }
  }

  cleanup();

  if (misplaced != 0) {
    std::cout << "Found " << misplaced << " CTAs on masked cores!" << std::endl;
    return 1;
  }
  if (uneven) {
    std::cout << "The enabled cores' CTA counts differ by more than one!" << std::endl;
    return 1;
  }
  if (errors != 0) {
    std::cout << "Found " << errors << " errors!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
