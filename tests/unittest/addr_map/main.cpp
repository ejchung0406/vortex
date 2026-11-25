// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <addr_map.h>
#include <cstdio>
#include <vector>

using namespace vortex;

int main() {
  constexpr uint32_t granule = 256;
  constexpr uint32_t line = 128;
  constexpr uint32_t sets = 256;
  constexpr uint32_t line_count = 1 << 20;
  constexpr L2SliceMap ref(32, granule, line, sets);
  constexpr auto loc = ref.locate(0x6a537);
  static_assert(loc.channel == 9 && loc.set == 104 && loc.addr == 0x3437);
  static_assert(ref.physical(loc.channel, loc.addr) == 0x6a537);
  static_assert(ref.locate(0).channel == ref.locate(2 * granule).channel);
  static_assert(ref.locate(granule).channel != ref.locate(0).channel);

  for (uint32_t slices : {1u, 2u, 4u, 16u, 32u, 64u}) {
    L2SliceMap map(slices, granule, line, sets);
    std::vector<uint32_t> counts(slices);
    for (uint32_t i = 0; i < line_count; ++i) {
      uint64_t base = uint64_t(i) * line;
      auto first = map.locate(base);
      ++counts.at(first.channel);
      for (uint32_t offset : {0u, 63u, 64u, line - 1}) {
        auto location = map.locate(base + offset);
        if (location.channel != first.channel || location.set != first.set
         || location.addr != first.addr + offset
         || map.physical(location.channel, location.addr) != base + offset) {
          std::fprintf(stderr, "slice map round trip failed at line %u, byte %u, %u slices\n",
                       i, offset, slices);
          return 1;
        }
      }
    }
    for (auto count : counts) {
      if (count != line_count / slices) {
        std::fprintf(stderr, "unbalanced mapping with %u slices\n", slices);
        return 1;
      }
    }

    for (uint64_t addr : {uint64_t(0xfedcba9876543210), uint64_t(UINT64_MAX)}) {
      auto location = map.locate(addr);
      if (map.physical(location.channel, location.addr) != addr) {
        std::fprintf(stderr, "high address bits lost with %u slices\n", slices);
        return 1;
      }
    }
  }
  std::puts("L2 slice map round trip and distribution passed");
}
