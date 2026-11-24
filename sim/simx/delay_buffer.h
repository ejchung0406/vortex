// Copyright © 2019-2026
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

#include <cassert>
#include <cstdint>
#include <deque>
#include <optional>
#include "types.h"

namespace vortex {

// Elastic buffer with a fixed latency: a packet that reaches In on cycle t
// leaves through Out to arrive on cycle t + latency, one per cycle at full
// throughput. The split form retains a producer's two-entry buffer in In and
// places the link FIFO behind it. The downstream channel holds the link's
// own two-entry output.
template <typename Type>
class DelayBuffer : public SimObject<DelayBuffer<Type>> {
public:
  using Ptr = std::shared_ptr<DelayBuffer<Type>>;

  SimChannel<Type> In;
  SimChannel<Type> Out;

  DelayBuffer(const SimContext& ctx, const char* name, uint32_t latency, bool split = false)
    : SimObject<DelayBuffer<Type>>(ctx, name)
    , In(this, split ? 2 : size_for(latency))
    , Out(this)
    , latency_(latency) {
    __assert(latency >= 1, "a delay buffer needs a latency");
    if (split) {
      In.set_registered_ready(true);
      if (latency > 1) {
        fifo_.emplace(this, size_for(latency) - 2);
        fifo_->set_registered_ready(true);
      }
    }
    auto& arrival = fifo_ ? *fifo_ : In;
    arrival.tx_callback([this](const Type&, uint64_t cycle) {
      arrivals_.push_back(cycle);
    });
  }

  // A FIFO for the packets in flight at full throughput plus the one
  // arriving, then a two-entry output stage.
  static uint32_t size_for(uint32_t latency) {
    return ((latency > 1) ? (1u << log2ceil(latency)) : 0) + 2;
  }

protected:
  void on_reset() {
    arrivals_.clear();
  }

  void on_tick() {
    auto& source = fifo_ ? *fifo_ : In;
    // One cycle is spent entering the FIFO, when present, and one leaving.
    uint32_t wait = latency_ - (fifo_ ? 2 : 1);
    uint64_t now = SimPlatform::instance().cycles();
    if (!source.empty() && arrivals_.front() + wait <= now && Out.try_send(source.peek(), 1)) {
      source.pop();
      arrivals_.pop_front();
    }
    if (fifo_ && !In.empty() && fifo_->try_send(In.peek(), 1)) {
      In.pop();
    }
    // Packets still on their way in count too: their send already woke us.
    if (In.size() == 0 && source.size() == 0) {
      this->tick_sleep();
    }
  }

private:
  uint32_t latency_;
  std::optional<SimChannel<Type>> fifo_;
  std::deque<uint64_t> arrivals_;

  friend class SimObject<DelayBuffer<Type>>;
};

} // namespace vortex
