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

static_assert(std::is_same<vt::ITYPE, vt::fp16>::value, "gemm_tmem generates fp16 operands");
static_assert(std::is_same<vt::OTYPE, vt::fp32>::value, "gemm_tmem accumulates in fp32");

using cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE, GEMM_NRC>;
using itype_t = vt::ITYPE::dtype;

constexpr uint32_t kTileM = 64;
constexpr uint32_t kStepK = cfg::fedpK * cfg::i_ratio;
constexpr uint32_t kCtaM  = MTILES * kTileM;
constexpr uint32_t kStage = (kCtaM + TILE_N) * KSTEPS * kStepK * sizeof(itype_t);

// The drift of the tensor core's fp32 accumulation from a double reference
// tracks K (sgemm_tcu_tmem's bound).
static uint32_t float_ulp(uint32_t K) {
  return std::max<uint32_t>(8, K / 5);
}

const char *kernel_file = "kernel.vxbin";
uint32_t xm = 256;
uint32_t xn = 1024;
uint32_t xk = 1024;
uint32_t xR = 0;  // B columns the CTAs share (0: each N tile its own)

vx_device_h device = nullptr;
vx_buffer_h A_buffer = nullptr;
vx_buffer_h B_buffer = nullptr;
vx_buffer_h C_buffer = nullptr;
vx_buffer_h T_buffer = nullptr;
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex tensor-memory GEMM." << std::endl;
  std::cout << "Usage: [-m M] [-n N] [-k K] [-R B columns the CTAs share] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "m:n:k:R:h")) != -1) {
    switch (c) {
    case 'm': xm = atoi(optarg); break;
    case 'n': xn = atoi(optarg); break;
    case 'k': xk = atoi(optarg); break;
    case 'R': xR = atoi(optarg); break;
    case 'h': show_usage(); exit(0); break;
    default:  show_usage(); exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    if (A_buffer) vx_buffer_release(A_buffer);
    if (B_buffer) vx_buffer_release(B_buffer);
    if (C_buffer) vx_buffer_release(C_buffer);
    if (T_buffer) vx_buffer_release(T_buffer);
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
  if (issue_width * cfg::xtileM != kTileM)
    reject("a tc.mma must cover 64 rows (4 TCU blocks)");

  const uint32_t M = xm, N = xn, K = xk;
  const uint32_t NB = xR ? xR : N;  // B columns on the device
  if (M % kCtaM != 0)
    reject("M must be a multiple of MTILES * 64 = " + std::to_string(kCtaM));
  if (N % TILE_N != 0 || NB % TILE_N != 0 || NB > N)
    reject("N and -R must be multiples of TILE_N = " + std::to_string(TILE_N) + ", -R at most N");
  if (K % (KSTEPS * kStepK) != 0)
    reject("K must be a multiple of KSTEPS * " + std::to_string(kStepK));
  const uint32_t k_steps = K / kStepK;
  const uint32_t lmem_size = TMEM_SLOT_BYTES + PIPE * kStage;
  if (lmem_size > lmem_bytes)
    reject("PIPE stages of " + std::to_string(kStage) + " bytes exceed the local memory");

  // Persistent CTAs: as many as the cores hold at once, by TMEM and by local memory.
  const uint32_t num_tiles = (N / TILE_N) * (M / kCtaM);
  const uint32_t per_core = std::min<uint32_t>(VX_CFG_TCU_TMEM_COLS / (MTILES * TILE_N), lmem_bytes / lmem_size);
  const uint32_t num_ctas = std::min<uint32_t>(num_tiles, per_core * num_cores);
  printf("GEMM_TMEM: M=%u N=%u K=%u R=%u tile=%ux%u mtiles=%u ksteps=%u pipe=%u epi_warps=%u stage_bytes=%u lmem=%u "
         "tmem_cols=%u tiles=%u ctas=%u ctas_per_core=%u cores=%u\n",
         M, N, K, xR, kCtaM, TILE_N, MTILES, KSTEPS, PIPE, EPI_WARPS, kStage, lmem_size, MTILES * TILE_N,
         num_tiles, num_ctas, per_core, (uint32_t)num_cores);

  // Operands, then their packed device copies: an M tile's (an N tile's) K
  // step is a block of 64 (TILE_N) rows of kStepK elements.
  std::vector<itype_t> h_A(size_t(M) * K), h_B(size_t(K) * NB);
  for (auto& v : h_A) v = rv_ftoh_s(bit_cast<uint32_t>(float(rand()) / RAND_MAX), 0, nullptr);
  for (auto& v : h_B) v = rv_ftoh_s(bit_cast<uint32_t>(float(rand()) / RAND_MAX), 0, nullptr);
  std::vector<itype_t> h_Ap(h_A.size()), h_Bp(h_B.size());
  for (uint32_t m = 0; m < M; ++m)
    for (uint32_t k = 0; k < K; ++k)
      h_Ap[((size_t(m / kTileM) * k_steps + k / kStepK) * kTileM + m % kTileM) * kStepK + k % kStepK] = h_A[size_t(m) * K + k];
  for (uint32_t k = 0; k < K; ++k)
    for (uint32_t n = 0; n < NB; ++n)
      h_Bp[((size_t(n / TILE_N) * k_steps + k / kStepK) * TILE_N + n % TILE_N) * kStepK + k % kStepK] = h_B[size_t(k) * NB + n];

  RT_CHECK(vx_buffer_create(device, h_Ap.size() * sizeof(itype_t), VX_MEM_READ, &A_buffer));
  RT_CHECK(vx_buffer_address(A_buffer, &kernel_arg.A_addr));
  RT_CHECK(vx_buffer_create(device, h_Bp.size() * sizeof(itype_t), VX_MEM_READ, &B_buffer));
  RT_CHECK(vx_buffer_address(B_buffer, &kernel_arg.B_addr));
  RT_CHECK(vx_buffer_create(device, size_t(M) * N * sizeof(float), VX_MEM_WRITE, &C_buffer));
  RT_CHECK(vx_buffer_address(C_buffer, &kernel_arg.C_addr));
  const uint32_t num_times = 4 * num_tiles + 2 * num_ctas;
  RT_CHECK(vx_buffer_create(device, num_times * sizeof(uint64_t), VX_MEM_WRITE, &T_buffer));
  RT_CHECK(vx_buffer_address(T_buffer, &kernel_arg.times_addr));
#ifdef PLAIN_LAYOUT
  // A as it is, B as the weights of a linear layer, W[N][K].
  h_Ap = h_A;
  for (uint32_t k = 0; k < K; ++k)
    for (uint32_t n = 0; n < NB; ++n)
      h_Bp[size_t(n) * K + k] = h_B[size_t(k) * NB + n];
#endif
  RT_CHECK(vx_enqueue_write(queue, A_buffer, 0, h_Ap.data(), h_Ap.size() * sizeof(itype_t), 0, nullptr, nullptr));
  RT_CHECK(vx_enqueue_write(queue, B_buffer, 0, h_Bp.data(), h_Bp.size() * sizeof(itype_t), 0, nullptr, nullptr));

  kernel_arg.M = M;
  kernel_arg.N = N;
  kernel_arg.K = K;
  kernel_arg.b_wrap = NB / TILE_N;

#ifdef PLAIN_LAYOUT
  // A transfer is (K step elements, rows, K steps): KSTEPS K steps of one M
  // tile, or one K step of the B tile.
  RT_CHECK(vortex::dxa::program_3d(device, 0, kernel_arg.A_addr,
    /*size0=*/kStepK, /*size1=*/M, /*size2=*/k_steps,
    /*stride0_bytes=*/K * sizeof(itype_t), /*stride1_bytes=*/kStepK * sizeof(itype_t),
    /*tile0=*/kStepK, /*tile1=*/kTileM, /*tile2=*/KSTEPS, /*elem_bytes=*/sizeof(itype_t)));
  RT_CHECK(vortex::dxa::program_3d(device, 1, kernel_arg.B_addr,
    /*size0=*/kStepK, /*size1=*/NB, /*size2=*/k_steps,
    /*stride0_bytes=*/K * sizeof(itype_t), /*stride1_bytes=*/kStepK * sizeof(itype_t),
    /*tile0=*/kStepK, /*tile1=*/TILE_N, /*tile2=*/1, /*elem_bytes=*/sizeof(itype_t)));
#else
  // A transfer copies KSTEPS K-step blocks of one M tile, or one of the B tile.
  RT_CHECK(vortex::dxa::program_2d(device, 0, kernel_arg.A_addr,
    /*size0=*/kTileM * kStepK, /*size1=*/(M / kTileM) * k_steps,
    /*stride0_bytes=*/kTileM * kStepK * sizeof(itype_t),
    /*tile0=*/kTileM * kStepK, /*tile1=*/KSTEPS, /*elem_bytes=*/sizeof(itype_t)));
  RT_CHECK(vortex::dxa::program_2d(device, 1, kernel_arg.B_addr,
    /*size0=*/TILE_N * kStepK, /*size1=*/(NB / TILE_N) * k_steps,
    /*stride0_bytes=*/TILE_N * kStepK * sizeof(itype_t),
    /*tile0=*/TILE_N * kStepK, /*tile1=*/1, /*elem_bytes=*/sizeof(itype_t)));
#endif

  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  std::vector<float> h_C(size_t(M) * N);
  std::vector<uint64_t> h_T(num_times);

  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 2;
  li.grid_dim[0]  = num_ctas;
  li.grid_dim[1]  = 1;
  li.block_dim[0] = (4 + EPI_WARPS) * NT;
  li.block_dim[1] = 1;
  li.lmem_size    = lmem_size;
  li.tmem_cols    = MTILES * TILE_N;

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
    uint64_t first = UINT64_MAX, last = 0, mult = 0, epi = 0;
    for (uint32_t c = 0; c < num_ctas; ++c) {
      first = std::min(first, h_T[4 * num_tiles + 2 * c]);
      last = std::max(last, h_T[4 * num_tiles + 2 * c + 1]);
    }
    for (uint32_t core = 0; core < num_cores; ++core) {
      std::vector<std::pair<uint64_t, uint64_t>> iv;
      for (uint32_t t = 0; t < num_tiles; ++t) {
        if (h_T[4 * t] == core) {
          iv.push_back({h_T[4 * t + 1], h_T[4 * t + 2]});
          epi += h_T[4 * t + 3] - h_T[4 * t + 2];
        }
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
    double flops = 2.0 * M * N * K;
    double staged = double(num_tiles) * (kCtaM + TILE_N) * K * sizeof(itype_t);
    uint64_t span = last - first;
    printf("GEMM_CYCLES: span=%lu multiply=%lu (summed over cores) epilogue=%lu (summed over tiles)\n", span, mult, epi);
    printf("GEMM_FLOPS_PER_CLOCK_PER_SM: multiply=%.1f kernel=%.1f\n", flops / mult, flops / (double(num_cores) * span));
    printf("GEMM_STAGED_BYTES_PER_CLOCK_PER_SM: multiply=%.2f kernel=%.2f total_bytes=%.0f\n",
           staged / mult, staged / (double(num_cores) * span), staged);
  }

  // Reference over the B columns on the device; CTA column nt used tile nt % b_wrap.
  int errors = 0;
  uint32_t max_ulp = 0;
  const uint32_t ulp_bound = float_ulp(K);
  {
    std::vector<float> fA(h_A.size()), fB(h_B.size());
    for (size_t i = 0; i < h_A.size(); ++i) fA[i] = bit_cast<float>(rv_htof_s(h_A[i], 0, nullptr));
    for (size_t i = 0; i < h_B.size(); ++i) fB[i] = bit_cast<float>(rv_htof_s(h_B[i], 0, nullptr));
    std::vector<double> acc(NB);
    for (uint32_t m = 0; m < M; ++m) {
      std::fill(acc.begin(), acc.end(), 0.0);
      for (uint32_t k = 0; k < K; ++k) {
        double a = fA[size_t(m) * K + k];
        const float* pB = &fB[size_t(k) * NB];
        for (uint32_t n = 0; n < NB; ++n)
          acc[n] += a * pB[n];
      }
      for (uint32_t n = 0; n < N; ++n) {
        float expected = float(acc[n % NB]);
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
