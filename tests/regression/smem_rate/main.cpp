// Issue rate of one warp, and of W warps sharing one core, by instruction
// class. One CTA of W warps runs `iters` iterations of one inline-asm block
// (common.h lists the classes); each warp reads mcycle around its loop.
// Prints the slowest warp's own loop time and the core span (first warp's
// start to last warp's end) per iteration, and checks every thread's final
// registers against the host's evaluation of the class.
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

static const char* kNames[WR_NKINDS] = {
  "int_dep", "int_ind4", "int_ind8", "fma_dep", "fma_ind4", "fma_ind8",
  "br_every1", "br_every4", "br_every16", "lds_own", "lds_chase", "sts_own", "lds_nouse", "sts_own4", "lds_nouse4", "sts_own8", "csr", "splitjoin", "bar", "div", "lds_own4", "sts_same", "mix"};

const char* kernel_file = "kernel.vxbin";
uint32_t kind  = 0;
uint32_t warps = 1;
uint32_t iters = 64;
uint32_t fair  = 0;

vx_device_h device = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_queue_h  queue   = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel  = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Usage: [-k kind 0.." << WR_NKINDS - 1 << "] [-w warps] [-i iterations] [-f percent: mix fails when one group's time is above that share of the other's] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "k:w:i:f:h")) != -1) {
    switch (c) {
    case 'k': kind  = atoi(optarg); break;
    case 'w': warps = atoi(optarg); break;
    case 'i': iters = atoi(optarg); break;
    case 'f': fair  = atoi(optarg); break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
  if (kind >= WR_NKINDS || warps == 0 || iters == 0) {
    show_usage();
    exit(-1);
  }
}

// What kernel_main leaves in out[] for thread t: the registers of its class
// after `iters` iterations, folded as the kernel folds them.
static uint64_t reference(uint32_t kind, uint32_t iters, uint32_t t, uint64_t word_size, bool mix_store) {
  uint64_t y = 3 + (kind & 1), n = iters;
  uint64_t a[8];
  float f[8];
  for (uint32_t i = 0; i < 8; ++i) {
    a[i] = t + i;
    f[i] = (1.f + t) + i;
  }
  auto fma = [&](uint32_t chains, uint64_t reps) {
    for (uint32_t c = 0; c < chains; ++c)
      for (uint64_t r = 0; r < reps; ++r)
        f[c] = std::fmaf(f[c], 0.999f, 1e-3f);
  };
  auto add = [&](uint32_t first, uint32_t last, uint64_t reps) {
    for (uint32_t i = first; i <= last; ++i)
      a[i] += reps * y;
  };
  switch (kind) {
  case WR_INT_DEP:   add(0, 0, 128 * n); break;
  case WR_INT_IND4:  add(0, 3, 32 * n); break;
  case WR_INT_IND8:  add(0, 7, 16 * n); break;
  case WR_FMA_DEP:   fma(1, 128 * n); break;
  case WR_FMA_IND4:  fma(4, 32 * n); break;
  case WR_FMA_IND8:  fma(8, 16 * n); break;
  case WR_BR4:       add(1, 3, 32 * n); break;
  case WR_BR16:      add(0, 2, 32 * n); add(3, 3, 24 * n); break;
  case WR_LDS_OWN:
  case WR_LDS_OWN4:
  case WR_LDS_NOUSE4:
  case WR_LDS_NOUSE: for (auto& v : a) v = 0; break;
  case WR_LDS_CHASE: a[0] = 0; break;
  case WR_MIX:
    if (!mix_store) { for (auto& v : a) v = 0; }
    break;
  case WR_DIV:       a[0] = t % y; break;
  default:           break; // WR_BR1, WR_STS_OWN: no register changes
  }
  float fv = f[0];
  for (uint32_t i = 1; i < 8; ++i)
    fv += f[i];
  uint32_t fbits;
  memcpy(&fbits, &fv, sizeof(fbits));
  uint64_t fold = 0;
  for (uint32_t i = 0; i < 8; ++i)
    fold ^= a[i] << i;
  fold += fbits;
  return (word_size == 8) ? fold : (fold & 0xffffffffull);
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
  uint64_t out_size = (warps * 2 + nthreads + 1) * sizeof(uint64_t);
  RT_CHECK(vx_buffer_create(device, out_size, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  kernel_arg.kind  = kind;
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
  li.lmem_size    = warps * (uint32_t)num_threads * sizeof(uint64_t);
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::vector<uint64_t> h_out(warps * 2 + nthreads + 1, 0);
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_size, 1, &launch_ev, &read_ev));
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  uint64_t lo = UINT64_MAX, hi = 0, worst = 0;
  uint64_t worst_store = 0, worst_load = 0;
  int errors = 0;
  for (uint32_t w = 0; w < warps; ++w) {
    uint64_t c0 = h_out[2 * w], c1 = h_out[2 * w + 1];
    if (c1 <= c0) {
      printf("*** error: warp %u start=%llu end=%llu\n", w, (unsigned long long)c0, (unsigned long long)c1);
      ++errors;
      continue;
    }
    lo = std::min(lo, c0);
    hi = std::max(hi, c1);
    worst = std::max(worst, c1 - c0);
    if ((w & 3) == 0) worst_store = std::max(worst_store, c1 - c0);
    else              worst_load  = std::max(worst_load, c1 - c0);
  }
  uint64_t hash = 1469598103934665603ull;
  uint64_t word_size = h_out[warps * 2 + nthreads];
  for (uint64_t i = 0; i < nthreads; ++i) {
    uint64_t got = h_out[warps * 2 + i], ref = reference(kind, iters, i, word_size, ((i / num_threads) & 3) == 0);
    if (got != ref) {
      if (errors < 16) {
        printf("*** error: thread %llu registers=%016llx, expected=%016llx\n", (unsigned long long)i,
               (unsigned long long)got, (unsigned long long)ref);
      }
      ++errors;
    }
    hash = (hash ^ got) * 1099511628211ull;
  }
  if (errors == 0) {
    printf("kind=%s W=%u iters=%u cyc_per_iter_warp=%.2f cyc_per_iter_sm=%.2f hash=%016llx\n", kNames[kind], warps, iters,
           (double)worst / iters, (double)(hi - lo) / iters, (unsigned long long)hash);
  }
  if (errors == 0 && kind == WR_MIX) {
    printf("mix: cyc_per_iter store_warps=%.2f load_warps=%.2f\n", (double)worst_store / iters, (double)worst_load / iters);
    uint64_t hi_t = std::max(worst_store, worst_load), lo_t = std::min(worst_store, worst_load);
    if (fair != 0 && hi_t * 100 > lo_t * fair) {
      printf("*** error: the slower group takes %.0f%% of the other's time, above %u%%\n", 100.0 * hi_t / lo_t, fair);
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
