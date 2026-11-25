#pragma once

#include <stdint.h>

constexpr uint32_t kThreads = 8;
constexpr uint32_t kWarps = 4;
constexpr uint32_t kRowWords = 32;
constexpr uint32_t kHalfWords = kRowWords / 2;
constexpr uint32_t kRegionWords = 512;
constexpr uint32_t kTileWords = 128;
constexpr uint32_t kTmemCols = 16;
constexpr uint32_t kTmemWords = kTmemCols * kThreads;
constexpr uint32_t kDmaGens = 32;
constexpr uint32_t kWriterGens = 32;
constexpr uint32_t kSnapshots = 24;
constexpr uint32_t kSamples = 128;

constexpr uint32_t kDmaLow = 0;
constexpr uint32_t kDmaHigh = 6 * kRowWords + kHalfWords;
constexpr uint32_t kSameStore = kDmaHigh - kHalfWords;
constexpr uint32_t kOtherStore = 12 * kRowWords;
constexpr uint32_t kOtherLoad = 14 * kRowWords;
constexpr uint32_t kDmaLowProbe = kDmaLow + kTileWords - kRowWords;

constexpr uint32_t kTcuOut = 0;
constexpr uint32_t kLoadOut = kTcuOut + (kSnapshots + 2) * kTmemWords;
constexpr uint32_t kStoreOut = kLoadOut + (kSamples + 1) * 3 * kThreads;
constexpr uint32_t kFinalOut = kStoreOut + kWriterGens * 2 * 2 * kThreads;
constexpr uint32_t kOutWords = kFinalOut + kRegionWords;

// The generation and index stay in a normal float's mantissa for the TMEM reads.
constexpr uint32_t kValueBase = 0x40000000;
constexpr uint32_t kGenShift = 10;
constexpr uint32_t kWordMask = (1u << kGenShift) - 1;

constexpr uint32_t value(uint32_t generation, uint32_t word) {
  return kValueBase | (generation << kGenShift) | word;
}

struct kernel_arg_t {
  uint64_t out_addr;
};
