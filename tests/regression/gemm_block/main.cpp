#include "common.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <rvfloats.h>
#include <string>
#include <string.h>
#include <tensor_cfg.h>
#include <unistd.h>
#include <util.h>
#include <vector>
#include <vortex2.h>
#include <dxa.h>

#define FLOAT_ULP 10
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

///////////////////////////////////////////////////////////////////////////////

template <typename T>
static void convert_row_to_col_major(T *dst, const T *src, uint32_t rows, uint32_t cols) {
  for (uint32_t r = 0; r < rows; ++r) {
    for (uint32_t c = 0; c < cols; ++c) {
      dst[c * rows + r] = src[r * cols + c];
    }
  }
}

///////////////////////////////////////////////////////////////////////////////

template <typename T>
struct data_accessor_t {
  using Type = typename T::dtype;
  static Type read(const Type *ptr, uint32_t offset) {
    return ptr[offset];
  }
  static void write(Type *ptr, uint32_t offset, Type value) {
    ptr[offset] = value;
  }
};

template <>
struct data_accessor_t<vt::int4> {
  static uint8_t read(const uint8_t *ptr, uint32_t offset) {
    uint32_t row_off = offset / 2;
    bool odd = offset & 0x1;
    uint8_t value8 = ptr[row_off];
    return odd ? (value8 >> 4) : (value8 & 0x0f); // to nibble
  }
  static void write(uint8_t *ptr, uint32_t offset, int32_t value) {
    uint32_t row_off = offset / 2;
    bool odd = offset & 0x1;
    uint8_t old_value = ptr[row_off];
    uint8_t new_value = odd ? ((old_value & 0x0f) | (value << 4))
                            : ((old_value & 0xf0) | (value & 0x0f));
    ptr[offset / 2] = new_value;
  }
};

template <>
struct data_accessor_t<vt::uint4> {
  static uint8_t read(const uint8_t *ptr, uint32_t offset) {
    uint32_t row_off = offset / 2;
    bool odd = offset & 0x1;
    uint8_t value8 = ptr[row_off];
    return odd ? (value8 >> 4) : (value8 & 0x0f); // to nibble
  }
  static void write(uint8_t *ptr, uint32_t offset, int32_t value) {
    uint32_t row_off = offset / 2;
    bool odd = offset & 0x1;
    uint8_t old_value = ptr[row_off];
    uint8_t new_value = odd ? ((old_value & 0x0f) | (value << 4))
                            : ((old_value & 0xf0) | (value & 0x0f));
    ptr[offset / 2] = new_value;
  }
};

///////////////////////////////////////////////////////////////////////////////

template <typename Type>
class Comparator {};

template <>
class Comparator<vt::int8> {
public:
  static int8_t generate() {
    return (int8_t)rand();
  }
  static bool compare(int8_t a, int8_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::uint8> {
public:
  static uint8_t generate() {
    return (uint8_t)rand();
  }
  static bool compare(uint8_t a, uint8_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::int4> {
public:
  static uint8_t generate() {
    return (uint8_t)rand(); // store 2 nibbles in a byte
  }
  static bool compare(uint8_t a, uint8_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::uint4> {
public:
  static uint8_t generate() {
    return (uint8_t)rand(); // store 2 nibbles in a byte
  }
  static bool compare(uint8_t a, uint8_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::int32> {
public:
  static int32_t generate() {
    return (int32_t)rand();
  }
  static bool compare(int32_t a, int32_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::fp16> {
public:
  static uint16_t generate() {
    auto fvalue = float(rand()) / RAND_MAX;
    return rv_ftoh_s(bit_cast<uint32_t>(fvalue), 0, nullptr);
  }
  static bool compare(uint16_t a, uint16_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::bf16> {
public:
  static uint16_t generate() {
    auto fvalue = float(rand()) / RAND_MAX;
    return rv_ftob_s(bit_cast<uint32_t>(fvalue), 0, nullptr);
  }
  static bool compare(uint16_t a, uint16_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::fp8> {
public:
  static uint8_t generate() {
    auto fvalue = float(rand()) / RAND_MAX;
    return rv_ftoe4m3_s(bit_cast<uint32_t>(fvalue), 0, nullptr);
  }
  static bool compare(uint8_t a, uint8_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::bf8> {
public:
  static uint8_t generate() {
    auto fvalue = float(rand()) / RAND_MAX;
    return rv_ftoe5m2_s(bit_cast<uint32_t>(fvalue), 0, nullptr);
  }
  static bool compare(uint8_t a, uint8_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::tf32> {
public:
  static uint32_t generate() {
    auto fvalue = float(rand()) / RAND_MAX;
    return rv_ftotf32_s(bit_cast<uint32_t>(fvalue), 0, nullptr);
  }
  static bool compare(uint32_t a, uint32_t b, int index, int errors) {
    if (a != b) {
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=0x%x, actual=0x%x\n", index, b, a);
      }
      return false;
    }
    return true;
  }
};

template <>
class Comparator<vt::fp32> {
public:
  static float generate() {
    return static_cast<float>(rand()) / RAND_MAX;
  }
  static bool compare(float a, float b, int index, int errors) {
    // Lower-precision inputs (fp8/bf8/fp16/bf16) accumulated into fp32
    // can drift well past the strict ULP=10 threshold used for fp32 inputs.
    // Use a relative-error tolerance for these.
    constexpr bool low_prec_input =
        std::is_same<vt::ITYPE, vt::fp8>::value
     || std::is_same<vt::ITYPE, vt::bf8>::value
     || std::is_same<vt::ITYPE, vt::fp16>::value
     || std::is_same<vt::ITYPE, vt::bf16>::value;
    if constexpr (low_prec_input) {
      if (a == 0.0f && b == 0.0f) {
        return true;
      }
      // 1% relative error is comfortably above any single-element
      // mixed-precision accumulation drift seen on the regression
      // matrices, and tight enough to catch real systematic errors.
      auto diff = std::abs((a - b) / b);
      if (diff < 0.01f) {
        return true;
      }
      if (errors < MAX_ERRORS) {
        printf("*** error: [%d] expected=%f, actual=%f\n", index, b, a);
      }
      return false;
    } else {
      union fi_t {
        float f;
        int32_t i;
      };
      fi_t fa, fb;
      fa.f = a;
      fb.f = b;
      auto d = std::abs(fa.i - fb.i);
      if (d > FLOAT_ULP) {
        if (errors < MAX_ERRORS) {
          printf("*** error: [%d] expected=%f, actual=%f\n", index, fb.f, fa.f);
        }
        return false;
      }
      return true;
    }
  }
};

///////////////////////////////////////////////////////////////////////////////

template <typename S, typename D>
struct muladd_t {
  using stype = typename S::dtype;
  using dtype = typename D::dtype;
  static dtype eval(stype a, stype b, dtype c) {
    return static_cast<dtype>(a) * static_cast<dtype>(b) + c;
  }
};

template <>
struct muladd_t<vt::fp16, vt::fp32> {
  static float eval(uint16_t a, uint16_t b, float c) {
    auto fa = bit_cast<float>(rv_htof_s(a, 0, nullptr));
    auto fb = bit_cast<float>(rv_htof_s(b, 0, nullptr));
    return fa * fb + c;
  }
};

template <>
struct muladd_t<vt::bf16, vt::fp32> {
  static float eval(uint16_t a, uint16_t b, float c) {
    auto fa = bit_cast<float>(rv_btof_s(a, 0, nullptr));
    auto fb = bit_cast<float>(rv_btof_s(b, 0, nullptr));
    return fa * fb + c;
  }
};

template <>
struct muladd_t<vt::fp8, vt::fp32> {
  static float eval(uint8_t a, uint8_t b, float c) {
    auto fa = bit_cast<float>(rv_e4m3tof_s(a, 0, nullptr));
    auto fb = bit_cast<float>(rv_e4m3tof_s(b, 0, nullptr));
    return fa * fb + c;
  }
};

template <>
struct muladd_t<vt::bf8, vt::fp32> {
  static float eval(uint8_t a, uint8_t b, float c) {
    auto fa = bit_cast<float>(rv_e5m2tof_s(a, 0, nullptr));
    auto fb = bit_cast<float>(rv_e5m2tof_s(b, 0, nullptr));
    return fa * fb + c;
  }
};

template <>
struct muladd_t<vt::tf32, vt::fp32> {
  static float eval(uint32_t a, uint32_t b, float c) {
    auto fa = bit_cast<float>(rv_tf32tof_s(a, 0, nullptr));
    auto fb = bit_cast<float>(rv_tf32tof_s(b, 0, nullptr));
    return fa * fb + c;
  }
};

template <>
struct muladd_t<vt::int4, vt::int32> {
  static int32_t eval(uint8_t a, uint8_t b, int32_t c) {
    int32_t a_val = a & 0xF;
    if (a & 0x8) {
      a_val |= 0xFFFFFFF0; // sign extend
    }
    int32_t b_val = b & 0xF;
    if (b & 0x8) {
      b_val |= 0xFFFFFFF0; // sign extend
    }
    return a_val * b_val + c;
  }
};

template <>
struct muladd_t<vt::uint4, vt::int32> {
  static int32_t eval(uint8_t a, uint8_t b, int32_t c) {
    int32_t a_val = a & 0xF;
    int32_t b_val = b & 0xF;
    return a_val * b_val + c;
  }
};

template<typename T>
inline typename T::dtype generate_A_value() {
  return Comparator<T>::generate();
}

template<typename T>
inline typename T::dtype generate_B_value() {
  return Comparator<T>::generate();
}

///////////////////////////////////////////////////////////////////////////////

static_assert(vt::tcu_output_valid(vt::ITYPE::id, vt::OTYPE::id),
              "TCU output type must be fp32 (int32 for integer inputs)");

using cfg = vt::wmma_config_t<VX_CFG_NUM_THREADS, vt::ITYPE, vt::OTYPE>;

using itype_t = typename vt::ITYPE::dtype;
using otype_t = typename vt::OTYPE::dtype;


static void matmul_cpu(otype_t *C, const itype_t *A, const itype_t *B, uint32_t M, uint32_t N, uint32_t K) {
  uint32_t subbytes = 8 / vt::ITYPE::bits;
  uint32_t KS = subbytes ? (K * subbytes) : K;
  for (uint32_t m = 0; m < M; ++m) {
    for (uint32_t n = 0; n < N; ++n) {
      if constexpr (std::is_same<vt::OTYPE, vt::fp32>::value) {
        // The tensor core accumulates the K products in a wide accumulator
        // and rounds to fp32 once; a per-step-rounded reference drifts by
        // several ULP over K. Each product is exact in fp32, so a double
        // accumulation reproduces the single-rounding dot product.
        double acc = 0.0;
        for (uint32_t k = 0; k < KS; ++k) {
          auto a = data_accessor_t<vt::ITYPE>::read(A, m * KS + k);
          auto b = data_accessor_t<vt::ITYPE>::read(B, k * N + n);
          acc += static_cast<double>(muladd_t<vt::ITYPE, vt::OTYPE>::eval(a, b, otype_t(0)));
        }
        data_accessor_t<vt::OTYPE>::write(C, m * N + n, static_cast<otype_t>(acc));
      } else {
        // int32 output (integer inputs): exact accumulation.
        otype_t sum(0);
        for (uint32_t k = 0; k < KS; ++k) {
          auto a = data_accessor_t<vt::ITYPE>::read(A, m * KS + k);
          auto b = data_accessor_t<vt::ITYPE>::read(B, k * N + n);
          sum = muladd_t<vt::ITYPE, vt::OTYPE>::eval(a, b, sum);
        }
        data_accessor_t<vt::OTYPE>::write(C, m * N + n, sum);
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////


static_assert(BW_TILES >= 1 && BW_TILES <= MTILES, "BW_TILES must be 1..MTILES");
static_assert(vt::ITYPE::bits >= 8, "the chunk layout needs whole-byte elements");

const char *kernel_file = "kernel.vxbin";

uint32_t xm = 0;           // 0: MTILES * tileM
uint32_t xn = 64;
uint32_t xk = 64;
uint32_t xw = 4;          // warps per CTA
uint32_t xc = 0;          // K elements per chunk (0: about 4 KB per chunk)
uint32_t xs = STAGE_LOAD; // staging mode
uint32_t xd = 2;          // chunk buffers per CTA in dxa mode
int      xb = -1;         // cores of the memory-bound role (-1: no split, every CTA multiplies MTILES tiles)
uint32_t xt = BW_TILES;   // A tiles a CTA of the memory-bound role multiplies: the kernel's BW_TILES
uint32_t xr = 1;          // rounds: the grid is this many times the N / cta_cols CTAs of one round
uint32_t xR = 0;          // B columns the compute-bound CTAs share (0: each its own)
uint32_t xo = ROLE_NONE;  // the role that runs alone (the other's CTAs do no work)
bool     xP = false;      // print each CTA's core, walk begin, end and cycles

vx_device_h device = nullptr;
vx_buffer_h A_buffer = nullptr;
vx_buffer_h B_buffer = nullptr;
vx_buffer_h C_buffer = nullptr;
vx_buffer_h core_buffer = nullptr;
#ifdef PROFILE_ENABLE
vx_buffer_h cycles_buffer = nullptr;
#endif
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex block-staged GEMM TCU Test." << std::endl;
  std::cout << "Usage: [-m M] [-n N] [-k K] [-w warps per CTA] [-c K elements per chunk] "
               "[-s 0|1 staging: load, dxa] [-d chunk buffers 1.." << MAX_DEPTH << " (dxa)] "
               "[-b cores of the memory-bound role] [-t its A tiles: " << BW_TILES << "] "
               "[-r rounds of N columns] [-R B columns the compute-bound CTAs share] "
               "[-o 1|2 run the memory-bound|compute-bound role alone] "
               "[-P print every CTA] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "m:n:k:w:c:s:d:b:t:r:R:o:Ph")) != -1) {
    switch (c) {
    case 'm':
      xm = atoi(optarg);
      break;
    case 'n':
      xn = atoi(optarg);
      break;
    case 'k':
      xk = atoi(optarg);
      break;
    case 'w':
      xw = atoi(optarg);
      break;
    case 'c':
      xc = atoi(optarg);
      break;
    case 's':
      xs = atoi(optarg);
      break;
    case 'd':
      xd = atoi(optarg);
      break;
    case 'b':
      xb = atoi(optarg);
      break;
    case 't':
      xt = atoi(optarg);
      break;
    case 'r':
      xr = atoi(optarg);
      break;
    case 'R':
      xR = atoi(optarg);
      break;
    case 'o':
      xo = atoi(optarg);
      break;
    case 'P':
      xP = true;
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
    if (core_buffer) vx_buffer_release(core_buffer);
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

// reports an invalid option combination and exits
static void reject(const std::string& msg) {
  std::cout << "Error: " << msg << "!" << std::endl;
  cleanup();
  exit(-1);
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  std::srand(50);

  std::cout << "open device connection" << std::endl;
  RT_CHECK(vx_device_open(0, &device));

  // create the command queue used for the async enqueue chain
  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t isa_flags;
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  if ((isa_flags & VX_ISA_EXT_TCU) == 0)
    reject("TCU extension not supported");
  if (xs > STAGE_DXA)
    reject("-s must be 0 (load) or 1 (dxa)");
  if (xs == STAGE_DXA && (isa_flags & VX_ISA_EXT_DXA) == 0)
    reject("-s 1 needs the DXA extension");

  uint64_t NT, num_warps, num_cores, lmem_bytes;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &NT));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_device_query(device, VX_CAPS_LOCAL_MEM_SIZE, &lmem_bytes));
  if (NT != VX_CFG_NUM_THREADS)
    reject("device warp size (" + std::to_string(NT) + ") must match VX_CFG_NUM_THREADS=" + std::to_string(VX_CFG_NUM_THREADS));

  uint32_t M = xm ? xm : MTILES * cfg::tileM;
  uint32_t N = xn;
  uint32_t K = xk;
  uint32_t W = xw;
  // the load copy uses one buffer; the DXA runs `depth` chunks ahead
  uint32_t depth = (xs == STAGE_DXA) ? xd : 1;

  if (W < 1 || W > num_warps)
    reject("-w must be 1.." + std::to_string(num_warps) + " warps");
  uint32_t cta_cols = W * cfg::tileN;
  uint32_t chunk_k = xc;
  if (chunk_k == 0) {
    uint32_t tiles = 4096 / (cta_cols * cfg::tileK * sizeof(itype_t));
    chunk_k = std::max(tiles, 1u) * cfg::tileK;
    while (chunk_k > cfg::tileK && (K % chunk_k) != 0)
      chunk_k -= cfg::tileK;
  }

  if (M != MTILES * cfg::tileM)
    reject("M must be MTILES * tileM = " + std::to_string(MTILES * cfg::tileM));
  if ((N % cta_cols) != 0)
    reject("N must be a multiple of warps * tileN = " + std::to_string(cta_cols));
  if ((chunk_k % cfg::tileK) != 0)
    reject("-c must be a multiple of tensor tileK=" + std::to_string(cfg::tileK));
  if ((K % chunk_k) != 0)
    reject("K must be a multiple of the chunk's K elements (" + std::to_string(chunk_k) + ")");
  if (depth < 1 || depth > MAX_DEPTH)
    reject("-d must be 1.." + std::to_string(MAX_DEPTH));
  if (xt != BW_TILES)
    reject("-t must be the BW_TILES the kernel was built with, " + std::to_string(BW_TILES));
  if (xb < -1 || xb > (int)num_cores)
    reject("-b must be 0.." + std::to_string(num_cores) + ", the device's cores");
  bool split = (xb >= 0);
  uint32_t bw_cores = split ? xb : 0;
  // the load copy moves 8-byte words: a column's chunk must be whole words in B
  if ((chunk_k * sizeof(itype_t)) % 8 != 0 || (K * sizeof(itype_t)) % 8 != 0)
    reject("K and -c must be a whole number of 8-byte words");
  if ((cta_cols * chunk_k * sizeof(itype_t)) % (8 * W * NT) != 0)
    reject("a chunk must be a whole number of 8-byte words per thread of the CTA");
  // the DXA descriptor holds a tile's dimensions in 16 bits
  if (xs == STAGE_DXA && (chunk_k > 0xffff || cta_cols > 0xffff))
    reject("a DXA chunk is at most 65535 elements a side");

  if (xr < 1)
    reject("-r must be at least 1 round");
  if ((xR % cta_cols) != 0 || xR > N)
    reject("-R must be a multiple of warps * tileN = " + std::to_string(cta_cols) + " columns, at most N");
  if (xo != ROLE_NONE && (xo > ROLE_CP || !split || (xo == ROLE_BW ? bw_cores == 0 : bw_cores == num_cores)))
    reject("-o must be 1 or 2, a role that -b gives a core");

  uint64_t chunk_bytes = (uint64_t)cta_cols * chunk_k * sizeof(itype_t);
  uint64_t local_mem = depth * chunk_bytes;
  if (local_mem > lmem_bytes)
    reject("a CTA's " + std::to_string(depth) + " chunk buffer(s) of " + std::to_string(chunk_bytes)
           + " bytes exceed the device's " + std::to_string(lmem_bytes) + " bytes of local memory");
  RT_CHECK(vx_check_occupancy(device, W * (uint32_t)NT, (uint32_t)local_mem));

  // The grid is xr rounds of the N / cta_cols CTAs that cover B once. B has
  // its N columns on the device whatever the rounds (the runtime faults on a
  // buffer of some 40 MB): CTA b stages the columns of CTA b % round_ctas, so
  // a round reads all of B again, and B must be several times the L2 for a
  // round to find none of it cached. C has NC columns: every CTA stores its
  // own, and each is checked against the reference of the columns it staged.
  uint32_t round_ctas = N / cta_cols;
  uint32_t NC = N * xr;
  size_t sizeA = M * K;
  size_t sizeB = (size_t)K * N;
  size_t sizeC = (size_t)M * NC;
  uint32_t grid_dim[2]  = {NC / cta_cols, 1};
  uint32_t block_dim[2] = {W * (uint32_t)NT, 1};

  std::cout << "input data type: " << vt::ITYPE::name << " (id=" << vt::ITYPE::id << ")" << std::endl;
  std::cout << "output data type: " << vt::OTYPE::name << " (id=" << vt::OTYPE::id << ")" << std::endl;
  std::cout << "WMMA Core Dimension: M=" << cfg::tcM << ", N=" << cfg::tcN << ", K=" << cfg::tcK << std::endl;
  std::cout << "WMMA Tile Dimension: M=" << cfg::tileM << ", N=" << cfg::tileN << ", K=" << cfg::tileK << std::endl;
  std::cout << "matrix A: " << M << "x" << K << std::endl;
  std::cout << "matrix B: " << K << "x" << N << std::endl;
  std::cout << "matrix C: " << M << "x" << N << std::endl;
  if (xr != 1)
    std::cout << "rounds: " << xr << " (" << NC << " columns of C)" << std::endl;
  // CTAs one SM holds at once: by its warps, and by its local memory
  uint64_t slots = std::min<uint64_t>(num_warps / W, lmem_bytes / local_mem);
  printf("staging=%s grid=%u warps_per_cta=%u cta_cols=%u chunk_k=%u chunk_bytes=%llu depth=%u "
         "lmem_per_cta=%llu cta_slots_per_core=%llu\n",
         (xs == STAGE_DXA) ? "dxa" : "load", grid_dim[0], W, cta_cols, chunk_k,
         (unsigned long long)chunk_bytes, depth, (unsigned long long)local_mem,
         (unsigned long long)slots);

  kernel_arg.M = M;
  kernel_arg.N = NC;
  kernel_arg.K = K;
  kernel_arg.chunk_k = chunk_k;
  kernel_arg.stage = xs;
  kernel_arg.depth = depth;
  kernel_arg.bw_cores = bw_cores;
  kernel_arg.bw_wrap = round_ctas;
  kernel_arg.cp_wrap = xR ? xR / cta_cols : round_ctas;
  kernel_arg.idle_role = (xo == ROLE_BW) ? ROLE_CP : (xo == ROLE_CP) ? ROLE_BW : ROLE_NONE;
  kernel_arg.last_cta = grid_dim[0] - 1;
  // An idle core takes `slots` CTAs and the launch lane's two more, and holds
  // them until the last CTA is done: the last one must not be among them.
  if (xo != ROLE_NONE && grid_dim[0] <= num_cores * (slots + 2))
    reject("-o needs a grid of more than " + std::to_string(num_cores * (slots + 2)) + " CTAs (-r)");

  std::cout << "allocate device memory" << std::endl;
  RT_CHECK(vx_buffer_create(device, sizeA * sizeof(itype_t), VX_MEM_READ, &A_buffer));
  RT_CHECK(vx_buffer_address(A_buffer, &kernel_arg.A_addr));
  RT_CHECK(vx_buffer_create(device, sizeB * sizeof(itype_t), VX_MEM_READ, &B_buffer));
  RT_CHECK(vx_buffer_address(B_buffer, &kernel_arg.B_addr));
  RT_CHECK(vx_buffer_create(device, sizeC * sizeof(otype_t), VX_MEM_WRITE, &C_buffer));
  RT_CHECK(vx_buffer_address(C_buffer, &kernel_arg.C_addr));

  uint32_t num_blocks = grid_dim[0] * grid_dim[1];
  RT_CHECK(vx_buffer_create(device, num_blocks * sizeof(uint32_t), VX_MEM_READ_WRITE, &core_buffer));
  RT_CHECK(vx_buffer_address(core_buffer, &kernel_arg.core_addr));

#ifdef PROFILE_ENABLE
  // per warp: the cycles of its K walk, then the cycle each began
  uint32_t num_counts = num_blocks * W;
  RT_CHECK(vx_buffer_create(device, 2 * num_counts * sizeof(uint32_t), VX_MEM_WRITE, &cycles_buffer));
  RT_CHECK(vx_buffer_address(cycles_buffer, &kernel_arg.cycles_addr));
  kernel_arg.begin_addr = kernel_arg.cycles_addr + num_counts * sizeof(uint32_t);
#endif

  std::cout << "A_addr=0x" << std::hex << kernel_arg.A_addr << std::endl;
  std::cout << "B_addr=0x" << std::hex << kernel_arg.B_addr << std::endl;
  std::cout << "C_addr=0x" << std::hex << kernel_arg.C_addr << std::dec << std::endl;

  // generate source data
  std::vector<itype_t> h_A(sizeA);
  std::vector<itype_t> h_B(sizeB);
  for (uint32_t i = 0; i < sizeA; ++i) {
    h_A[i] = generate_A_value<vt::ITYPE>();
  }
  for (uint32_t i = 0; i < sizeB; ++i) {
    h_B[i] = generate_B_value<vt::ITYPE>();
  }

  std::cout << "upload matrix A buffer" << std::endl;
  RT_CHECK(vx_enqueue_write(queue, A_buffer, 0, h_A.data(),
                            sizeA * sizeof(itype_t), 0, nullptr, nullptr));

  // B is stored col-major: the K weights of a column are contiguous.
  // h_B_col must outlive the async write below.
  std::vector<itype_t> h_B_col(sizeB);
  std::cout << "upload matrix B buffer" << std::endl;
  convert_row_to_col_major(h_B_col.data(), h_B.data(), K, N);
  RT_CHECK(vx_enqueue_write(queue, B_buffer, 0, h_B_col.data(),
                            sizeB * sizeof(itype_t), 0, nullptr, nullptr));

  // a CTA that did not run leaves no core behind
  std::vector<uint32_t> h_core(num_blocks, UINT32_MAX);
  RT_CHECK(vx_enqueue_write(queue, core_buffer, 0, h_core.data(),
                            num_blocks * sizeof(uint32_t), 0, nullptr, nullptr));

  if (xs == STAGE_DXA) {
    // A chunk is chunk_k elements along K (contiguous in B) by the CTA's
    // columns, K elements apart; the row-major destination is [column][k].
    RT_CHECK(vortex::dxa::program_2d(device, 0, kernel_arg.B_addr,
      /*size0=*/K, /*size1=*/N,
      /*stride0_bytes=*/K * sizeof(itype_t),
      /*tile0=*/chunk_k, /*tile1=*/cta_cols,
      /*elem_bytes=*/sizeof(itype_t)));
  }

  std::cout << "load kernel module" << std::endl;
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  // Host result buffers — must outlive the async reads enqueued below.
  std::vector<otype_t> h_C(sizeC);
#ifdef PROFILE_ENABLE
  std::vector<uint32_t> h_cycles(2 * num_counts);
#endif

  // args passed as a host blob (UVA)
  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 2;
  li.grid_dim[0]  = grid_dim[0];
  li.grid_dim[1]  = grid_dim[1];
  li.block_dim[0] = block_dim[0];
  li.block_dim[1] = block_dim[1];
  li.lmem_size    = (uint32_t)local_mem;

  std::vector<otype_t> h_ref((size_t)M * N);
  matmul_cpu(h_ref.data(), h_A.data(), h_B.data(), M, N, K);

  auto time_start = std::chrono::high_resolution_clock::now();

  std::cout << "launch kernel" << std::endl;
  vx_event_h launch_ev = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));

  // read results back — chained after the launch on the same queue
  std::cout << "download destination buffer" << std::endl;
  vx_event_h read_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_C.data(), C_buffer, 0,
                           sizeC * sizeof(otype_t), 1, &launch_ev, &read_ev));
  vx_event_h core_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_core.data(), core_buffer, 0,
                           num_blocks * sizeof(uint32_t), 1, &read_ev, &core_ev));
#ifdef PROFILE_ENABLE
  vx_event_h cyc_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_cycles.data(), cycles_buffer, 0,
                           2 * num_counts * sizeof(uint32_t), 1, &core_ev, &cyc_ev));
#endif

  std::cout << "wait for completion" << std::endl;
#ifdef PROFILE_ENABLE
  RT_CHECK(vx_event_wait_value(cyc_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(cyc_ev);
#else
  RT_CHECK(vx_event_wait_value(core_ev, 1, VX_TIMEOUT_INFINITE));
#endif
  vx_event_release(core_ev);
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  auto time_end = std::chrono::high_resolution_clock::now();
  double elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(time_end - time_start).count();
  printf("Elapsed time: %lg ms\n", elapsed);

#ifdef PROFILE_ENABLE
  // report TCU cycle counts
  {
    uint32_t max_cycles = 0;
    for (uint32_t i = 0; i < num_counts; ++i) {
      max_cycles = std::max(max_cycles, h_cycles[i]);
    }
    printf("TCU_CYCLES: max=%u (across %u blocks)\n", max_cycles, num_blocks);
  }
#endif

  // the role of a CTA is the role of the core it ran on
  auto is_bw = [&](uint32_t b) { return h_core[b] < bw_cores; };
  // a CTA that did not run, or one of the role that sits a solo run out
  auto idle = [&](uint32_t b) {
    return h_core[b] >= num_cores || (xo != ROLE_NONE && xo != (uint32_t)(is_bw(b) ? ROLE_BW : ROLE_CP));
  };

  if (split) {
    uint64_t cta_weight_bytes = (uint64_t)cta_cols * K * sizeof(itype_t);
    uint32_t ctas[2] = {0, 0}; // [0] memory-bound role, [1] compute-bound role
    std::vector<uint32_t> core_ctas(num_cores, 0);
    for (uint32_t b = 0; b < num_blocks; ++b) {
      if (h_core[b] >= num_cores)
        continue;
      ++ctas[is_bw(b) ? 0 : 1];
      ++core_ctas[h_core[b]];
    }
    printf("roles: bw_cores=%u bw_tiles=%u bw_ctas=%u bw_weight_bytes=%llu cp_cores=%u cp_tiles=%u "
           "cp_ctas=%u cp_weight_bytes=%llu ctas_per_core=",
           bw_cores, xt, ctas[0], (unsigned long long)(ctas[0] * cta_weight_bytes),
           (uint32_t)num_cores - bw_cores, (uint32_t)MTILES, ctas[1],
           (unsigned long long)(ctas[1] * cta_weight_bytes));
    for (uint32_t c = 0; c < num_cores; ++c) {
      printf("%s%u", c ? "," : "", core_ctas[c]);
    }
    printf("\n");
#ifdef PROFILE_ENABLE
    // A CTA's K walk runs from its first warp's start to its last warp's end.
    // A core counts cycles only while it has work, and every core is handed
    // its first CTA within a few cycles of the launch, so the cores' counters
    // agree to within those cycles until a core runs out of CTAs.
    struct role_t {
      uint32_t ctas = 0, max = 0, first = UINT32_MAX, last = 0, last_begin = 0;
      uint64_t sum = 0;
    } roles[2];
    std::vector<uint32_t> cta_begin(num_blocks), cta_end(num_blocks), cta_walk(num_blocks);
    for (uint32_t b = 0; b < num_blocks; ++b) {
      if (h_core[b] >= num_cores)
        continue;
      uint32_t cycles = 0, begin = UINT32_MAX, end = 0;
      for (uint32_t w = 0; w < W; ++w) {
        uint32_t c = h_cycles[b * W + w];
        uint32_t t = h_cycles[num_counts + b * W + w];
        cycles = std::max(cycles, c);
        begin = std::min(begin, t);
        end = std::max(end, t + c);
      }
      cta_begin[b] = begin;
      cta_end[b] = end;
      cta_walk[b] = cycles;
      if (xP)
        printf("CTA %u core=%u begin=%u end=%u walk=%u\n", b, h_core[b], begin, end, cycles);
      auto& r = roles[is_bw(b) ? 0 : 1];
      ++r.ctas;
      if (idle(b))
        continue;
      r.max = std::max(r.max, cycles);
      r.sum += cycles;
      r.first = std::min(r.first, begin);
      r.last = std::max(r.last, end);
      r.last_begin = std::max(r.last_begin, begin);
    }
    const char* names[2] = {"BW", "CP"};
    for (int i = 0; i < 2; ++i) {
      auto& r = roles[i];
      printf("TCU_CYCLES_%s: max=%u mean=%.0f ctas=%u\n", names[i], r.max,
             r.ctas ? (double)r.sum / r.ctas : 0.0, r.ctas);
    }
    if (xo == ROLE_NONE && roles[0].ctas && roles[1].ctas) {
      // the window in which CTAs of both roles are running
      uint32_t lo = std::max(roles[0].first, roles[1].first);
      uint32_t hi = std::min(roles[0].last, roles[1].last);
      uint32_t inside[2] = {0, 0};
      for (uint32_t b = 0; b < num_blocks; ++b) {
        if (!idle(b) && cta_begin[b] >= lo && cta_end[b] <= hi)
          ++inside[is_bw(b) ? 0 : 1];
      }
      printf("roles_time: bw_begin=%u bw_end=%u cp_begin=%u cp_end=%u both_running=%u "
             "bw_ctas_inside=%u/%u cp_ctas_inside=%u/%u\n",
             roles[0].first, roles[0].last, roles[1].first, roles[1].last, (hi > lo) ? hi - lo : 0,
             inside[0], roles[0].ctas, inside[1], roles[1].ctas);
    }
    // The work each role completed in a window of the run, and its rates over
    // the window. With both roles running the window is the one above. A role
    // running alone is measured from its first CTA's start to its last CTA's
    // start: until then the grid had CTAs left, so its cores were full.
    // `inside` counts the CTAs that began and ended in the window. `overlap`
    // counts every CTA by the share of its K walk that fell in the window, so
    // the CTAs under way at either edge count for the part they did there.
    uint32_t lo = 0, hi = 0;
    if (xo == ROLE_NONE) {
      lo = std::max(roles[0].first, roles[1].first);
      hi = std::min(roles[0].last, roles[1].last);
    } else {
      auto& r = roles[xo == ROLE_BW ? 0 : 1];
      lo = r.first;
      hi = r.last_begin;
    }
    uint32_t run_lo = UINT32_MAX, run_hi = 0;
    for (int i = 0; i < 2; ++i) {
      if (xo == ROLE_NONE || xo == (uint32_t)(i ? ROLE_CP : ROLE_BW)) {
        run_lo = std::min(run_lo, roles[i].first);
        run_hi = std::max(run_hi, roles[i].last);
      }
    }
    auto report = [&](const char* tag, uint32_t lo, uint32_t hi) {
      if (hi <= lo)
        return;
      double window = hi - lo;
      printf("rate%s_window: %s begin=%u end=%u cycles=%u run_cycles=%u share_of_run=%.3f\n", tag,
             (xo == ROLE_NONE) ? "both" : (xo == ROLE_BW) ? "solo_BW" : "solo_CP",
             lo, hi, hi - lo, run_hi - run_lo, window / (run_hi - run_lo));
      for (int i = 0; i < 2; ++i) {
        if (xo != ROLE_NONE && xo != (uint32_t)(i ? ROLE_CP : ROLE_BW))
          continue;
        uint64_t cta_mults = (uint64_t)W * (K / cfg::tileK) * (i ? MTILES : xt);
        uint32_t inside = 0;
        double overlap = 0, running = 0;
        for (uint32_t b = 0; b < num_blocks; ++b) {
          if (idle(b) || (is_bw(b) ? 0 : 1) != i)
            continue;
          if (cta_begin[b] >= lo && cta_end[b] <= hi)
            ++inside;
          uint32_t from = std::max(cta_begin[b], lo);
          uint32_t to = std::min(cta_end[b], hi);
          if (to > from) {
            running += to - from;
            overlap += (double)(to - from) / (cta_end[b] - cta_begin[b]);
          }
        }
        printf("rate%s_%s: ctas_inside=%u bytes=%llu mults=%llu bytes_per_cycle=%.3f mults_per_cycle=%.5f "
               "ctas_overlap=%.2f overlap_bytes_per_cycle=%.3f overlap_mults_per_cycle=%.5f ctas_running=%.2f\n",
               tag, names[i], inside, (unsigned long long)(inside * cta_weight_bytes),
               (unsigned long long)(inside * cta_mults),
               inside * cta_weight_bytes / window, inside * cta_mults / window,
               overlap, overlap * cta_weight_bytes / window, overlap * cta_mults / window,
               running / window);
      }
    };
    report("", lo, hi);
    // The same without each role's first generation, the CTAs that began
    // before any CTA of the role ended. Those start within a few thousand
    // cycles of each other and walk K in step, which is faster than the mix
    // of phases every later CTA runs in; a window that holds them reads high
    // by the share of it they take.
    uint32_t steady = lo;
    for (int i = 0; i < 2; ++i) {
      if (xo != ROLE_NONE && xo != (uint32_t)(i ? ROLE_CP : ROLE_BW))
        continue;
      uint32_t first_end = UINT32_MAX;
      for (uint32_t b = 0; b < num_blocks; ++b) {
        if (!idle(b) && (is_bw(b) ? 0 : 1) == i)
          first_end = std::min(first_end, cta_end[b]);
      }
      for (uint32_t b = 0; b < num_blocks; ++b) {
        if (!idle(b) && (is_bw(b) ? 0 : 1) == i && cta_begin[b] < first_end)
          steady = std::max(steady, cta_end[b]);
      }
    }
    report("_steady", steady, hi);
    // The walk cycles of each core's CTAs, without the core's first generation
    // (the CTAs that began before its first CTA ended), as quartiles: a
    // comparison of runs that does not rest on the CTAs one window holds.
    for (uint32_t c = 0; c < num_cores; ++c) {
      const bool bw = c < bw_cores;
      if (xo != ROLE_NONE && xo != (bw ? ROLE_BW : ROLE_CP))
        continue;
      uint32_t first_end = UINT32_MAX, all = 0;
      for (uint32_t b = 0; b < num_blocks; ++b) {
        if (h_core[b] == c && !idle(b)) {
          first_end = std::min(first_end, cta_end[b]);
          ++all;
        }
      }
      std::vector<uint32_t> w;
      for (uint32_t b = 0; b < num_blocks; ++b) {
        if (h_core[b] == c && !idle(b) && cta_begin[b] >= first_end)
          w.push_back(cta_walk[b]);
      }
      if (w.empty())
        continue;
      std::sort(w.begin(), w.end());
      auto q = [&](double f) { return w[std::min<size_t>(w.size() - 1, (size_t)(f * (w.size() - 1) + 0.5))]; };
      printf("walk_%s: core=%u ctas=%u steady_ctas=%zu min=%u q1=%u median=%u q3=%u max=%u\n",
             names[bw ? 0 : 1], c, all, w.size(), w.front(), q(0.25), q(0.5), q(0.75), w.back());
    }
#endif
  }

  // A CTA stores the tiles of its role only: the rows of the others are not checked.
  std::cout << "verify result" << std::endl;
  int errors = 0;
  for (uint32_t b = 0; b < num_blocks; ++b) {
    if (h_core[b] >= num_cores) {
      if (errors < MAX_ERRORS) {
        printf("*** error: CTA %u reported core %u of %u\n", b, h_core[b], (uint32_t)num_cores);
      }
      ++errors;
      continue;
    }
    // an idle CTA stores nothing
    uint32_t rows = idle(b) ? 0 : (is_bw(b) ? xt : MTILES) * cfg::tileM;
    // the CTA of the first round whose columns of B this one staged
    uint32_t src = b % (is_bw(b) ? kernel_arg.bw_wrap : kernel_arg.cp_wrap);
    for (uint32_t m = 0; m < rows; ++m) {
      for (uint32_t c = 0; c < cta_cols; ++c) {
        size_t i = (size_t)m * NC + b * cta_cols + c;
        size_t r = (size_t)m * N + src * cta_cols + c;
        if (!Comparator<vt::OTYPE>::compare(h_C[i], h_ref[r], i, errors)) {
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
    return errors != 0;
  }

  std::cout << "PASSED!" << std::endl;

  return 0;
}
