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

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include "types.h"

namespace vortex {

// A byte-rate gate over a set of parallel streams (VX_rate_gate). The
// streams together pass at most `bytes_per_cycle` bytes of charged packets a
// cycle, each charged packet costing one line; an uncharged packet passes
// free. The credit grows by bytes_per_cycle a cycle up to one line per stream
// beyond it, so the long-run rate is exact and what an idle spell saves is at
// most one line per stream. Charged packets take turns round-robin from where
// the last cycle's grants ended. A stream passes at most one packet a cycle,
// a cycle after its grant.
template <typename Pkt>
class NocRateGate : public SimObject<NocRateGate<Pkt>> {
public:
  using Ptr = std::shared_ptr<NocRateGate<Pkt>>;
  using Charged = std::function<bool(const Pkt&)>;

  std::vector<SimChannel<Pkt>> In;
  std::vector<SimChannel<Pkt>> Out;

  NocRateGate(const SimContext& ctx, const char* name, uint32_t streams,
              uint32_t bytes_per_cycle, uint32_t line_size, Charged charged)
    : SimObject<NocRateGate<Pkt>>(ctx, name)
    , In(streams, this)
    , Out(streams, this)
    , rate_(bytes_per_cycle)
    , line_(line_size)
    , cap_(uint64_t(bytes_per_cycle) + uint64_t(streams) * line_size)
    , charged_(charged) {
    __assert(bytes_per_cycle != 0, "a rate gate needs a rate");
    __assert(ispow2(line_size), "a rate gate counts power-of-two lines");
  }

protected:
  void on_reset() {
    credit_ = cap_;
    rr_ = 0;
    last_cycle_ = SimPlatform::instance().cycles();
  }

  void on_tick() {
    uint64_t now = SimPlatform::instance().cycles();
    credit_ = std::min<uint64_t>(cap_, credit_ + (now - last_cycle_) * rate_);
    last_cycle_ = now;

    uint32_t n = In.size();
    uint64_t quota = credit_ / line_;
    uint32_t start = rr_;
    bool idle = true;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t s = (start + k) % n;
      auto& in = In.at(s);
      idle &= (in.size() == 0);
      if (in.empty()) {
        continue;
      }
      bool charged = charged_(in.peek());
      if (charged && quota == 0) {
        continue;
      }
      if (!Out.at(s).try_send(in.peek(), 1)) {
        continue;
      }
      in.pop();
      if (charged) {
        --quota;
        credit_ -= line_;
        rr_ = (s + 1) % n;
      }
    }
    // size() counts packets still on their way in: their send already woke us.
    if (idle) {
      this->tick_sleep();
    }
  }

private:
  uint32_t rate_;
  uint32_t line_;
  uint64_t cap_;
  Charged  charged_;
  uint64_t credit_ = 0;
  uint32_t rr_ = 0;
  uint64_t last_cycle_ = 0;

  friend class SimObject<NocRateGate<Pkt>>;
};

} // namespace vortex
