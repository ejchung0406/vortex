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

#pragma once

#include <bitmanip.h>
#include <cstdint>
#include <stdexcept>

namespace vortex {

class L2SliceMap {
public:
  struct Location {
    uint32_t channel;
    uint64_t set;
    uint64_t addr;
  };

  constexpr L2SliceMap(uint32_t slices, uint32_t granule, uint32_t line_size, uint32_t sets = 1)
    : slice_mask_(slices - 1)
    , set_mask_(sets - 1)
    , slice_bits_(log2ceil(slices))
    , local_low_bits_(log2ceil(granule) + (slices > 1))
    , line_bits_(log2ceil(line_size)) {
    if (!ispow2(slices) || !ispow2(granule) || !ispow2(line_size)
     || !ispow2(sets) || granule < line_size) {
      throw std::invalid_argument("L2 slice map requires powers of two and granule >= line size");
    }
  }

  constexpr Location locate(uint64_t physical) const {
    uint64_t raw = exchange_slice_bit(physical);
    uint64_t high = raw >> (local_low_bits_ + slice_bits_);
    uint32_t raw_slice = (raw >> local_low_bits_) & slice_mask_;
    uint64_t local = (high << local_low_bits_) | (raw & low_mask());
    return {raw_slice ^ fold(high), (local >> line_bits_) & set_mask_, local};
  }

  constexpr uint64_t physical(uint32_t channel, uint64_t local) const {
    assert(channel <= slice_mask_);
    assert(local <= (UINT64_MAX >> slice_bits_));
    uint64_t high = local >> local_low_bits_;
    uint64_t raw_slice = channel ^ fold(high);
    uint64_t raw = (((high << slice_bits_) | raw_slice) << local_low_bits_) | (local & low_mask());
    return exchange_slice_bit(raw);
  }

private:
  constexpr uint64_t low_mask() const {
    return (uint64_t(1) << local_low_bits_) - 1;
  }

  // Keep the bit just above the first slice-select bit in the channel address.
  constexpr uint64_t exchange_slice_bit(uint64_t addr) const {
    if (slice_bits_ == 0)
      return addr;
    uint64_t change = (addr ^ (addr >> 1)) & (uint64_t(1) << (local_low_bits_ - 1));
    return addr ^ change ^ (change << 1);
  }

  // Only retained bits participate in the XOR so the removed bits can be recovered.
  constexpr uint32_t fold(uint64_t high) const {
    if (slice_bits_ == 0)
      return 0;
    uint32_t folded = 0;
    for (; high != 0; high >>= slice_bits_) {
      folded ^= high & slice_mask_;
    }
    return folded;
  }

  uint32_t slice_mask_;
  uint32_t set_mask_;
  uint32_t slice_bits_;
  uint32_t local_low_bits_;
  uint32_t line_bits_;
};

}
