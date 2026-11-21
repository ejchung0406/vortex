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
              "tmem_roof generates fp16 or bf16 operands");
static_assert(std::is_same<vt::OTYPE, vt::fp32>::value, "tmem_roof accumulates in fp32");

using cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, ROOF_NRC>;
using itype_t = vt::ITYPE::dtype;

// Operands are small integers, so every product and every partial sum is an
// integer below 2^24 and the fp32 accumulation is exact.
static constexpr int kMaxA = 2;
static constexpr int kMaxB = 3;

static int value_a(uint32_t cta, uint32_t s, uint32_t m, uint32_t k) { return (m + 2 * k + s + cta) % (kMaxA + 1); }
static int value_b(uint32_t cta, uint32_t s, uint32_t k, uint32_t n) { return (3 * k + n + 2 * s + cta) % (kMaxB + 1); }

static itype_t to_itype(int value) {
  auto bits = bit_cast<uint32_t>(float(value));
  if constexpr (std::is_same<vt::ITYPE, vt::fp16>::value) {
    return rv_ftoh_s(bits, 0, nullptr);
  } else {
    return rv_ftob_s(bits, 0, nullptr);
  }
}

const char *kernel_file = "kernel.vxbin";
uint32_t mode  = MODE_A_SMEM;
uint32_t N     = 0;  // 0: one epilogue transfer's columns
uint32_t iters = 64;
uint32_t slabs = 4;
uint32_t cp_batch = 1;
uint32_t grid  = 1;
uint32_t issue = ISSUE_LOOP;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h C_buffer = nullptr;
vx_buffer_h cycles_buffer = nullptr;
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex tensor-memory engine roof." << std::endl;
  std::cout << "Usage: [-m mode: 0=A smem, B K-major 1=A smem, B block-major 2=A by tc.cp 3=A smem, B [column][k]] [-n tile N] [-i iters]"
               " [-s slabs] [-c K steps per tc.cp] [-g grid] [-u issue: 0=loop 1=unrolled 2=every warp 3=loop + 8 nops] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "m:n:i:s:c:g:u:h")) != -1) {
    switch (c) {
    case 'm': mode = atoi(optarg); break;
    case 'n': N = atoi(optarg); break;
    case 'i': iters = atoi(optarg); break;
    case 's': slabs = atoi(optarg); break;
    case 'c': cp_batch = atoi(optarg); break;
    case 'g': grid = atoi(optarg); break;
    case 'u': issue = atoi(optarg); break;
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
    if (src_buffer) vx_buffer_release(src_buffer);
    if (C_buffer) vx_buffer_release(C_buffer);
    if (cycles_buffer) vx_buffer_release(cycles_buffer);
    if (kernel)  vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue)   vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

static bool is_pow2(uint32_t x) { return x != 0 && (x & (x - 1)) == 0; }

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  std::cout << "open device connection" << std::endl;
  RT_CHECK(vx_device_open(0, &device));

  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t NT, issue_width;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &NT));
  RT_CHECK(vx_device_query(device, VX_CAPS_ISSUE_WIDTH, &issue_width));
  if (NT != VX_CFG_NUM_THREADS) {
    std::cout << "Error: device warp size (" << NT << ") must match VX_CFG_NUM_THREADS=" << VX_CFG_NUM_THREADS << "!" << std::endl;
    cleanup();
    return -1;
  }

  // One warp per TCU block: the engine computes blocks * xtileM rows per op.
  const uint32_t warps = issue_width;
  const uint32_t ctaM  = warps * cfg::xtileM;
  const uint32_t stepK = cfg::fedpK * cfg::i_ratio;  // K elements of one tc.mma
  const uint32_t step_words = cfg::fedpK;
  if (N == 0) {
    N = cfg::xtileN;
  }
  if (mode > MODE_B_CHUNK || (N % cfg::xtileN) != 0 || N / cfg::tcN > 128
   || !is_pow2(slabs) || iters == 0 || (iters % slabs) != 0
   || !is_pow2(cp_batch) || cp_batch > slabs || issue > ISSUE_PAD
   || (issue != ISSUE_LOOP && (mode != MODE_A_SMEM || slabs != 4))) {
    std::cout << "Error: invalid arguments (N a multiple of " << cfg::xtileN << " up to " << 128 * cfg::tcN
              << ", slabs and cp batch powers of two, iters a multiple of slabs)!" << std::endl;
    cleanup();
    return -1;
  }
  const uint32_t passes = iters / slabs;
  if (uint64_t(iters) * stepK * kMaxA * kMaxB >= (1u << 24)) {
    std::cout << "Error: too many iterations for an exact fp32 sum!" << std::endl;
    cleanup();
    return -1;
  }

  const size_t a_elems = size_t(ctaM) * stepK;  // one slab
  const size_t b_elems = size_t(N) * stepK;
  const size_t cta_elems = slabs * (a_elems + b_elems);
  const size_t sizeC = size_t(grid) * ctaM * N;
  uint32_t tmem_cols = 32;
  while (tmem_cols < N + ((mode == MODE_A_TMEM) ? slabs * step_words : 0)) {
    tmem_cols <<= 1;
  }

  printf("TMEM_GEOMETRY: mode=%u itype=%s M=%u N=%u K=%u warps_per_cta=%u grid=%u iters=%u slabs=%u cp_batch=%u issue=%u tmem_cols=%u\n",
         mode, vt::ITYPE::name, ctaM, N, stepK, warps, grid, iters, slabs, cp_batch, issue, tmem_cols);

  kernel_arg.mode  = mode;
  kernel_arg.N     = N;
  kernel_arg.iters = iters;
  kernel_arg.slabs = slabs;
  kernel_arg.cp_batch = cp_batch;
  kernel_arg.issue = issue;

  std::cout << "allocate device memory" << std::endl;
  RT_CHECK(vx_buffer_create(device, grid * cta_elems * sizeof(itype_t), VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_buffer_create(device, sizeC * sizeof(float), VX_MEM_WRITE, &C_buffer));
  RT_CHECK(vx_buffer_address(C_buffer, &kernel_arg.C_addr));
  RT_CHECK(vx_buffer_create(device, grid * 2 * sizeof(uint64_t), VX_MEM_WRITE, &cycles_buffer));
  RT_CHECK(vx_buffer_address(cycles_buffer, &kernel_arg.cycles_addr));

  // A slab: row-major [m][k], or for tc.cp in TMEM storage order (column
  // group, block, M step, then lane i and column j of the fragment row).
  // B slab: K-major [n][k], which is also the block-major layout of one K step;
  // MODE_B_CHUNK lays B out as [n][slab][k].
  std::vector<itype_t> h_src(grid * cta_elems);
  for (uint32_t g = 0; g < grid; ++g) {
    itype_t* pA = h_src.data() + g * cta_elems;
    itype_t* pB = pA + slabs * a_elems;
    for (uint32_t s = 0; s < slabs; ++s) {
      for (uint32_t m = 0; m < ctaM; ++m) {
        for (uint32_t k = 0; k < stepK; ++k) {
          size_t idx = size_t(m) * stepK + k;
          if (mode == MODE_A_TMEM) {
            uint32_t z = k / cfg::i_ratio, e = k % cfg::i_ratio;
            uint32_t grp = z / cfg::tcN, j = z % cfg::tcN;
            uint32_t blk = m / cfg::xtileM, ms = (m % cfg::xtileM) / cfg::tcM, i = m % cfg::tcM;
            size_t word = ((size_t(grp) * warps + blk) * cfg::m_steps + ms) * VX_CFG_NUM_THREADS + i * cfg::tcN + j;
            idx = word * cfg::i_ratio + e;
          }
          pA[s * a_elems + idx] = to_itype(value_a(g, s, m, k));
        }
      }
      for (uint32_t n = 0; n < N; ++n) {
        for (uint32_t k = 0; k < stepK; ++k) {
          size_t idx = (mode == MODE_B_CHUNK) ? (size_t(n) * slabs + s) * stepK + k
                                              : s * b_elems + size_t(n) * stepK + k;
          pB[idx] = to_itype(value_b(g, s, k, n));
        }
      }
    }
  }

  std::cout << "upload source buffers" << std::endl;
  RT_CHECK(vx_enqueue_write(queue, src_buffer, 0, h_src.data(), h_src.size() * sizeof(itype_t), 0, nullptr, nullptr));

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
  li.block_dim[0] = warps * NT;
  li.block_dim[1] = 1;
  li.lmem_size    = TMEM_SLOT_BYTES + cta_elems * sizeof(itype_t);
  li.tmem_cols    = tmem_cols;

  std::vector<float> h_C(sizeC);
  std::vector<uint64_t> h_cycles(grid * 2);

  std::cout << "launch kernel" << std::endl;
  vx_event_h launch_ev = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  std::cout << "download destination buffer" << std::endl;
  vx_event_h read_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_C.data(), C_buffer, 0, sizeC * sizeof(float), 1, &launch_ev, &read_ev));
  vx_event_h cyc_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_cycles.data(), cycles_buffer, 0,
                           h_cycles.size() * sizeof(uint64_t), 1, &read_ev, &cyc_ev));
  RT_CHECK(vx_event_wait_value(cyc_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(cyc_ev);
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  {
    // span: from the first CTA issuing its first op to the last commit arriving.
    uint64_t first = UINT64_MAX, last = 0;
    for (uint32_t g = 0; g < grid; ++g) {
      first = std::min(first, h_cycles[2 * g]);
      last = std::max(last, h_cycles[2 * g + 1]);
    }
    uint64_t span = last - first;
    uint64_t fmas = uint64_t(grid) * iters * ctaM * N * stepK;
    printf("TMEM_CYCLES: span=%lu first=%lu last=%lu ops=%u cycles_per_op=%.2f\n", span, first, last,
           grid * iters, double(span) / (grid * iters));
    printf("TMEM_FMAS: %lu\n", fmas);
    printf("TMEM_FLOPS_PER_CYCLE: %.1f\n", 2.0 * fmas / span);
  }

  std::cout << "verify result" << std::endl;
  int errors = 0;
  for (uint32_t g = 0; g < grid; ++g) {
    for (uint32_t m = 0; m < ctaM; ++m) {
      for (uint32_t n = 0; n < N; ++n) {
        int dot = 0;
        for (uint32_t s = 0; s < slabs; ++s) {
          for (uint32_t k = 0; k < stepK; ++k) {
            dot += value_a(g, s, m, k) * value_b(g, s, k, n);
          }
        }
        float expected = float(passes) * float(dot);
        size_t idx = (size_t(g) * ctaM + m) * N + n;
        if (h_C[idx] != expected) {
          if (errors < MAX_ERRORS) {
            printf("*** error: cta=%u [%u,%u] expected=%f, actual=%f\n", g, m, n, expected, h_C[idx]);
          }
          ++errors;
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
