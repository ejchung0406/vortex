#include "common.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <rvfloats.h>
#include <string>
#include <string.h>
#include <tensor_cfg.h>
#include <type_traits>
#include <unistd.h>
#include <util.h>
#include <vector>
#include <vortex2.h>
#include <dxa.h>

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

static_assert(std::is_same<vt::ITYPE, vt::fp16>::value, "gemm_tmem_decode generates fp16 operands");
static_assert(std::is_same<vt::OTYPE, vt::fp32>::value, "gemm_tmem_decode accumulates in fp32");

using cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, GEMM_NRC>;
using itype_t = vt::ITYPE::dtype;

constexpr uint32_t kStepK   = cfg::fedpK * cfg::i_ratio;
constexpr uint32_t kStepRow = kStepK * sizeof(itype_t);
constexpr uint32_t kStageW  = KSTEPS * TILE_N * kStepRow;

// The drift of the tensor core's fp32 accumulation from a double reference
// tracks K (sgemm_tcu_tmem's bound).
static uint32_t float_ulp(uint32_t K) {
  return std::max<uint32_t>(8, K / 5);
}

const char *kernel_file = "kernel.vxbin";
uint32_t xm = 16;
uint32_t xn = 8192;
uint32_t xk = 4096;
uint32_t xP = 1;  // passes over the N tiles
bool xa = false;  // place the buffers past the heap's first 2 MB

vx_device_h device = nullptr;
vx_buffer_h A_buffer = nullptr;
vx_buffer_h W_buffer = nullptr;
vx_buffer_h C_buffer = nullptr;
vx_buffer_h T_buffer = nullptr;
vx_buffer_h pad_buffer = nullptr;
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex tensor-memory decode GEMM." << std::endl;
  std::cout << "Usage: [-m M (1..64)] [-n N] [-k K] [-P passes over W] [-a: buffers past the first 2 MB] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "m:n:k:P:ah")) != -1) {
    switch (c) {
    case 'm': xm = atoi(optarg); break;
    case 'n': xn = atoi(optarg); break;
    case 'k': xk = atoi(optarg); break;
    case 'P': xP = atoi(optarg); break;
    case 'a': xa = true; break;
    case 'h': show_usage(); exit(0); break;
    default:  show_usage(); exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    if (A_buffer) vx_buffer_release(A_buffer);
    if (W_buffer) vx_buffer_release(W_buffer);
    if (C_buffer) vx_buffer_release(C_buffer);
    if (T_buffer) vx_buffer_release(T_buffer);
    if (pad_buffer) vx_buffer_release(pad_buffer);
    if (kernel)  vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue)   vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

static void reject(const std::string& msg) {
  std::cout << "Error: " << msg << "!" << std::endl;
  cleanup();
  exit(-1);
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);
  std::srand(50);

  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t isa_flags, NT, issue_width, num_cores, lmem_bytes;
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &NT));
  RT_CHECK(vx_device_query(device, VX_CAPS_ISSUE_WIDTH, &issue_width));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &lmem_bytes));
  if ((isa_flags & VX_ISA_EXT_TCU) == 0 || (isa_flags & VX_ISA_EXT_DXA) == 0)
    reject("needs the TCU and DXA extensions");
  if (NT != VX_CFG_NUM_THREADS)
    reject("device warp size must match VX_CFG_NUM_THREADS");
  if (issue_width * cfg::xtileM != 64)
    reject("a tc.mma must cover 64 rows (4 TCU blocks)");

  const uint32_t M = xm, N = xn, K = xk;
  if (M < 1 || M > 64)
    reject("M must be 1..64, the lanes of one tc.mma");
  if (N % TILE_N != 0)
    reject("N must be a multiple of TILE_N = " + std::to_string(TILE_N));
  if (K % (KSTEPS * kStepK) != 0)
    reject("K must be a multiple of KSTEPS * " + std::to_string(kStepK));
  const uint32_t k_steps = K / kStepK;
  const uint32_t num_tiles = N / TILE_N;
  const uint32_t total_tiles = num_tiles * xP;
  if (num_tiles < num_cores)
    reject("N must give every core a tile: at least " + std::to_string(num_cores * TILE_N));
  const uint32_t stage_a = ((KSTEPS - 1) * M * kStepRow + 64 * kStepRow + 127) / 128 * 128;
  const uint32_t lmem_size = TMEM_SLOT_BYTES + PIPE * (stage_a + kStageW);
  if (lmem_size > lmem_bytes)
    reject("PIPE stages exceed the local memory");
  // One persistent CTA per core: its two accumulator buffers take TMEM.
  const uint32_t num_ctas = num_cores;
  const uint32_t c_rows = (M + cfg::xtileM - 1) / cfg::xtileM * cfg::xtileM;
  const uint32_t warps = STAGE_LOAD ? (LOAD_WARP0 + LOAD_WARPS) : CONSUME_WARPS ? (LOAD_WARP0 + CONSUME_WARPS) : LOAD_WARP0;
  const double w_bytes = double(N) * K * sizeof(itype_t);
  printf("GEMM_DECODE: M=%u N=%u K=%u tile_n=%u ksteps=%u pipe=%u xfer=%u xfers_per_stage=%u layout=%u "
         "stage=%s warps=%u stage_bytes=%u lmem=%u tiles=%u ctas=%u cores=%u weight_bytes=%.0f\n",
         M, N, K, TILE_N, KSTEPS, PIPE, XFER, kStageW / XFER, LAYOUT, STAGE_LOAD ? "load" : "dxa",
         warps, stage_a + kStageW, lmem_size, num_tiles, num_ctas, (uint32_t)num_cores, w_bytes);

  // A is packed by K step, [K/kstep][M][kstep]; W is the logical [K][N] in
  // the device layout LAYOUT.
  std::vector<itype_t> h_A(size_t(M) * K), h_W(size_t(K) * N);
  for (auto& v : h_A) v = rv_ftoh_s(bit_cast<uint32_t>(float(rand()) / RAND_MAX), 0, nullptr);
  for (auto& v : h_W) v = rv_ftoh_s(bit_cast<uint32_t>(float(rand()) / RAND_MAX), 0, nullptr);
  std::vector<itype_t> h_Ap(h_A.size()), h_Wd(h_W.size());
  for (uint32_t m = 0; m < M; ++m)
    for (uint32_t k = 0; k < K; ++k)
      h_Ap[(size_t(k / kStepK) * M + m) * kStepK + k % kStepK] = h_A[size_t(m) * K + k];
  for (uint32_t k = 0; k < K; ++k) {
    for (uint32_t n = 0; n < N; ++n) {
      size_t idx;
      if (LAYOUT == LAYOUT_PACKED)
        idx = ((size_t(n / TILE_N) * k_steps + k / kStepK) * TILE_N + n % TILE_N) * kStepK + k % kStepK;
      else if (LAYOUT == LAYOUT_KN)
        idx = size_t(k) * N + n;
      else
        idx = size_t(n) * K + k;
      h_Wd[idx] = h_W[size_t(k) * N + n];
    }
  }

  if (xa) {
    // The heap starts below the first 2 MB boundary, which the superpage heap
    // (VORTEX_VM_MEGAPAGE_HEAP) maps with 4 KB pages; a GPU driver backs a
    // large allocation with 2 MB pages throughout.
    RT_CHECK(vx_buffer_create(device, 2u << 20, VX_MEM_READ, &pad_buffer));
  }
  RT_CHECK(vx_buffer_create(device, h_Ap.size() * sizeof(itype_t), VX_MEM_READ, &A_buffer));
  RT_CHECK(vx_buffer_address(A_buffer, &kernel_arg.A_addr));
  RT_CHECK(vx_buffer_create(device, h_Wd.size() * sizeof(itype_t), VX_MEM_READ, &W_buffer));
  RT_CHECK(vx_buffer_address(W_buffer, &kernel_arg.W_addr));
  RT_CHECK(vx_buffer_create(device, size_t(c_rows) * N * sizeof(float), VX_MEM_WRITE, &C_buffer));
  RT_CHECK(vx_buffer_address(C_buffer, &kernel_arg.C_addr));
  const uint32_t num_times = 4 * total_tiles + 2 * num_ctas;
  RT_CHECK(vx_buffer_create(device, num_times * sizeof(uint64_t), VX_MEM_WRITE, &T_buffer));
  RT_CHECK(vx_buffer_address(T_buffer, &kernel_arg.times_addr));
  RT_CHECK(vx_enqueue_write(queue, A_buffer, 0, h_Ap.data(), h_Ap.size() * sizeof(itype_t), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, W_buffer, 0, h_Wd.data(), h_Wd.size() * sizeof(itype_t), 0, nullptr, nullptr));

  kernel_arg.M = M;
  kernel_arg.N = N;
  kernel_arg.K = K;
  kernel_arg.passes = xP;

  if (!STAGE_LOAD) {
    // A: KSTEPS K steps of M rows, one contiguous run.
    RT_CHECK(vortex::dxa::program_2d(device, 0, kernel_arg.A_addr,
      /*size0=*/M * kStepK, /*size1=*/k_steps, /*stride0_bytes=*/M * kStepRow,
      /*tile0=*/M * kStepK, /*tile1=*/KSTEPS, /*elem_bytes=*/sizeof(itype_t)));
    // W: one transfer is XFER bytes of a stage, landing as rows of the K step.
    if (LAYOUT == LAYOUT_PACKED) {
      RT_CHECK(vortex::dxa::program_2d(device, 1, kernel_arg.W_addr,
        /*size0=*/XFER / sizeof(itype_t), /*size1=*/uint32_t(w_bytes / XFER), /*stride0_bytes=*/XFER,
        /*tile0=*/XFER / sizeof(itype_t), /*tile1=*/1, /*elem_bytes=*/sizeof(itype_t)));
    } else if (LAYOUT == LAYOUT_NK16 || LAYOUT == LAYOUT_NK32) {
      // (elements of a row, n, K step or pair): rows of 32 or 64 bytes.
      const uint32_t row = (LAYOUT == LAYOUT_NK32) ? 2 * kStepK : kStepK;
      RT_CHECK(vortex::dxa::program_3d(device, 1, kernel_arg.W_addr,
        /*size0=*/row, /*size1=*/N, /*size2=*/K / row,
        /*stride0_bytes=*/K * sizeof(itype_t), /*stride1_bytes=*/row * sizeof(itype_t),
        /*tile0=*/row, /*tile1=*/XFER / (row * sizeof(itype_t)), /*tile2=*/1, /*elem_bytes=*/sizeof(itype_t)));
    } else {
      // W[K][N]: (n, k) tiles scattered K-major, n's K step 32 bytes apart.
      RT_CHECK(vortex::dxa::program_2d(device, 1, kernel_arg.W_addr,
        /*size0=*/N, /*size1=*/K, /*stride0_bytes=*/N * sizeof(itype_t),
        /*tile0=*/XFER / kStepRow, /*tile1=*/kStepK, /*elem_bytes=*/sizeof(itype_t)));
      RT_CHECK(vortex::dxa::set_layout(device, 1, vortex::dxa::Layout::KMajor, /*rank=*/2, sizeof(itype_t)));
    }
  }

  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  std::vector<float> h_C(size_t(c_rows) * N);
  std::vector<uint64_t> h_T(num_times);

  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 2;
  li.grid_dim[0]  = num_ctas;
  li.grid_dim[1]  = 1;
  li.block_dim[0] = warps * NT;
  li.block_dim[1] = 1;
  li.lmem_size    = lmem_size;
  li.tmem_cols    = 2 * TILE_N;

  vx_event_h launch_ev = nullptr, read_ev = nullptr, t_ev = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));
  RT_CHECK(vx_enqueue_read(queue, h_C.data(), C_buffer, 0, h_C.size() * sizeof(float), 1, &launch_ev, &read_ev));
  RT_CHECK(vx_enqueue_read(queue, h_T.data(), T_buffer, 0, h_T.size() * sizeof(uint64_t), 1, &read_ev, &t_ev));
  RT_CHECK(vx_event_wait_value(t_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(t_ev);
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  {
    // The multiply phase of a tile runs from its first stage landing to its
    // last multiply completing; per core the union of its tiles' phases.
    // With several passes, also the passes after the first, which find W in the L2.
    uint64_t first = UINT64_MAX, last = 0;
    for (uint32_t c = 0; c < num_ctas; ++c) {
      first = std::min(first, h_T[4 * total_tiles + 2 * c]);
      last = std::max(last, h_T[4 * total_tiles + 2 * c + 1]);
    }
    auto union_cycles = [&](uint32_t t0) {
      uint64_t mult = 0;
      for (uint32_t core = 0; core < num_cores; ++core) {
        std::vector<std::pair<uint64_t, uint64_t>> iv;
        for (uint32_t t = t0; t < total_tiles; ++t) {
          if (h_T[4 * t] == core)
            iv.push_back({h_T[4 * t + 1], h_T[4 * t + 2]});
        }
        std::sort(iv.begin(), iv.end());
        uint64_t lo = iv.empty() ? 0 : iv[0].first, hi = lo;
        for (auto& p : iv) {
          if (p.first > hi) {
            mult += hi - lo;
            lo = p.first;
          }
          hi = std::max(hi, p.second);
        }
        mult += hi - lo;
      }
      return mult;
    };
    uint64_t mult = union_cycles(0);
    uint64_t span = last - first;
    double bytes = w_bytes * xP;
    double per_mult = bytes / mult, per_kernel = bytes / (double(num_cores) * span);
    printf("DECODE_CYCLES: span=%lu multiply=%lu (summed over cores)\n", span, mult);
    printf("DECODE_W_BYTES_PER_CLOCK_PER_SM: multiply=%.2f kernel=%.2f\n", per_mult, per_kernel);
    printf("DECODE_W_GBS_PER_SM: multiply=%.1f kernel=%.1f total_kernel=%.1f (at %d MHz)\n",
           per_mult * VX_CFG_PLATFORM_CLOCK_RATE / 1000.0, per_kernel * VX_CFG_PLATFORM_CLOCK_RATE / 1000.0,
           bytes / span * VX_CFG_PLATFORM_CLOCK_RATE / 1000.0, VX_CFG_PLATFORM_CLOCK_RATE);
    if (xP > 1) {
      uint64_t later = union_cycles(num_tiles);
      printf("DECODE_W_BYTES_PER_CLOCK_PER_SM_LATER_PASSES: multiply=%.2f\n", w_bytes * (xP - 1) / later);
    }
  }

  int errors = 0;
  uint32_t max_ulp = 0;
  const uint32_t ulp_bound = float_ulp(K);
#if defined(NO_MMA) || defined(READ_ONLY)
  std::cout << "staging or reads only, C not checked" << std::endl;
  if (false)
#elif CONSUME_WARPS
  {
    // Each consumer thread's XOR of its words of every stage of its CTA's tiles.
    constexpr uint32_t kT = CONSUME_WARPS * VX_CFG_NUM_THREADS;
    const uint32_t stage_words = kStageW / 8, num_stages = k_steps / KSTEPS;
    auto words = reinterpret_cast<const uint64_t*>(h_Wd.data());
    std::vector<uint64_t> got(num_ctas * kT);
    memcpy(got.data(), h_C.data(), got.size() * 8);  // the consumers' checksums, at the start of C
    for (uint32_t c = 0; c < num_ctas; ++c) {
      for (uint32_t t = 0; t < kT; ++t) {
        uint64_t x = 0;
        for (uint32_t tile = c; tile < total_tiles; tile += num_ctas)
          for (uint32_t i = 0; i < num_stages; ++i) {
            size_t base = (size_t((tile % num_tiles) * k_steps + i * KSTEPS) * TILE_N * kStepRow) / 8;
            for (uint32_t j = t; j < stage_words; j += kT) x ^= words[base + j];
          }
        if (got[c * kT + t] != x) {
          if (errors < MAX_ERRORS) printf("*** error: checksum cta=%u thread=%u\n", c, t);
          ++errors;
        }
      }
    }
  }
  if (false)
#endif
  {
    std::vector<float> fW(h_W.size());
    for (size_t i = 0; i < h_W.size(); ++i) fW[i] = bit_cast<float>(rv_htof_s(h_W[i], 0, nullptr));
    std::vector<double> acc(N);
    for (uint32_t m = 0; m < M; ++m) {
      std::fill(acc.begin(), acc.end(), 0.0);
      for (uint32_t k = 0; k < K; ++k) {
        double a = bit_cast<float>(rv_htof_s(h_A[size_t(m) * K + k], 0, nullptr));
        const float* pW = &fW[size_t(k) * N];
        for (uint32_t n = 0; n < N; ++n)
          acc[n] += a * pW[n];
      }
      for (uint32_t n = 0; n < N; ++n) {
        float expected = float(acc[n]);
        float actual = h_C[size_t(m) * N + n];
        int32_t d = std::abs(bit_cast<int32_t>(actual) - bit_cast<int32_t>(expected));
        max_ulp = std::max<uint32_t>(max_ulp, d);
        if (d > (int32_t)ulp_bound) {
          if (errors < MAX_ERRORS)
            printf("*** error: [%u,%u] expected=%f, actual=%f\n", m, n, expected, actual);
          ++errors;
        }
      }
    }
  }
  printf("reference drift: max=%u ULP, bound=%u ULP (K=%u)\n", max_ulp, ulp_bound, K);

  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " / " << size_t(M) * N << " errors!" << std::endl;
    std::cout << "FAILED!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
