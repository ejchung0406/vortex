#include "common.h"
#include <algorithm>
#include <iostream>
#include <rvfloats.h>
#include <string.h>
#include <tensor_cfg.h>
#include <type_traits>
#include <unistd.h>
#include <util.h>
#include <vector>
#include <vortex2.h>

#define MAX_ERRORS 100

#define RT_CHECK(_expr)                                      \
  do {                                                       \
    int _ret = _expr;                                        \
    if (0 == _ret)                                           \
      break;                                                 \
    printf("Error: '%s' returned %d!\n", #_expr, (int)_ret); \
    cleanup();                                               \
    exit(-1);                                                \
  } while (false)

using namespace vortex;
namespace vt = tensor;

static_assert(std::is_same<vt::ITYPE, vt::fp16>::value || std::is_same<vt::ITYPE, vt::bf16>::value,
              "tcu_roof generates fp16 or bf16 operands");
static_assert(std::is_same<vt::OTYPE, vt::fp32>::value, "tcu_roof accumulates in fp32");

using wm_cfg = vt::wmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE>;
using wg_cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE>;

using itype_t = vt::ITYPE::dtype;
using otype_t = vt::OTYPE::dtype;

// Operands are small integers, so every product and every partial sum is an
// integer below 2^24 and the fp32 accumulation is exact.
static constexpr int kMaxA = 2;
static constexpr int kMaxB = 3;
static int value_a(uint32_t cta, uint32_t m, uint32_t k) { return (m + 2 * k + cta) % (kMaxA + 1); }
static int value_b(uint32_t cta, uint32_t k, uint32_t n) { return (3 * k + n + cta) % (kMaxB + 1); }

static itype_t to_itype(int value) {
  auto bits = bit_cast<uint32_t>(float(value));
  if constexpr (std::is_same<vt::ITYPE, vt::fp16>::value) {
    return rv_ftoh_s(bits, 0, nullptr);
  } else {
    return rv_ftob_s(bits, 0, nullptr);
  }
}

const char *kernel_file = "kernel.vxbin";

uint32_t mode  = MODE_WMMA;
uint32_t iters = 64;
uint32_t acc   = 1;
uint32_t grid  = 0; // CTAs; 0 fills every warp of every core
uint32_t warps = 0; // warps per CTA; 0 is 1 for wmma and the issue width for wgmma

vx_device_h device = nullptr;
vx_buffer_h A_buffer = nullptr;
vx_buffer_h B_buffer = nullptr;
vx_buffer_h C_buffer = nullptr;
#ifdef PROFILE_ENABLE
vx_buffer_h cycles_buffer = nullptr;
#endif
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex tensor core roof." << std::endl;
  std::cout << "Usage: [-m mode: 0=wmma 1=wgmma] [-i iters] [-g grid] [-a accumulators: 1, 2 or " << MAX_ACC
            << "] [-w warps per CTA (wgmma)] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "m:i:g:a:w:h")) != -1) {
    switch (c) {
    case 'm':
      mode = atoi(optarg);
      break;
    case 'i':
      iters = atoi(optarg);
      break;
    case 'g':
      grid = atoi(optarg);
      break;
    case 'a':
      acc = atoi(optarg);
      break;
    case 'w':
      warps = atoi(optarg);
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
    if (A_buffer) vx_buffer_release(A_buffer);
    if (B_buffer) vx_buffer_release(B_buffer);
    if (C_buffer) vx_buffer_release(C_buffer);
#ifdef PROFILE_ENABLE
    if (cycles_buffer) vx_buffer_release(cycles_buffer);
#endif
    if (kernel)  vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue)   vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  if (mode != MODE_WMMA && mode != MODE_WGMMA) {
    std::cout << "Error: mode must be 0 (wmma) or 1 (wgmma)!" << std::endl;
    return -1;
  }
#ifndef VX_CFG_TCU_WGMMA_ENABLE
  if (mode == MODE_WGMMA) {
    std::cout << "Error: wgmma needs VX_CFG_TCU_WGMMA_ENABLE!" << std::endl;
    return -1;
  }
#endif
  if (acc != 1 && acc != 2 && acc != MAX_ACC) {
    std::cout << "Error: accumulators must be 1, 2 or " << MAX_ACC << "!" << std::endl;
    return -1;
  }
  if (iters == 0 || (iters % acc) != 0) {
    std::cout << "Error: iters must be a positive multiple of the accumulators!" << std::endl;
    return -1;
  }

  std::cout << "open device connection" << std::endl;
  RT_CHECK(vx_device_open(0, &device));

  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t isa_flags;
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  if ((isa_flags & VX_ISA_EXT_TCU) == 0) {
    std::cout << "TCU extension not supported!" << std::endl;
    cleanup();
    return -1;
  }

  uint64_t NT, num_warps, num_cores, issue_width;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &NT));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_ISSUE_WIDTH, &issue_width));
  if (NT != VX_CFG_NUM_THREADS) {
    std::cout << "Error: device warp size (" << NT << ") must match VX_CFG_NUM_THREADS=" << VX_CFG_NUM_THREADS << "!" << std::endl;
    cleanup();
    return -1;
  }

  // per-CTA operand shapes
  if (mode == MODE_WMMA) {
    warps = 1;
  } else if (warps == 0) {
    warps = std::min(issue_width, num_warps);
  }
  if (warps > num_warps) {
    std::cout << "Error: " << warps << " warps per CTA exceed the core's " << num_warps << "!" << std::endl;
    cleanup();
    return -1;
  }
  if (grid == 0) {
    grid = num_cores * num_warps / warps;
  }
  uint32_t warpM = (mode == MODE_WMMA) ? wm_cfg::tileM : wg_cfg::xtileM;
  uint32_t tileN = (mode == MODE_WMMA) ? wm_cfg::tileN : wg_cfg::xtileN;
  uint32_t tileK = (mode == MODE_WMMA) ? wm_cfg::tileK : wg_cfg::tileK;
  uint32_t ctaM  = warps * warpM;
  uint32_t block_threads = warps * NT;
  if (mode == MODE_WGMMA && ((ctaM * tileK) % block_threads || (tileK * tileN) % block_threads)) {
    std::cout << "Error: " << warps << " warps cannot stage the operands evenly!" << std::endl;
    cleanup();
    return -1;
  }

  uint32_t per_acc = iters / acc; // multiplies into each accumulator
  if (uint64_t(per_acc) * tileK * kMaxA * kMaxB >= (1u << 24)) {
    std::cout << "Error: too many iterations for an exact fp32 sum!" << std::endl;
    cleanup();
    return -1;
  }

  size_t sizeA = size_t(grid) * ctaM * tileK;
  size_t sizeB = size_t(grid) * tileK * tileN;
  size_t tileC = size_t(ctaM) * tileN;
  size_t sizeC = size_t(grid) * MAX_ACC * tileC;
  uint32_t num_slots = grid * warps;

  printf("TCU_GEOMETRY: mode=%s itype=%s otype=%s tileM=%u tileN=%u tileK=%u warps_per_cta=%u grid=%u iters=%u acc=%u\n",
         (mode == MODE_WMMA) ? "wmma" : "wgmma", vt::ITYPE::name, vt::OTYPE::name,
         warpM, tileN, tileK, warps, grid, iters, acc);

  kernel_arg.mode  = mode;
  kernel_arg.iters = iters;
  kernel_arg.acc   = acc;

  std::cout << "allocate device memory" << std::endl;
  RT_CHECK(vx_buffer_create(device, sizeA * sizeof(itype_t), VX_MEM_READ, &A_buffer));
  RT_CHECK(vx_buffer_address(A_buffer, &kernel_arg.A_addr));
  RT_CHECK(vx_buffer_create(device, sizeB * sizeof(itype_t), VX_MEM_READ, &B_buffer));
  RT_CHECK(vx_buffer_address(B_buffer, &kernel_arg.B_addr));
  RT_CHECK(vx_buffer_create(device, sizeC * sizeof(otype_t), VX_MEM_WRITE, &C_buffer));
  RT_CHECK(vx_buffer_address(C_buffer, &kernel_arg.C_addr));
#ifdef PROFILE_ENABLE
  RT_CHECK(vx_buffer_create(device, num_slots * 2 * sizeof(uint64_t), VX_MEM_WRITE, &cycles_buffer));
  RT_CHECK(vx_buffer_address(cycles_buffer, &kernel_arg.cycles_addr));
#endif

  // A is row-major; B is column-major for wmma and row-major for wgmma
  std::vector<itype_t> h_A(sizeA);
  std::vector<itype_t> h_B(sizeB);
  for (uint32_t g = 0; g < grid; ++g) {
    for (uint32_t m = 0; m < ctaM; ++m) {
      for (uint32_t k = 0; k < tileK; ++k) {
        h_A[(size_t(g) * ctaM + m) * tileK + k] = to_itype(value_a(g, m, k));
      }
    }
    for (uint32_t k = 0; k < tileK; ++k) {
      for (uint32_t n = 0; n < tileN; ++n) {
        size_t off = (mode == MODE_WMMA) ? (n * tileK + k) : (k * tileN + n);
        h_B[size_t(g) * tileK * tileN + off] = to_itype(value_b(g, k, n));
      }
    }
  }

  std::cout << "upload source buffers" << std::endl;
  RT_CHECK(vx_enqueue_write(queue, A_buffer, 0, h_A.data(), sizeA * sizeof(itype_t), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, B_buffer, 0, h_B.data(), sizeB * sizeof(itype_t), 0, nullptr, nullptr));

  std::cout << "load kernel module" << std::endl;
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 2;
  li.grid_dim[0]  = grid;
  li.grid_dim[1]  = 1;
  li.block_dim[0] = block_threads;
  li.block_dim[1] = 1;
  if (mode == MODE_WGMMA) {
    li.lmem_size = (ctaM * tileK + tileK * tileN) * sizeof(itype_t);
  }

  std::vector<otype_t> h_C(sizeC);

  std::cout << "launch kernel" << std::endl;
  vx_event_h launch_ev = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::cout << "download destination buffer" << std::endl;
  vx_event_h read_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_C.data(), C_buffer, 0, sizeC * sizeof(otype_t), 1, &launch_ev, &read_ev));
#ifdef PROFILE_ENABLE
  std::vector<uint64_t> h_cycles(num_slots * 2);
  vx_event_h cyc_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_cycles.data(), cycles_buffer, 0,
                           h_cycles.size() * sizeof(uint64_t), 1, &read_ev, &cyc_ev));
  RT_CHECK(vx_event_wait_value(cyc_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(cyc_ev);
#else
  RT_CHECK(vx_event_wait_value(read_ev, 1, VX_TIMEOUT_INFINITE));
#endif
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

#ifdef PROFILE_ENABLE
  {
    // max: the longest loop of any warp. span: from the first warp entering
    // its loop to the last one leaving, which is what all the multiplies took.
    uint64_t max_cycles = 0, first = UINT64_MAX, last = 0;
    for (uint32_t s = 0; s < num_slots; ++s) {
      uint64_t t0 = h_cycles[2 * s], t1 = h_cycles[2 * s + 1];
      max_cycles = std::max(max_cycles, t1 - t0);
      first = std::min(first, t0);
      last = std::max(last, t1);
    }
    uint64_t span = last - first;
    uint64_t fmas = uint64_t(num_slots) * iters * warpM * tileN * tileK;
    printf("TCU_CYCLES: max=%lu span=%lu (across %u warps)\n", max_cycles, span, num_slots);
    printf("TCU_FMAS: %lu\n", fmas);
    printf("TCU_FLOPS_PER_CYCLE: %.1f (max) %.1f (span)\n",
           2.0 * fmas / max_cycles, 2.0 * fmas / span);
  }
#endif

  std::cout << "verify result" << std::endl;
  int errors = 0;
  for (uint32_t g = 0; g < grid; ++g) {
    for (uint32_t m = 0; m < ctaM; ++m) {
      for (uint32_t n = 0; n < tileN; ++n) {
        int dot = 0;
        for (uint32_t k = 0; k < tileK; ++k) {
          dot += value_a(g, m, k) * value_b(g, k, n);
        }
        for (uint32_t a = 0; a < MAX_ACC; ++a) {
          otype_t expected = (a < acc) ? float(per_acc) * float(dot) : 0.0f;
          size_t idx = (size_t(g) * MAX_ACC + a) * tileC + m * tileN + n;
          if (h_C[idx] != expected) {
            if (errors < MAX_ERRORS) {
              printf("*** error: cta=%u acc=%u [%u,%u] expected=%f, actual=%f\n", g, a, m, n, expected, h_C[idx]);
            }
            ++errors;
          }
        }
      }
    }
  }

  std::cout << "cleanup" << std::endl;
  cleanup();

  if (errors != 0) {
    std::cout << "Found " << std::dec << errors << " / " << sizeC << " errors!" << std::endl;
    std::cout << "FAILED!" << std::endl;
    return 1;
  }

  std::cout << "PASSED!" << std::endl;

  return 0;
}
