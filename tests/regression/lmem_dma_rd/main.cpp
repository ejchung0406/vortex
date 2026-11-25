#include <dxa.h>
#include <vortex2.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "common.h"

#define RT_CHECK(expr)                                                   \
  do {                                                                   \
    int status = (expr);                                                 \
    if (status != 0) {                                                   \
      fprintf(stderr, "%s returned %d\n", #expr, status);                 \
      exit(EXIT_FAILURE);                                                \
    }                                                                    \
  } while (false)

static uint32_t errors = 0;

static void check(bool ok, const char* phase, uint32_t sample, uint32_t word, uint32_t got) {
  if (!ok && errors++ < 12) {
    fprintf(stderr, "%s sample=%u word=%u got=%08x\n", phase, sample, word, got);
  }
}

static bool is_dma(uint32_t word) {
  return word < kTileWords || (word >= kDmaHigh && word < kDmaHigh + kTileWords);
}

static uint32_t final_generation(uint32_t word) {
  if (is_dma(word)) {
    return kDmaGens;
  }
  if (!(word & 1) && ((word >= kSameStore && word < kSameStore + kHalfWords)
                   || (word >= kOtherStore && word < kOtherStore + kHalfWords))) {
    return 2 * kWriterGens;
  }
  return 0;
}

static uint32_t check_sample(uint32_t got, uint32_t word, uint32_t old, const char* phase, uint32_t sample) {
  uint32_t generation = (got - kValueBase) >> kGenShift;
  check(got == value(generation, word) && generation >= old && generation <= final_generation(word),
        phase, sample, word, got);
  return generation;
}

static void check_tcu(const uint32_t* out) {
  std::array<uint32_t, kTmemWords> words{};
  std::array<uint32_t, kTmemWords> last{};
  std::array<bool, kRegionWords> seen{};
  bool seen_low = false, seen_high = false;
  for (uint32_t i = 0; i < kTmemWords; ++i) {
    uint32_t word = out[i] & kWordMask;
    if (word >= kRegionWords || out[i] != value(0, word) || seen[word]) {
      check(false, "initial TCU copy", 0, i, out[i]);
      return;
    }
    seen[word] = true;
    words[i] = word;
    seen_low |= word < kTileWords;
    seen_high |= word >= kDmaHigh && word < kDmaHigh + kTileWords;
  }
  check(seen_low && seen_high
        && seen[kSameStore] && seen[kOtherStore] && seen[kOtherLoad], "TCU copy coverage", 0, 0, 0);

  bool mid_dma = false;
  for (uint32_t s = 1; s <= kSnapshots + 1; ++s) {
    std::array<uint32_t, kRegionWords / kHalfWords> half_gen;
    half_gen.fill(UINT32_MAX);
    for (uint32_t i = 0; i < kTmemWords; ++i) {
      uint32_t word = words[i];
      uint32_t got = out[s * kTmemWords + i];
      uint32_t generation = check_sample(got, word, last[i], "TCU copy order", s);
      last[i] = generation;
      if (is_dma(word)) {
        uint32_t& half = half_gen[word / kHalfWords];
        check(half == UINT32_MAX || half == generation, "TCU copy of a DMA half", s, word, got);
        half = generation;
        mid_dma |= generation > 0 && generation < kDmaGens;
      }
      if (s == kSnapshots + 1) {
        check(got == value(final_generation(word), word), "final TCU copy", s, word, got);
      }
    }
  }
  check(mid_dma, "no TCU copy overlapped DMA", 0, 0, 0);
}

static void check_lsu(const uint32_t* out) {
  constexpr uint32_t probes[] = {kDmaLowProbe, kDmaHigh, kOtherLoad};
  for (uint32_t p = 0; p < 3; ++p) {
    uint32_t previous = 0;
    bool mid_dma = false;
    for (uint32_t s = 0; s <= kSamples; ++s) {
      uint32_t generation = 0;
      for (uint32_t t = 0; t < kThreads; ++t) {
        uint32_t word = probes[p] + 2 * t;
        uint32_t got = out[kLoadOut + (s * 3 + p) * kThreads + t];
        uint32_t observed = check_sample(got, word, previous, "LSU load order", s);
        if (t == 0) {
          generation = observed;
        } else {
          check(observed == generation, "LSU load of a DMA half", s, word, got);
        }
        if (s == kSamples) {
          check(got == value(final_generation(word), word), "final LSU load", s, word, got);
        }
      }
      previous = generation;
      mid_dma |= generation > 0 && generation < kDmaGens;
    }
    check(p == 2 || mid_dma, "no LSU load observed an intermediate DMA generation", 0, probes[p], 0);
  }

  for (uint32_t g = 1; g <= kWriterGens; ++g) {
    for (uint32_t p = 0; p < 2; ++p) {
      for (uint32_t t = 0; t < kThreads; ++t) {
        uint32_t word = (p == 0 ? kSameStore : kOtherStore) + 2 * t;
        uint32_t offset = kStoreOut + ((g - 1) * 4 + p * 2) * kThreads + t;
        check(out[offset] == value(2 * g - 1, word), "AMO after store", g, word, out[offset]);
        check(out[offset + kThreads] == value(2 * g, word), "load after AMO", g, word, out[offset + kThreads]);
      }
    }
  }
  for (uint32_t word = 0; word < kRegionWords; ++word) {
    uint32_t got = out[kFinalOut + word];
    check(got == value(final_generation(word), word), "final LMEM contents", 0, word, got);
  }
}

int main() {
  vx_device_h device = nullptr;
  RT_CHECK(vx_device_open(0, &device));

  uint64_t caps = 0, threads = 0, warps = 0;
  RT_CHECK(vx_device_query(device, VX_CAPS_ISA_FLAGS, &caps));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &threads));
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_WARPS, &warps));
  if (threads != kThreads || warps < kWarps || !(caps & VX_ISA_EXT_DXA) || !(caps & VX_ISA_STD_A)) {
    fprintf(stderr, "lmem_dma_rd needs eight threads, four warps, DXA and atomics\n");
    vx_device_release(device);
    return EXIT_FAILURE;
  }

  std::vector<uint32_t> src(2 * kDmaGens * kTileWords);
  for (uint32_t g = 0; g < kDmaGens; ++g) {
    for (uint32_t i = 0; i < kTileWords; ++i) {
      src[(2 * g) * kTileWords + i] = value(g + 1, kDmaLow + i);
      src[(2 * g + 1) * kTileWords + i] = value(g + 1, kDmaHigh + i);
    }
  }

  vx_queue_h queue = nullptr;
  vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &qi, &queue));
  vx_buffer_h source = nullptr, output = nullptr;
  RT_CHECK(vx_buffer_create(device, src.size() * sizeof(uint32_t), VX_MEM_READ, &source));
  RT_CHECK(vx_buffer_create(device, kOutWords * sizeof(uint32_t), VX_MEM_WRITE, &output));
  uint64_t src_addr = 0;
  kernel_arg_t args{};
  RT_CHECK(vx_buffer_address(source, &src_addr));
  RT_CHECK(vx_buffer_address(output, &args.out_addr));
  RT_CHECK(vx_enqueue_write(queue, source, 0, src.data(), src.size() * sizeof(uint32_t), 0, nullptr, nullptr));
  RT_CHECK(vortex::dxa::program_1d(device, 0, src_addr, src.size(), kTileWords, sizeof(uint32_t)));

  vx_module_h module = nullptr;
  vx_kernel_h kernel = nullptr;
  RT_CHECK(vx_module_load_file(device, "kernel.vxbin", &module));
  RT_CHECK(vx_module_get_kernel(module, "main", &kernel));
  vx_launch_info_t launch = {};
  launch.struct_size = sizeof(launch);
  launch.kernel = kernel;
  launch.args_host = &args;
  launch.args_size = sizeof(args);
  launch.ndim = 1;
  launch.grid_dim[0] = 1;
  launch.block_dim[0] = kWarps * kThreads;
  launch.lmem_size = kRegionWords * sizeof(uint32_t);
  launch.tmem_cols = 32;

  vx_event_h launched = nullptr, read = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &launch, 0, nullptr, &launched));
  std::vector<uint32_t> out(kOutWords);
  RT_CHECK(vx_enqueue_read(queue, out.data(), output, 0, out.size() * sizeof(uint32_t), 1, &launched, &read));
  RT_CHECK(vx_event_wait_value(read, 1, VX_TIMEOUT_INFINITE));

  check_tcu(out.data() + kTcuOut);
  check_lsu(out.data());
  vx_event_release(read);
  vx_event_release(launched);
  vx_kernel_release(kernel);
  vx_module_release(module);
  vx_buffer_release(output);
  vx_buffer_release(source);
  vx_queue_release(queue);
  vx_device_dump_perf(device, stdout);
  vx_device_release(device);

  puts(errors ? "FAILED!" : "PASSED!");
  return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
