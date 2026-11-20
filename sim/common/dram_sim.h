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

#include <cstdint>
#include <dram_timing.h>

namespace vortex {

// Device memory behind the platform's memory banks: one DRAM channel (an
// independent controller) per bank, timed by Ramulator.
class DramSim {
public:
  typedef bool (*ResponseCallback)(void *arg);

  // The JEDEC standards Ramulator models, each at its own organization and
  // timing (sw/common/dram_timing.h); the data rate picks the speed.
  using Type = dram_timing::Standard;

  struct Config {
    Type     type;
    uint32_t num_channels;  // need not be a power of two
    uint32_t block_size;    // bytes per request
    uint32_t core_clock;    // MHz
    uint32_t data_rate;     // MT/s per pin
    uint32_t latency;       // fixed read latency added in core cycles (PHY, controller, NoC)
    uint32_t queue_size;    // requests a channel holds in flight; 0 = unbounded
    uint32_t bank_groups = 4;    // per pseudo-channel, four banks each (HBM)
    uint32_t sched_window = 32;  // read, and write, bursts a channel's scheduler chooses among
    // Bytes of consecutive addresses kept on one channel. One block (or 0) is
    // the plain interleave: block i on channel i mod channels, 128 B per bank
    // visit. A larger unit is kept whole in one row of one bank, and the unit's
    // channel and bank come from its index XORed with a hash of the bits above,
    // so every (channel, bank) is used once per channels x banks units and
    // streams a fixed distance apart do not fall on the same ones.
    uint32_t interleave = 0;

    bool mixed() const { return interleave > block_size; }
    uint32_t visit_bytes() const { return this->mixed() ? interleave : 128; }

    // The channel of a byte address and the address within that channel.
    struct Location {
      uint32_t channel;
      uint64_t addr;
    };
    Location locate(uint64_t addr) const;
  };

  // The memory configured in VX_config.toml.
  static Config platform_config();

  DramSim(const Config& config);
  ~DramSim();

  void reset();

  void tick();

  // False while the channel holds queue_size requests.
  bool can_accept(uint32_t channel) const;

  // addr: byte address within the channel.
  void send_request(uint32_t channel, uint64_t addr, bool is_write, ResponseCallback response_cb, void* arg);

  // The backend: Ramulator, or with VORTEX_DRAM_MODEL=bank the bank model.
  class Impl;

private:
  Impl* impl_;
};

}
