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
#include "types.h"

// Kernel-agnostic framework element: compiles against whichever simulation
// kernel types.h selected.
namespace vortex {

// Registered boundary stage: a one-entry-per-tick forwarder whose outgoing
// send carries an explicit >=1 cycle delay. Placed at an execution-domain
// boundary and owned by the SENDING domain, it guarantees the cross-domain
// edge is never combinational regardless of the arbiter chain behind it.
// Backpressure is credit-based: the stall decision reads only the local
// credit counter, never the downstream endpoint's occupancy; each downstream
// pop returns a credit one cycle later (a registered ready, like a real
// pipeline-boundary crossing).
template <typename Type>
class RegSlice : public SimObject<RegSlice<Type>> {
public:
  using Ptr = std::shared_ptr<RegSlice<Type>>;

  SimChannel<Type> In;
  SimChannel<Type> Out;

  RegSlice(const SimContext& ctx, const char* name, uint32_t delay)
    : SimObject<RegSlice<Type>>(ctx, name)
    , In(this)
    , Out(this)
    , delay_(delay)
    , credits_(-1) {
    __assert(delay >= 1, "boundary stage requires a registered delay");
    Out.mark_boundary();
  }

protected:
  void on_reset() {
    // Runs single-threaded (workers parked): safe point to (re)install the
    // credit-return hook on the downstream endpoint and refill the pool —
    // in-flight credit returns were dropped by the platform reset.
    this->init_credits();
  }

  void on_tick() {
    __assert(credits_ >= 0, "boundary credit underflow");
    if (!In.empty() && credits_ > 0) {
      Out.send(In.peek(), delay_);
      --credits_;
      In.pop();
    }
    // Out of credits, the stage waits on a credit return, which re-arms it.
    if (In.size() == 0 || credits_ == 0) {
      this->tick_sleep();
    }
  }

private:
  // Runs at reset, after bind(): the downstream endpoint and its capacity
  // exist by then. Credits track downstream slots exactly (send consumes,
  // pop returns after one cycle), so Out.send can never overflow the sink.
  void init_credits() {
    SimChannelBase* endpoint = &Out;
    while (endpoint->sink()) {
      endpoint = endpoint->sink();
    }
    // A full credit refill assumes no stale packets hold downstream slots.
    __assert(endpoint->size() == 0, "boundary refill with stale downstream packets");
    credits_ = static_cast<int32_t>(Out.capacity());
    endpoint->pop_callback([this]() {
      // Credit returns to the producing domain with one cycle of latency
      // (a registered ready), delivered by the producer's own thread.
      SimPlatform::instance().cross_call(this->domain(), [this]() {
        ++credits_;
        this->tick_wake();
      });
    });
  }

  uint32_t delay_;
  int32_t credits_;

  friend class SimObject<RegSlice<Type>>;
};

// A RegSlice whose input side is a combinational priority arbiter: each cycle
// it registers the first non-empty input, its index appended to the tag's low
// bits. Input 0 sees exactly a RegSlice (the same slots, the same cycle), so a
// port that gains a second, lower-priority client keeps its timing while that
// client is idle.
template <typename Type>
class RegMerge : public SimObject<RegMerge<Type>> {
public:
  using Ptr = std::shared_ptr<RegMerge<Type>>;

  std::vector<SimChannel<Type>> In;
  SimChannel<Type> Out;

  RegMerge(const SimContext& ctx, const char* name, uint32_t num_inputs, uint32_t delay)
    : SimObject<RegMerge<Type>>(ctx, name)
    , In(num_inputs, this)
    , Out(this)
    , delay_(delay)
    , sel_bits_(log2ceil(num_inputs))
    , credits_(-1) {
    __assert(delay >= 1, "boundary stage requires a registered delay");
    __assert(num_inputs >= 2, "a merge has two inputs or more");
    Out.mark_boundary();
  }

protected:
  void on_reset() {
    SimChannelBase* endpoint = &Out;
    while (endpoint->sink()) {
      endpoint = endpoint->sink();
    }
    __assert(endpoint->size() == 0, "boundary refill with stale downstream packets");
    credits_ = static_cast<int32_t>(Out.capacity());
    endpoint->pop_callback([this]() {
      SimPlatform::instance().cross_call(this->domain(), [this]() {
        ++credits_;
        this->tick_wake();
      });
    });
  }

  void on_tick() {
    __assert(credits_ >= 0, "boundary credit underflow");
    bool pending = false;
    for (uint32_t i = 0; i < In.size(); ++i) {
      if (In.at(i).empty()) {
        continue;
      }
      if (credits_ > 0) {
        Type pkt = In.at(i).peek();
        pkt.tag = (pkt.tag << sel_bits_) | i;
        Out.send(pkt, delay_);
        --credits_;
        In.at(i).pop();
      }
      break;
    }
    for (auto& in : In) {
      pending |= (in.size() != 0);
    }
    if (!pending || credits_ == 0) {
      this->tick_sleep();
    }
  }

private:
  uint32_t delay_;
  uint32_t sel_bits_;
  int32_t credits_;

  friend class SimObject<RegMerge<Type>>;
};

// The response side of a RegMerge: a RegSlice whose output is a demux by the
// tag's low bits (stripped on the way). Output 0 sees exactly a RegSlice.
template <typename Type>
class RegSplit : public SimObject<RegSplit<Type>> {
public:
  using Ptr = std::shared_ptr<RegSplit<Type>>;

  SimChannel<Type> In;
  std::vector<SimChannel<Type>> Out;

  RegSplit(const SimContext& ctx, const char* name, uint32_t num_outputs, uint32_t delay)
    : SimObject<RegSplit<Type>>(ctx, name)
    , In(this)
    , Out(num_outputs, this)
    , delay_(delay)
    , sel_bits_(log2ceil(num_outputs))
    , credits_(num_outputs, -1) {
    __assert(delay >= 1, "boundary stage requires a registered delay");
    __assert(num_outputs >= 2, "a split has two outputs or more");
    for (auto& out : Out) {
      out.mark_boundary();
    }
  }

protected:
  void on_reset() {
    for (uint32_t o = 0; o < Out.size(); ++o) {
      SimChannelBase* endpoint = &Out.at(o);
      while (endpoint->sink()) {
        endpoint = endpoint->sink();
      }
      __assert(endpoint->size() == 0, "boundary refill with stale downstream packets");
      credits_.at(o) = static_cast<int32_t>(Out.at(o).capacity());
      endpoint->pop_callback([this, o]() {
        SimPlatform::instance().cross_call(this->domain(), [this, o]() {
          ++credits_.at(o);
          this->tick_wake();
        });
      });
    }
  }

  void on_tick() {
    bool blocked = false;
    if (!In.empty()) {
      Type pkt = In.peek();
      uint32_t o = uint32_t(pkt.tag & ((uint64_t(1) << sel_bits_) - 1));
      __assert(o < Out.size(), "split: no such output");
      __assert(credits_.at(o) >= 0, "boundary credit underflow");
      if (credits_.at(o) > 0) {
        pkt.tag >>= sel_bits_;
        Out.at(o).send(pkt, delay_);
        --credits_.at(o);
        In.pop();
      } else {
        blocked = true;
      }
    }
    if (In.size() == 0 || blocked) {
      this->tick_sleep();
    }
  }

private:
  uint32_t delay_;
  uint32_t sel_bits_;
  std::vector<int32_t> credits_;

  friend class SimObject<RegSplit<Type>>;
};

} // namespace vortex
