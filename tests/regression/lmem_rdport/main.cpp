#include "common.h"
#include <algorithm>
#include <dxa.h>
#include <iostream>
#include <string.h>
#include <tensor_cfg.h>
#include <unistd.h>
#include <vector>
#include <vortex2.h>

#define MAX_ERRORS 20

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

using cfg = vt::wgmma_config_t<VX_CFG_NUM_THREADS, vt::fp16, vt::fp32, 32>;

const char *kernel_file = "kernel.vxbin";
uint32_t mode   = MODE_RACE;
uint32_t groups = 4;    // column groups of a sub-region, a multiple of 4
uint32_t copies = 8;
uint32_t reps   = 48;
uint32_t gens   = 192;  // DXA generations; the slower stores and atomics do a fraction
uint32_t N      = 0;    // 0: one fragment row, so an op is mostly operand reads
uint32_t ops    = 64;
uint32_t sweeps = 8;
uint32_t outlast = 6;
uint32_t span   = 16;
uint32_t conflict = 1;
uint32_t use_cp = 0;
uint32_t cta_warps = 0; // 0: one per TCU block
// With both sides saturated a row read gets every other cycle of the port
// and a read pass waits a cycle per row read, so the tensor work keeps half
// its rate at worst and the loads most of theirs.
uint32_t t_floor_pct = 40;
uint32_t l_floor_pct = 80;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h out_buffer = nullptr;
vx_buffer_h stat_buffer = nullptr;
vx_queue_h  queue = nullptr;
vx_module_h module_ = nullptr;
vx_kernel_h kernel = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
  std::cout << "Vortex local-memory row read port test." << std::endl;
  std::cout << "Usage: [-m mode: 0=race 1=starve] race: [-g column groups per sub-region] [-c copies] [-r tc.cp per copy]"
               " [-w generations] starve: [-n tile N] [-i tensor ops] [-s sweeps] [-k times the fixed work the other side runs] [-l loads per sweep]"
               " [-x 0|1: lanes in one bank] [-t 0=tc.mma 1=tc.cp] [-W warps in the CTA] [-f tensor floor, percent of the solo rate] [-F loads floor] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "m:g:c:r:w:n:i:s:k:l:x:t:W:f:F:h")) != -1) {
    switch (c) {
    case 'm': mode = atoi(optarg); break;
    case 'g': groups = atoi(optarg); break;
    case 'c': copies = atoi(optarg); break;
    case 'r': reps = atoi(optarg); break;
    case 'w': gens = atoi(optarg); break;
    case 'n': N = atoi(optarg); break;
    case 'i': ops = atoi(optarg); break;
    case 's': sweeps = atoi(optarg); break;
    case 'k': outlast = atoi(optarg); break;
    case 'l': span = atoi(optarg); break;
    case 'x': conflict = atoi(optarg); break;
    case 't': use_cp = atoi(optarg); break;
    case 'W': cta_warps = atoi(optarg); break;
    case 'f': t_floor_pct = atoi(optarg); break;
    case 'F': l_floor_pct = atoi(optarg); break;
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
    if (out_buffer) vx_buffer_release(out_buffer);
    if (stat_buffer) vx_buffer_release(stat_buffer);
    if (kernel)  vx_kernel_release(kernel);
    if (module_) vx_module_release(module_);
    if (queue)   vx_queue_release(queue);
    vx_device_dump_perf(device, stdout);
    vx_device_release(device);
  }
}

static uint32_t enc(uint32_t gen, uint32_t idx) {
  return VAL_BASE | (gen << GEN_SHIFT) | idx;
}

static int errors = 0;
static void error(const char* what, uint32_t copy, uint32_t pos, uint32_t idx, uint32_t value) {
  if (errors < MAX_ERRORS) {
    printf("*** error: %s: copy=%u pos=%u word=%u value=0x%08x\n", what, copy, pos, idx, value);
  }
  ++errors;
}

// Every word the tensor engine copied is some generation of that word, never
// older than the copy before saw; a row the DXA writes is of one generation;
// the last copy, taken behind the writers, has the final generations.
static void check_race(const std::vector<uint32_t>& h_out, uint32_t copy_words, uint32_t sub_words,
                       const uint32_t last_gen[NUM_SUBS]) {
  const uint32_t words = NUM_SUBS * sub_words;
  const uint32_t row_words = VX_CFG_LMEM_NUM_BANKS * (VX_CFG_XLEN / 8) / 4;
  const uint32_t num_copies = copies + 2;
  std::vector<uint32_t> idx_of(copy_words);
  std::vector<bool> seen(words, false);
  for (uint32_t p = 0; p < copy_words; ++p) {
    uint32_t v = h_out[p];
    uint32_t idx = v & IDX_MASK;
    if (v != enc(0, idx) || idx >= words || seen[idx]) {
      error("copy 0 is not the initial region", 0, p, idx, v);
      idx = 0;
    }
    seen[idx] = true;
    idx_of[p] = idx;
  }
  if (errors != 0) {
    return;
  }
  std::vector<uint32_t> prev(copy_words, 0);
  std::vector<uint32_t> row_gen(words / row_words);
  uint32_t moved[NUM_SUBS] = {}, partial[NUM_SUBS] = {};
  for (uint32_t c = 1; c < num_copies; ++c) {
    std::fill(row_gen.begin(), row_gen.end(), UINT32_MAX);
    bool mid[NUM_SUBS] = {};
    for (uint32_t p = 0; p < copy_words; ++p) {
      uint32_t v = h_out[c * copy_words + p];
      uint32_t idx = idx_of[p];
      uint32_t sub = idx / sub_words;
      uint32_t gen = (v - VAL_BASE) >> GEN_SHIFT;
      if ((v & IDX_MASK) != idx || v < VAL_BASE || gen > last_gen[sub]) {
        error("not a generation of the word", c, p, idx, v);
        continue;
      }
      if (gen < prev[p]) {
        error("older than the copy before", c, p, idx, v);
      }
      if (sub == SUB_DXA) {
        uint32_t& rg = row_gen[idx / row_words];
        if (rg == UINT32_MAX) {
          rg = gen;
        } else if (rg != gen) {
          error("a DXA row of two generations", c, p, idx, v);
        }
      }
      if (c + 1 == num_copies && gen != last_gen[sub]) {
        error("the last copy is not the last generation", c, p, idx, v);
      }
      moved[sub] += (gen != prev[p]);
      mid[sub] = mid[sub] || (gen != 0 && gen != last_gen[sub]);
      prev[p] = gen;
    }
    for (uint32_t s = 0; s < NUM_SUBS; ++s) {
      partial[s] += mid[s];
    }
  }
  static const char* names[NUM_SUBS] = {"dxa", "store", "amo"};
  for (uint32_t s = 0; s < NUM_SUBS; ++s) {
    printf("RACE: %s gens=%u words_that_moved=%u copies_mid_stream=%u\n", names[s], last_gen[s], moved[s], partial[s]);
    // a writer that never overlapped the copies raced nothing
    if (last_gen[s] != 0 && partial[s] == 0) {
      printf("*** error: no copy saw the %s writer mid-stream\n", names[s]);
      ++errors;
    }
  }
}

// Each side's fixed work is timed alone and beside the other side, which
// must still be running when it ends.
static void check_starve(const std::vector<uint64_t>& st, uint32_t load_warps) {
  const uint64_t* ts = st.data();
  double t_ratio = double(ts[ST_SOLO]) / double(ts[ST_CONT]);
  printf("STARVE: tensor ops=%u solo=%lu beside the loads=%lu cycles, rate ratio=%.3f\n",
         ops, ts[ST_SOLO], ts[ST_CONT], t_ratio);
  // A warp's own time depends on which engine its requests fall to, with or
  // without the tensor work, so the loads are judged together.
  uint64_t l_solo = 0, l_cont = 0;
  for (uint32_t w = 1; w <= load_warps; ++w) {
    const uint64_t* ls = st.data() + w * ST_WORDS;
    double l_ratio = double(ls[ST_SOLO]) / double(ls[ST_CONT]);
    printf("STARVE: load warp %u sweeps=%u solo=%lu beside the tensor ops=%lu cycles, rate ratio=%.3f\n",
           w, sweeps, ls[ST_SOLO], ls[ST_CONT], l_ratio);
    l_solo += ls[ST_SOLO];
    l_cont += ls[ST_CONT];
    if (ls[ST_END_T] < ts[ST_END_T]) {
      printf("*** error: load warp %u ended %lu cycles before the tensor work it runs beside (raise -k)\n",
             w, ts[ST_END_T] - ls[ST_END_T]);
      ++errors;
    }
    if (ts[ST_END_L] < ls[ST_END_L]) {
      printf("*** error: the tensor ops ended %lu cycles before load warp %u's loads (raise -k)\n",
             ls[ST_END_L] - ts[ST_END_L], w);
      ++errors;
    }
  }
  double t_floor = t_floor_pct / 100.0, l_floor = l_floor_pct / 100.0;
  double l_all = double(l_solo) / double(l_cont);
  printf("STARVE: rate ratio tensor=%.3f (floor %.2f) loads=%.3f (floor %.2f)\n", t_ratio, t_floor, l_all, l_floor);
  if (t_ratio < t_floor) {
    printf("*** error: the row reads fell to %.3f of their solo rate beside the loads\n", t_ratio);
    ++errors;
  }
  if (l_all < l_floor) {
    printf("*** error: the loads fell to %.3f of their solo rate beside the row reads\n", l_all);
    ++errors;
  }
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  std::cout << "open device connection" << std::endl;
  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = { sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t NT, issue_width, isa_flags;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &NT));
  RT_CHECK(vx_device_query(device, VX_CAPS_ISSUE_WIDTH, &issue_width));
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  if (NT != VX_CFG_NUM_THREADS) {
    std::cout << "Error: device warp size (" << NT << ") must match VX_CFG_NUM_THREADS=" << VX_CFG_NUM_THREADS << "!" << std::endl;
    cleanup();
    return -1;
  }
  // One warp per TCU block: warp 0 issues the tensor ops, the others write or load.
  uint64_t num_warps;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &num_warps));
  const uint32_t tcu_blocks = issue_width;
  const uint32_t warps = (mode == MODE_STARVE && cta_warps != 0) ? cta_warps : tcu_blocks;
  if (tcu_blocks < 4 || warps < 2 || warps > num_warps || warps > 24) {
    std::cout << "Error: the test needs four TCU blocks (ISSUE_WIDTH >= 4) and a CTA of 2 to 24 of the core's warps!" << std::endl;
    cleanup();
    return -1;
  }
  const bool has_amo = (isa_flags & VX_ISA_STD_A) != 0;

  // tmem geometry, as vx_tmem.h derives it from the warp size
  uint32_t lg = 0;
  while ((1u << (lg + 1)) <= NT) {
    ++lg;
  }
  const uint32_t frag_cols = 1u << (lg / 2);
  const uint32_t block_lanes = 2u << ((lg + 1) / 2);
  const uint32_t row_bytes = VX_CFG_LMEM_NUM_BANKS * (VX_CFG_XLEN / 8);
  // a column group in shared memory: every block's two M steps of NT words
  const uint32_t group_words = tcu_blocks * cfg::m_steps * NT;
  const uint32_t sub_words = groups * group_words;
  const uint32_t cols = NUM_SUBS * groups * frag_cols;
  const uint32_t ld_cols = 8 * NT / block_lanes;
  const uint32_t copy_words = cols * block_lanes;
  const uint32_t last_gen[NUM_SUBS] = {gens, std::max(1u, gens / 8), has_amo ? std::max(1u, gens / 32) : 0};

  size_t out_words = 0, lmem_size = 0;
  uint32_t tmem_cols = 32;
  if (mode == MODE_RACE) {
    if (groups == 0 || (cols % ld_cols) != 0 || (sub_words * 4) % row_bytes != 0 || gens == 0 || gens > MAX_GENS
     || NUM_SUBS * sub_words > IDX_MASK || reps == 0) {
      std::cout << "Error: invalid arguments (column groups a multiple of " << ld_cols / frag_cols
                << ", generations up to " << MAX_GENS << ")!" << std::endl;
      cleanup();
      return -1;
    }
    while (tmem_cols < cols) {
      tmem_cols <<= 1;
    }
    out_words = size_t(copies + 2) * copy_words;
    lmem_size = SLOT_BYTES + NUM_SUBS * sub_words * 4;
    printf("RDPORT_RACE: warps=%u NT=%u row_bytes=%u region_rows=%u cols=%u copies=%u reps=%u gens=%u/%u/%u\n",
           warps, (uint32_t)NT, row_bytes, NUM_SUBS * sub_words * 4 / row_bytes, cols, copies, reps,
           last_gen[0], last_gen[1], last_gen[2]);
  } else if (mode == MODE_STARVE) {
    if (N == 0) {
      N = cfg::xtileN;
    }
    if ((N % cfg::xtileN) != 0 || N == 0 || N / cfg::tcN > 128 || ops == 0 || outlast == 0 || sweeps == 0
     || span == 0 || (span % 8) != 0) {
      std::cout << "Error: invalid arguments (N a multiple of " << cfg::xtileN << " up to " << 128 * cfg::tcN << ")!" << std::endl;
      cleanup();
      return -1;
    }
    while (tmem_cols < N) {
      tmem_cols <<= 1;
    }
    const uint32_t step_bytes = cfg::fedpK * 4;
    const uint32_t ld_words = NT * (conflict ? row_bytes / 4 : 1) + span;
    out_words = warps;
    const uint32_t opnd_bytes = use_cp ? (N / frag_cols) * group_words * 4 : (tcu_blocks * cfg::xtileM + N) * step_bytes;
    lmem_size = SLOT_BYTES + opnd_bytes + ld_words * 4;
    printf("RDPORT_STARVE: warps=%u NT=%u row_bytes=%u N=%u ops=%u(%s) sweeps=%u span=%u conflict=%u\n",
           warps, (uint32_t)NT, row_bytes, N, ops, use_cp ? "tc.cp" : "tc.mma", sweeps, span, conflict);
  } else {
    show_usage();
    cleanup();
    return -1;
  }

  kernel_arg.mode       = mode;
  kernel_arg.sub_words  = sub_words;
  kernel_arg.cols       = cols;
  kernel_arg.copies     = copies;
  kernel_arg.reps       = reps;
  kernel_arg.dxa_gens   = last_gen[SUB_DXA];
  kernel_arg.store_gens = last_gen[SUB_STORE];
  kernel_arg.amo_gens   = last_gen[SUB_AMO];
  kernel_arg.N          = N;
  kernel_arg.ops        = ops;
  kernel_arg.sweeps     = sweeps;
  kernel_arg.span       = span;
  kernel_arg.conflict   = conflict;
  kernel_arg.use_cp     = use_cp;
  kernel_arg.outlast    = outlast;

  std::cout << "allocate device memory" << std::endl;
  const size_t stat_words = warps * ST_WORDS;
  // the DXA's source: one tile of the sub-region per generation
  std::vector<uint32_t> h_src(size_t(gens) * sub_words);
  for (uint32_t g = 0; g < gens; ++g) {
    for (uint32_t i = 0; i < sub_words; ++i) {
      h_src[size_t(g) * sub_words + i] = enc(g + 1, SUB_DXA * sub_words + i);
    }
  }
  uint64_t src_addr = 0;
  RT_CHECK(vx_buffer_create(device, h_src.size() * 4, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_buffer_address(src_buffer, &src_addr));
  RT_CHECK(vx_buffer_create(device, out_words * 4, VX_MEM_WRITE, &out_buffer));
  RT_CHECK(vx_buffer_address(out_buffer, &kernel_arg.out_addr));
  RT_CHECK(vx_buffer_create(device, stat_words * 8, VX_MEM_WRITE, &stat_buffer));
  RT_CHECK(vx_buffer_address(stat_buffer, &kernel_arg.stat_addr));

  std::cout << "upload source buffer" << std::endl;
  RT_CHECK(vx_enqueue_write(queue, src_buffer, 0, h_src.data(), h_src.size() * 4, 0, nullptr, nullptr));
  RT_CHECK(dxa::program_1d(device, 0, src_addr, (uint32_t)h_src.size(), sub_words, 4));

  std::cout << "load kernel module" << std::endl;
  RT_CHECK(vx_module_load_file(device, kernel_file, &module_));
  RT_CHECK(vx_module_get_kernel(module_, "main", &kernel));

  vx_launch_info_t li = {};
  li.struct_size  = sizeof(li);
  li.kernel       = kernel;
  li.args_host    = &kernel_arg;
  li.args_size    = sizeof(kernel_arg);
  li.ndim         = 2;
  li.grid_dim[0]  = 1;
  li.grid_dim[1]  = 1;
  li.block_dim[0] = warps * NT;
  li.block_dim[1] = 1;
  li.lmem_size    = lmem_size;
  li.tmem_cols    = tmem_cols;

  std::vector<uint32_t> h_out(out_words);
  std::vector<uint64_t> h_stat(stat_words);

  std::cout << "launch kernel" << std::endl;
  vx_event_h launch_ev = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch_ev));
  std::cout << "download destination buffer" << std::endl;
  vx_event_h read_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_out.data(), out_buffer, 0, out_words * 4, 1, &launch_ev, &read_ev));
  vx_event_h stat_ev = nullptr;
  RT_CHECK(vx_enqueue_read(queue, h_stat.data(), stat_buffer, 0, stat_words * 8, 1, &read_ev, &stat_ev));
  RT_CHECK(vx_event_wait_value(stat_ev, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(stat_ev);
  vx_event_release(read_ev);
  vx_event_release(launch_ev);

  std::cout << "verify result" << std::endl;
  if (mode == MODE_RACE) {
    check_race(h_out, copy_words, sub_words, last_gen);
  } else {
    check_starve(h_stat, warps - 1);
  }

  std::cout << "cleanup" << std::endl;
  cleanup();

  if (errors != 0) {
    std::cout << "Found " << std::dec << errors << " errors!" << std::endl;
    std::cout << "FAILED!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
