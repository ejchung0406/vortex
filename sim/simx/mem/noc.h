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
    // each input is its producer's registered buffer
    for (auto& in : In) {
      in.set_registered_ready(true);
    }
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

// The switch of an SM group (VX_noc_switch): the group's socket memory ports
// on one side, its uplink lanes to the L2 on the other.
//
// A request takes the lane its line address selects, so one address always
// keeps one path, and carries its port in the low bits of its tag; a response
// returns to the port its tag names. Each direction is a crossbar with one
// round-robin arbiter per output and a registered two-entry output buffer
// (VX_stream_xbar): an output with more than 1.5 x 8 inputs arbitrates them in
// registered slices of 8 and then the slices, a cycle more.
//
// The L2 serves a flush token when it heads the input it arrived on and holds
// every input's cacheable requests meanwhile. With one private link per port
// that orders a token behind the port's earlier requests and ahead of its
// later ones; across address-hashed lanes it does not. So each port counts
// its requests between here and the L2: a token waits until the port has none
// left in flight, and the port's later requests wait until the token has
// reached the L2.
class NocSwitch : public SimObject<NocSwitch> {
public:
  using Ptr = std::shared_ptr<NocSwitch>;

  std::vector<SimChannel<MemReq>> ReqIn;   // per port
  std::vector<SimChannel<MemRsp>> RspOut;
  std::vector<SimChannel<MemReq>> ReqOut;  // per lane
  std::vector<SimChannel<MemRsp>> RspIn;
  // per lane: the last hop into the L2 passes through here to be counted
  std::vector<SimChannel<MemReq>> ReqDeliver;

  NocSwitch(const SimContext& ctx, const char* name, uint32_t ports, uint32_t lanes, uint32_t line_bits)
    : SimObject<NocSwitch>(ctx, name)
    , ReqIn(ports, this)
    , RspOut(ports, this)
    , ReqOut(lanes, this)
    , RspIn(lanes, this)
    , ReqDeliver(lanes, this)
    , port_bits_(log2ceil(ports))
    , lane_bits_(log2ceil(lanes))
    , line_bits_(line_bits)
    , req_xbar_(this, ports, lanes)
    , rsp_xbar_(this, lanes, ports)
    , in_flight_(ports, 0)
    , token_out_(ports, false)
    , at_l2_(lanes) {
    __assert(ispow2(lanes), "the lanes of an SM group are a power of two");
    // each input is its producer's registered buffer
    for (auto& in : ReqIn) {
      in.set_registered_ready(true);
    }
    for (auto& in : RspIn) {
      in.set_registered_ready(true);
    }
    for (uint32_t l = 0; l < lanes; ++l) {
      ReqDeliver.at(l).tx_callback([this, l](const MemReq& req, uint64_t) {
        at_l2_.at(l).push_back({uint32_t(req.tag) & ((1u << port_bits_) - 1), req.flags.flush != 0});
      });
    }
  }

  // A request that takes a line back with it, which a byte gate charges:
  // a load or an atomic, not a store or a flush token.
  static bool returns_line(const MemReq& req) {
    return req.op != MemOp::ST && req.op != MemOp::FLUSH && !req.flags.flush;
  }

  // Cycles a request, and a response, take through a switch of this size.
  static uint32_t req_latency(uint32_t ports) { return 1 + Xbar<MemReq>::tree_levels(ports); }
  static uint32_t rsp_latency(uint32_t lanes) { return 1 + Xbar<MemRsp>::tree_levels(lanes); }

protected:
  void on_reset() {
    req_xbar_.reset();
    rsp_xbar_.reset();
    std::fill(in_flight_.begin(), in_flight_.end(), 0);
    std::fill(token_out_.begin(), token_out_.end(), false);
    delivered_.clear();
    // The L2 takes a request when it pops it from its input, which may hold
    // it first (a flush in progress): that pop is the delivery. Reset runs
    // after bind(), so the lane's endpoint exists by now.
    for (uint32_t l = 0; l < at_l2_.size(); ++l) {
      at_l2_[l].clear();
      SimChannelBase* endpoint = &ReqDeliver.at(l);
      while (endpoint->sink()) {
        endpoint = endpoint->sink();
      }
      endpoint->pop_callback([this, l]() {
        auto& q = at_l2_.at(l);
        __assert(!q.empty(), "the L2 took a request the lane did not carry");
        delivered_.push_back({q.front().port, SimPlatform::instance().cycles(), q.front().token});
        q.pop_front();
      });
    }
  }

  void on_tick() {
    uint64_t now = SimPlatform::instance().cycles();

    // A delivery counts from the cycle after the L2 took the request.
    while (!delivered_.empty() && delivered_.front().cycle < now) {
      uint32_t p = delivered_.front().port;
      bool token = delivered_.front().token;
      delivered_.pop_front();
      __assert(in_flight_.at(p) != 0, "a delivery the port did not send");
      __assert(!token || in_flight_.at(p) == 1, "a token entered the L2 with other requests of its port in flight");
      if (--in_flight_.at(p) == 0) {
        token_out_.at(p) = false;
      }
    }

    bool req_busy = req_xbar_.tick(ReqIn, ReqOut,
      [&](uint32_t p, const MemReq& req) {
        return !token_out_.at(p) && !(req.flags.flush && in_flight_.at(p) != 0);
      },
      [&](const MemReq& req) {
        return this->lane_of(req.addr);
      },
      [&](uint32_t p, const MemReq& req) {
        ++in_flight_.at(p);
        token_out_.at(p) = req.flags.flush;
      },
      [&](uint32_t p, const MemReq& req) {
        MemReq out(req);
        if (port_bits_ != 0) {
          __assert((uint64_t(req.tag) >> (32 - port_bits_)) == 0, "the port bits overflow the tag");
          out.tag = (req.tag << port_bits_) | p;
        }
        return out;
      });

    bool rsp_busy = rsp_xbar_.tick(RspIn, RspOut,
      [](uint32_t, const MemRsp&) {
        return true;
      },
      [&](const MemRsp& rsp) {
        return uint32_t(rsp.tag) & ((1u << port_bits_) - 1);
      },
      [](uint32_t, const MemRsp&) {},
      [&](uint32_t, const MemRsp& rsp) {
        MemRsp out(rsp);
        out.tag = rsp.tag >> port_bits_;
        return out;
      });

    if (!req_busy && !rsp_busy) {
      this->tick_sleep();
    }
  }

private:
  // XOR fold of the line address onto the lanes.
  uint32_t lane_of(uint64_t addr) const {
    uint64_t line = addr >> line_bits_;
    uint64_t h = line;
    if (lane_bits_ != 0) {
      for (uint64_t x = line >> lane_bits_; x != 0; x >>= lane_bits_) {
        h ^= x;
      }
    }
    return uint32_t(h & ((1u << lane_bits_) - 1));
  }

  // VX_stream_xbar with a registered two-entry output buffer per output.
  template <typename Pkt>
  class Xbar {
  public:
    static constexpr uint32_t kMaxFanout = 8;

    // Registered slice levels ahead of an output's join arbiter.
    static uint32_t tree_levels(uint32_t inputs) {
      return (inputs > kMaxFanout + kMaxFanout / 2) ? 1 : 0;
    }

    Xbar(SimObjectBase* owner, uint32_t inputs, uint32_t outputs)
      : inputs_(inputs)
      , outputs_(outputs)
      , slices_(tree_levels(inputs) ? (inputs + kMaxFanout - 1) / kMaxFanout : 0)
      , slice_rr_(outputs * slices_)
      , join_rr_(outputs)
      , slice_buf_(outputs * slices_, SimChannel<Pkt>(owner, 2))
      , requests_(outputs) {
      for (auto& buf : slice_buf_) {
        buf.set_registered_ready(true);
      }
      __assert(slices_ <= kMaxFanout + kMaxFanout / 2, "a crossbar output takes one slice level");
      this->reset();
    }

    void reset() {
      // a round-robin arbiter first grants its request 0
      for (uint32_t i = 0; i < slice_rr_.size(); ++i) {
        slice_rr_[i] = this->slice_size(i % slices_) - 1;
      }
      std::fill(join_rr_.begin(), join_rr_.end(), (slices_ ? slices_ : inputs_) - 1);
    }

    // One cycle. `eligible(i, pkt)` holds input i's head back, `select(pkt)`
    // names its output, `taken(i, pkt)` runs when the input's head leaves it
    // and `convert(i, pkt)` gives the packet its output form. Returns whether
    // anything is left inside or on its way in.
    template <typename Eligible, typename Select, typename Taken, typename Convert>
    bool tick(std::vector<SimChannel<Pkt>>& in, std::vector<SimChannel<Pkt>>& out,
              Eligible&& eligible, Select&& select, Taken&& taken, Convert&& convert) {
      bool busy = false;
      for (auto& r : requests_) {
        r.clear();
      }
      for (uint32_t i = 0; i < inputs_; ++i) {
        auto& ch = in.at(i);
        busy |= (ch.size() != 0);
        if (ch.empty() || !eligible(i, ch.peek())) {
          continue;
        }
        requests_.at(select(ch.peek())).push_back(i);
      }
      for (uint32_t o = 0; o < outputs_; ++o) {
        auto& reqs = requests_[o];
        if (slices_ == 0) {
          // one arbiter over the inputs
          if (reqs.empty()) {
            continue;
          }
          uint32_t g = pick(reqs, join_rr_[o], inputs_, 0);
          auto& ch = in.at(g);
          if (out.at(o).try_send(convert(g, ch.peek()), 1)) {
            taken(g, ch.peek());
            ch.pop();
            join_rr_[o] = g;
          }
          continue;
        }
        uint32_t base = o * slices_;
        scratch_.clear();
        for (uint32_t s = 0; s < slices_; ++s) {
          auto& buf = slice_buf_[base + s];
          busy |= (buf.size() != 0);
          if (!buf.empty()) {
            scratch_.push_back(s);
          }
        }
        if (!scratch_.empty()) {
          uint32_t g = pick(scratch_, join_rr_[o], slices_, 0);
          auto& buf = slice_buf_[base + g];
          if (out.at(o).try_send(buf.peek(), 1)) {
            buf.pop();
            join_rr_[o] = g;
          }
        }
        // requests arrive in input order, so each slice's are contiguous
        for (size_t k = 0; k < reqs.size();) {
          uint32_t s = reqs[k] / kMaxFanout;
          scratch_.clear();
          while (k < reqs.size() && reqs[k] / kMaxFanout == s) {
            scratch_.push_back(reqs[k++]);
          }
          uint32_t lo = s * kMaxFanout;
          uint32_t g = pick(scratch_, slice_rr_[base + s], this->slice_size(s), lo);
          auto& ch = in.at(g);
          if (slice_buf_[base + s].try_send(convert(g, ch.peek()), 1)) {
            taken(g, ch.peek());
            ch.pop();
            slice_rr_[base + s] = g - lo;
          }
        }
      }
      return busy;
    }

  private:
    uint32_t slice_size(uint32_t s) const {
      return slices_ ? std::min(kMaxFanout, inputs_ - s * kMaxFanout) : inputs_;
    }

    // Round-robin: the first of `reqs` (ascending, offset by `lo`) after `last`.
    static uint32_t pick(const std::vector<uint32_t>& reqs, uint32_t last, uint32_t size, uint32_t lo) {
      uint32_t start = (last + 1) % size;
      for (uint32_t r : reqs) {
        if (r - lo >= start) {
          return r;
        }
      }
      return reqs.front();
    }

    uint32_t inputs_;
    uint32_t outputs_;
    uint32_t slices_;
    std::vector<uint32_t> slice_rr_;
    std::vector<uint32_t> join_rr_;
    // an output's slice buffers: registered, two entries each
    std::vector<SimChannel<Pkt>> slice_buf_;
    std::vector<std::vector<uint32_t>> requests_;
    std::vector<uint32_t> scratch_;
  };

  struct Delivery {
    uint32_t port;
    uint64_t cycle;
    bool     token;
  };

  uint32_t port_bits_;
  uint32_t lane_bits_;
  uint32_t line_bits_;
  Xbar<MemReq> req_xbar_;
  Xbar<MemRsp> rsp_xbar_;
  std::vector<uint32_t> in_flight_;
  std::vector<bool>     token_out_;
  std::deque<Delivery>  delivered_;
  // per lane: the port and kind of each request waiting at the L2's input
  struct Arrival {
    uint32_t port;
    bool     token;
  };
  std::vector<std::deque<Arrival>> at_l2_;

  friend class SimObject<NocSwitch>;
};

} // namespace vortex
