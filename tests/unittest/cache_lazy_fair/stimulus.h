#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <queue>
#include <utility>

namespace lazy_fair {

constexpr unsigned fills = 12;
constexpr unsigned sector_bytes = 64;
constexpr unsigned marker = fills;
constexpr uint8_t stored_byte = 0xc5;

[[noreturn]] inline void fail(const char* message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(1);
}

template <class CachePorts>
int run(CachePorts& cache) {
  std::queue<std::pair<unsigned, uint8_t>> memory;
  std::array<bool, fills> requested{}, returned{};
  unsigned issued = 0, completed = 0, fills_before_marker = 0;
  bool stored = false, read_marker = false, saw_marker = false;
  bool burst = false;
  for (unsigned cycle = 0; cycle < 2000; ++cycle) {
    // A full-sector store allocates a partial line. Its later load must
    // refetch through the lazy queue without reading any memory bytes.
    if (memory.size() == fills) {
      burst = true;
    }
    if (burst && !memory.empty()) {
      auto [tag, data] = memory.front();
      if (cache.respond(tag, data)) {
        memory.pop();
      }
    }
    if (!stored) {
      stored = cache.request(true, marker, stored_byte);
    } else if (issued < fills) {
      issued += cache.request(false, issued, 0);
    } else if (!read_marker && burst) {
      read_marker = cache.request(false, marker, 0);
    }
    unsigned tag, sector;
    if (cache.memory_request(tag, sector)) {
      if (sector >= fills || requested[sector]) {
        fail("unexpected memory read");
      }
      requested[sector] = true;
      memory.emplace(tag, sector + 1);
    }
    uint8_t data;
    if (cache.core_response(tag, data)) {
      if (tag == marker) {
        if (data != stored_byte || saw_marker) {
          fail("incorrect lazy refetch response");
        }
        saw_marker = true;
        fills_before_marker = completed;
      } else {
        if (tag >= fills || returned[tag] || data != tag + 1) {
          fail("incorrect memory fill response");
        }
        returned[tag] = true;
        ++completed;
      }
    }
    cache.tick();
    if (completed == fills && saw_marker) {
      std::cout << "lazy refetch completed after " << fills_before_marker
                << " of " << fills << " memory fills\n";
      if (fills_before_marker == fills) {
        fail("lazy refetch starved until the memory fill stream drained");
      }
      return 0;
    }
  }
  fail("cache did not drain");
}

} // namespace lazy_fair
