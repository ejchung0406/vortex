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

#include "local_mem.h"
#include "mem_block_pool.h"
#include <mem.h>
#include <bitmanip.h>
#include <vector>
#include <deque>
#include <cstring>
#if VX_CFG_EXT_A_ENABLED
#include "amo_ops.h"
#include "amo_unit.h"
#endif

using namespace vortex;

// A request's timing, in cycles from the one its engine takes it in:
//   0  the engine takes the request into its input queue of two; it takes one
//      only in a cycle that starts with at most one queued
//   1  the lanes' words are compared and the request moves on to be issued
//   2  its first pass is issued to the bank registers
//   3  the banks are accessed
//   4  each served lane takes its bank's word
//   5  the response reaches its block's response register
//   6  the response register goes to the LSU block
// With several LSU blocks, the block an engine takes from is chosen the cycle
// before (see arbitrate()), and a second, read-only engine works the banks'
// second port, so two warps' reads go through each cycle. Stores and atomics
// take the first engine alone: one enters it only once the read engine has
// drained, and the read engine takes no read while a store or atomic sits in
// the first engine, while one waited at an input the cycle before, or in a
// cycle the first engine may be taking one, so every write keeps its order
// against every read.
// The row read port takes whole bank rows on the banks' second port. A request
// seen in one cycle is served the next at the earliest, and not while an
// atomic owns its bank. It shares the port with the read engine in turn: the
// read engine issues no pass in the first cycle a request is seen, nor in
// every other cycle while requests keep coming, and a row read waits a cycle
// behind a pass of the read engine. The first engine's passes that read the
// banks take the same turns, so a cycle's reads are a row read's or the two
// engines' passes', never both; its stores pass regardless. A row read also waits while a DMA write
// or a store lands in its row, and then returns the row as written. A row read
// held up by an atomic or by a write in its row stops the first engine's
// passes until it is served.
// With LMEM_WR_MERGE the lanes of a plain store that write different bytes of
// one bank word take its access together. With LMEM_WR_ROW (several LSU
// blocks) a plain store goes to its block's own store engine instead of the
// first engine: it enters only while the first and the read engine are empty,
// and those take nothing while a store is at an input or in a store engine.
// The store engines' passes share a cycle when they take different banks, in a
// rotating order of choice; a row read held up by a write stops their passes
// as it stops the first engine's.
// The model does a pass's bank accesses when it is issued. Every access keeps
// its order relative to the others, so this is the order the banks see.
class LocalMem::Impl {
protected:
  // a pass issued at cycle t has its response at the response queue's head
  // at t + 3, from where the block's response register takes it
  static constexpr uint32_t RSP_LATENCY = 3;
  // an engine's response slots, held from the pass until the block takes the
  // response from its response register
  static constexpr uint32_t RSP_QUEUE_SIZE = 8;
#if VX_CFG_EXT_A_ENABLED
  // An atomic reads its word, registers it, then writes the new value back:
  // the two cycles after its pass the banks take nothing else.
  static constexpr uint32_t AMO_STALL_CYCLES = 2;
#endif

  struct Request {
    LsuReq   req;
    uint32_t block;
    Request() : req(0), block(0) {}
  };

  struct RspEntry {
    uint64_t ready;
    uint32_t block;
    LsuRsp   rsp;
  };

  static constexpr uint32_t S0_SIZE = 2;

  struct Engine {
    std::deque<Request> s0;
    bool        s1_valid = false;
    Request     s1;
    BitVector<> pending;
    // the next pass's bank winners are known: a pass that leaves lanes
    // pending is followed by a cycle that finds the next ones
    bool        ready = false;
    std::deque<RspEntry> rsp_queue;
    // a pass was issued this cycle: its bank accesses happen in the next
    bool        passed = false;
    // and they read the banks
    bool        passed_read = false;
  };

  LocalMem* simobject_;
  Config    config_;
  RAM       ram_;
  uint32_t  addr_bits_;
  uint32_t  lg2_word_;
#if VX_CFG_EXT_A_ENABLED
  AmoUnit   amo_unit_;
#endif
  uint32_t  stall_cycles_;
  std::vector<Engine> engines_;
  // LMEM_WR_ROW: one store engine per LSU block. A plain store goes to its
  // block's engine and the two engines above keep the reads and the atomics:
  // a store enters only while both of those are empty, and they take nothing
  // while a store is at an input or in a store engine, so every store keeps
  // its order against every read. The store engines' passes share a cycle when
  // they take different banks: the one after the last cycle's first taker
  // chooses first, and each later one in turn passes if its banks are free.
  std::vector<Engine> store_engines_;
  uint32_t  s_rr_;
  bool      wr_merge_;
  bool      wr_row_;
  struct RspReg {
    bool     valid = false;
    uint32_t engine = 0;
    LsuRsp   rsp;
    RspReg() : rsp(0) {}
  };
  std::vector<RspReg> rsp_regs_; // per block
  std::vector<uint32_t> arb_last_; // per engine, its round-robin arbiter's last choice
  struct Selection {
    bool     valid = false;
    uint32_t block = 0;
    bool     read = false; // the chosen block's request is known to be a plain read
  };
  Selection a_sel_;
  Selection b_sel_;
  bool      write_waiting_r_;
  bool      dma_slot_;
  uint32_t  dma_last_grant_;
  bool      rd_slot_;
  // the read engine was held back for a row read in the last cycle
  bool      b_held_;
  // a row read an atomic or a write in its row held up, not yet served
  bool      rd_wait_;
  // rows written this cycle: by the DMA port, and by the store pass issued
  // the cycle before
  std::vector<uint64_t> wr_rows_;
  // rows of the store pass issued this cycle
  std::vector<uint64_t> store_rows_;
  mutable PerfStats perf_stats_;

  uint64_t to_local_addr(uint64_t addr) const {
    return bit_getw(addr, 0, addr_bits_ - 1);
  }

  // A bank serves one word per cycle; byte addresses within one word are the
  // same access as far as the bank is concerned.
  uint64_t bank_word(uint64_t addr) const {
    return to_local_addr(addr) >> lg2_word_;
  }

  uint32_t bank_of(uint64_t addr) const {
    return (uint32_t)(bank_word(addr) & ((1u << config_.B) - 1));
  }

  // The unit two accesses meet in: a bank row, or a block when a row is
  // narrower than the blocks the model's row accesses are made of.
  uint64_t row_of(uint64_t addr) const {
    constexpr uint64_t kUnit = (DMA_ROW_SIZE > VX_CFG_MEM_BLOCK_SIZE) ? DMA_ROW_SIZE : VX_CFG_MEM_BLOCK_SIZE;
    return to_local_addr(addr) / kUnit;
  }

  static bool is_plain_read(const LsuReq& req) {
    return !req.is_write() && !req.is_amo();
  }

  static bool is_plain_store(const LsuReq& req) {
    return req.is_write() && !req.is_amo();
  }

  static bool busy(const Engine& e) {
    return !e.s0.empty() || e.s1_valid;
  }

  static bool holds_write(const Engine& e) {
    for (auto& r : e.s0) {
      if (!is_plain_read(r.req)) {
        return true;
      }
    }
    return e.s1_valid && !is_plain_read(e.s1.req);
  }

  // The lanes one pass serves: each bank goes to its lowest pending lane, and
  // a plain read also serves every pending lane reading the same word. A
  // store or an atomic takes its bank alone, so lanes writing one word land
  // in lane order and the highest lane's bytes are the ones left. With
  // LMEM_WR_MERGE a plain-store lane also rides along when every lower
  // pending lane of its bank writes other bytes of the same word.
  BitVector<> select_pass(const LsuReq& req, const BitVector<>& pending) const {
    uint32_t num_banks = 1u << config_.B;
    std::vector<int32_t> winner(num_banks, -1);
    BitVector<> served(pending.size());
    bool join = is_plain_read(req);
    bool merge = wr_merge_ && is_plain_store(req);
    for (uint32_t i = 0; i < pending.size(); ++i) {
      if (!pending.test(i)) {
        continue;
      }
      uint32_t b = this->bank_of(req.addrs.at(i));
      if (winner.at(b) == -1) {
        winner.at(b) = i;
        served.set(i);
      } else if (join && this->bank_word(req.addrs.at(i)) == this->bank_word(req.addrs.at(winner.at(b)))) {
        served.set(i);
      } else if (merge) {
        bool rides = true;
        for (uint32_t k = 0; k < i && rides; ++k) {
          if (!pending.test(k) || this->bank_of(req.addrs.at(k)) != b) {
            continue;
          }
          rides = (this->bank_word(req.addrs.at(k)) == this->bank_word(req.addrs.at(i)))
               && (this->word_bytes(req, k) & this->word_bytes(req, i)) == 0;
        }
        if (rides) {
          served.set(i);
        }
      }
    }
    if (merge) {
      // The lanes sharing a bank's access write different bytes of one word.
      // The lanes are written one by one here, so only this check shows a
      // lane taken along to another word.
      std::vector<uint32_t> bytes(num_banks, 0);
      for (uint32_t i = 0; i < pending.size(); ++i) {
        if (!served.test(i)) {
          continue;
        }
        uint32_t b = this->bank_of(req.addrs.at(i));
        __assert(this->bank_word(req.addrs.at(i)) == this->bank_word(req.addrs.at(winner.at(b))),
                 "LocalMem: merged store lanes of different words");
        __assert((bytes.at(b) & this->word_bytes(req, i)) == 0, "LocalMem: merged store lanes enable a byte twice");
        bytes.at(b) |= this->word_bytes(req, i);
      }
    }
    return served;
  }

  // the bytes of its bank word a store lane writes
  uint32_t word_bytes(const LsuReq& req, uint32_t i) const {
    uint64_t la = to_local_addr(req.addrs.at(i));
    uint32_t shift = (uint32_t)(la & (VX_CFG_MEM_BLOCK_SIZE - 1)) & ~(config_.word_size - 1);
    return (uint32_t)((req.byteen.at(i) >> shift) & ((1u << config_.word_size) - 1));
  }

  // Do one lane's bank access and return the word the lane gets back, if any.
  std::shared_ptr<mem_block_t> access(const LsuReq& req, uint32_t i) {
    uint64_t la = to_local_addr(req.addrs.at(i));
    uint64_t line_addr = la & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
#if VX_CFG_EXT_A_ENABLED
    uint32_t hart_id = make_hart_id(req.cid, req.wid, req.tids.at(i));
    if (req.is_amo()) {
      // Shared-memory atomics read-modify-write in place and return the old
      // word; LR/SC reservations are kept per hart.
      const uint32_t bmask    = VX_CFG_MEM_BLOCK_SIZE - 1;
      const uint32_t byte_off = (uint32_t)(la & bmask);
      const uint64_t byteen   = req.byteen.at(i);
      const uint32_t wbytes   = byteen ? (uint32_t)__builtin_popcountll(byteen) : 4u;
      const uint8_t  width    = (wbytes >= 8) ? 3 : 2;
      const bool     is_lr    = (req.op == MemOp::AMO_LR);
      const bool     is_sc    = (req.op == MemOp::AMO_SC);
      const bool     sc_fail  = is_sc && !amo_unit_.check(hart_id, la);
      const bool     do_store = !is_lr && !sc_fail;

      uint8_t obuf[8] = {0};
      ram_.read(obuf, la, wbytes);
      const uint64_t old_word = amo_load_word(obuf, 0, width);
      const uint64_t rhs = req.data.at(i)
        ? amo_load_word(req.data.at(i)->data(), byte_off, width) : 0;
      auto rmw = amo_compute(req.op, width, old_word, rhs,
                             req.flags.amo_unsigned, req.amo_cmp.at(i));

      auto rsp_data = make_mem_block();
      std::memset(rsp_data->data(), 0, rsp_data->size());
      const uint64_t ret_word = is_sc ? (sc_fail ? 1ull : 0ull) : rmw.ret_word;
      amo_store_word(rsp_data->data(), byte_off, width, ret_word);

      // An SC gives up its own reservation whether it succeeds or fails; an
      // LR claims one. A committed store breaks every other hart's
      // reservation on the word, or a second hart's stale reservation would
      // survive the first hart's successful SC and one update would be lost.
      if (is_lr) {
        amo_unit_.reserve(hart_id, la);
      } else if (is_sc) {
        amo_unit_.clear(hart_id, la);
      }
      if (do_store) {
        uint8_t sbuf[8];
        amo_store_word(sbuf, 0, width, rmw.new_word);
        ram_.write(sbuf, la, wbytes);
        amo_unit_.invalidate(la, hart_id);
      }
      // an atomic responds, so it counts as a read
      ++perf_stats_.reads;
      return rsp_data;
    }
#endif
    if (req.is_write()) {
      for (uint32_t b = 0; req.data.at(i) && b < VX_CFG_MEM_BLOCK_SIZE; ++b) {
        if (req.byteen.at(i) & (1ull << b)) {
          uint8_t value = (*req.data.at(i))[b];
          ram_.write(&value, line_addr + b, 1);
        }
      }
#if VX_CFG_EXT_A_ENABLED
      // A plain store breaks a reservation on the word it writes, as an
      // atomic's commit does, or a store between a hart's load-reserved and
      // its store-conditional would go unnoticed.
      amo_unit_.invalidate(la, hart_id);
#endif
      ++perf_stats_.writes;
      return nullptr;
    }
    auto rsp_data = make_mem_block();
    ram_.read(rsp_data->data(), line_addr, VX_CFG_MEM_BLOCK_SIZE);
    ++perf_stats_.reads;
    return rsp_data;
  }

  bool dma_pending() const {
    for (auto& in : simobject_->DmaInputs) {
      if (!in.empty()) {
        return true;
      }
    }
    return false;
  }

  // Serve one DMA client's row access. Clients are served round-robin,
  // starting after the last one served.
  void dma_fire() {
    for (uint32_t i = 0; i < config_.dma_clients; ++i) {
      uint32_t c = (dma_last_grant_ + 1 + i) % config_.dma_clients;
      bool pending = false;
      bool rsp_ready = true;
      for (uint32_t p = 0; p < DMA_PORTS; ++p) {
        auto& in = simobject_->DmaInputs.at(c * DMA_PORTS + p);
        if (in.empty()) {
          continue;
        }
        pending = true;
        if (!in.peek().is_write() && simobject_->DmaOutputs.at(c * DMA_PORTS + p).full()) {
          rsp_ready = false;
        }
      }
      if (!pending) {
        continue;
      }
      if (!rsp_ready) {
        return;
      }
      for (uint32_t p = 0; p < DMA_PORTS; ++p) {
        auto& in = simobject_->DmaInputs.at(c * DMA_PORTS + p);
        if (in.empty()) {
          continue;
        }
        auto& req = in.peek();
        uint64_t line_addr = to_local_addr(req.addr) & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
        if (req.is_write()) {
          for (uint32_t b = 0; req.data && b < VX_CFG_MEM_BLOCK_SIZE; ++b) {
            if (req.byteen & (1ull << b)) {
              uint8_t value = (*req.data)[b];
              ram_.write(&value, line_addr + b, 1);
            }
          }
#if VX_CFG_EXT_A_ENABLED
          amo_unit_.invalidate(to_local_addr(req.addr), req.hart_id);
#endif
          if (config_.rd_port) {
            wr_rows_.push_back(this->row_of(req.addr));
          }
        } else {
          MemRsp rsp{req.tag, req.hart_id, req.uuid};
          auto rsp_data = make_mem_block();
          ram_.read(rsp_data->data(), line_addr, VX_CFG_MEM_BLOCK_SIZE);
          rsp.data = rsp_data;
          // The request arrived over a registered channel; the read data
          // returns the cycle after it was issued.
          simobject_->DmaOutputs.at(c * DMA_PORTS + p).send(rsp, 0);
        }
        DT(4, simobject_->name() << "-dma" << c << " req : " << req);
        in.pop();
      }
      dma_last_grant_ = c;
      return;
    }
  }

  bool rd_pending() const {
    for (auto& in : simobject_->RdInputs) {
      if (!in.empty()) {
        return true;
      }
    }
    return false;
  }

  // A write lands this cycle in a row the row read port's request reads.
  bool rd_row_written() const {
    for (auto& in : simobject_->RdInputs) {
      if (in.empty()) {
        continue;
      }
      uint64_t row = this->row_of(in.peek().addr);
      for (auto w : wr_rows_) {
        if (w == row) {
          return true;
        }
      }
    }
    return false;
  }

  // Serve the row read port's request. Returns false when its response
  // cannot be taken.
  bool rd_fire() {
    for (uint32_t p = 0; p < simobject_->RdInputs.size(); ++p) {
      if (!simobject_->RdInputs.at(p).empty() && simobject_->RdOutputs.at(p).full()) {
        return false;
      }
    }
    for (uint32_t p = 0; p < simobject_->RdInputs.size(); ++p) {
      auto& in = simobject_->RdInputs.at(p);
      if (in.empty()) {
        continue;
      }
      auto& req = in.peek();
      uint64_t line_addr = to_local_addr(req.addr) & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
      MemRsp rsp{req.tag, req.hart_id, req.uuid};
      auto rsp_data = make_mem_block();
      ram_.read(rsp_data->data(), line_addr, VX_CFG_MEM_BLOCK_SIZE);
      rsp.data = rsp_data;
      simobject_->RdOutputs.at(p).send(rsp, 0);
      DT(4, simobject_->name() << "-rd req : " << req);
      in.pop();
    }
    return true;
  }

  // Issue an engine's next pass. Returns true when it was the request's last,
  // which frees S1 for a new request in the same cycle. The response queue
  // occupancy is the one from the start of the cycle.
  bool issue_pass(Engine& e, uint32_t rsp_count) {
    auto& req = e.s1.req;
    bool has_rsp = !req.is_write() || req.is_amo();
    if (has_rsp && rsp_count >= RSP_QUEUE_SIZE) {
      return false;
    }
    auto served = this->select_pass(req, e.pending);
    e.passed = true;
    e.passed_read = has_rsp;
    LsuRsp rsp(config_.num_lanes);
    rsp.tag  = req.tag;
    rsp.cid  = req.cid;
    rsp.uuid = req.uuid;
    for (uint32_t i = 0; i < config_.num_lanes; ++i) {
      if (!served.test(i)) {
        continue;
      }
      auto data = this->access(req, i);
      if (config_.rd_port && req.is_write() && !req.is_amo()) {
        store_rows_.push_back(this->row_of(req.addrs.at(i)));
      }
      if (has_rsp) {
        rsp.mask.set(i);
        rsp.data.at(i) = data;
      }
      e.pending.reset(i);
    }
    DT(4, simobject_->name() << " pass: served=" << served << ", " << req);
    if (has_rsp) {
      e.rsp_queue.push_back({SimPlatform::instance().cycles() + RSP_LATENCY, e.s1.block, std::move(rsp)});
    }
#if VX_CFG_EXT_A_ENABLED
    if (req.is_amo()) {
      stall_cycles_ = AMO_STALL_CYCLES;
    }
#endif
    if (e.pending.none()) {
      e.s1_valid = false;
      return true;
    }
    ++perf_stats_.bank_stalls;
    e.ready = false;
    return false;
  }

  void take(Engine& e, uint32_t b) {
    auto& in = simobject_->ReqIn.at(b);
    Request r;
    r.req = in.peek();
    r.block = b;
    DT(4, simobject_->name() << "-req" << b << ": " << r.req);
    e.s0.push_back(std::move(r));
    in.pop();
  }

  // The lowest eligible block after the last one chosen, round-robin.
  int32_t pick(const std::vector<bool>& eligible, uint32_t& last) {
    for (uint32_t i = 0; i < config_.num_blocks; ++i) {
      uint32_t b = (last + 1 + i) % config_.num_blocks;
      if (eligible.at(b)) {
        last = b;
        return b;
      }
    }
    return -1;
  }

  // Each engine takes the block chosen for it the cycle before, if its
  // request is still there, then the choices for the next cycle are made from
  // the requests waiting at the start of this one. A block served this cycle
  // may present a new request of either kind next cycle, so a choice of it
  // counts as a store: the first engine takes a store only while the read
  // engine is idle, and the read engine takes nothing in that cycle. The read
  // engine takes a read only while no store or atomic waited at an input the
  // cycle before or sits in the first engine.
  void arbitrate(const std::vector<bool>& s0_ready, bool a_write, bool a_busy, bool b_busy,
                 const std::vector<bool>& s_ready, bool s_busy) {
    uint32_t n = config_.num_blocks;
    std::vector<bool> in_valid(n);
    std::vector<bool> in_read(n);
    std::vector<bool> in_store(n, false);
    std::vector<bool> fired(n, false);
    bool any_store = false;
    for (uint32_t b = 0; b < n; ++b) {
      auto& in = simobject_->ReqIn.at(b);
      in_valid.at(b) = !in.empty();
      in_read.at(b)  = in_valid.at(b) && is_plain_read(in.peek());
      if (wr_row_ && in_valid.at(b) && is_plain_store(in.peek())) {
        // the store engines' requests: the two engines below do not see them
        in_store.at(b) = true;
        in_valid.at(b) = false;
        any_store = true;
      }
    }
    bool rd_gate = !any_store && !s_busy;
    bool a_take = a_sel_.valid && s0_ready.at(0) && (a_sel_.read || !b_busy) && rd_gate;
    bool b_take = b_sel_.valid && s0_ready.at(1) && !a_write && !write_waiting_r_
               && !(a_take && !a_sel_.read) && rd_gate;
    bool b_fired = false;
    for (uint32_t b = 0; b < n; ++b) {
      if (in_store.at(b) && s_ready.at(b) && !a_busy && !b_busy) {
        this->take(store_engines_.at(b), b);
      }
    }
    if (a_take && in_valid.at(a_sel_.block)) {
      this->take(engines_.at(0), a_sel_.block);
      fired.at(a_sel_.block) = true;
    }
    if (b_take && in_valid.at(b_sel_.block)) {
      this->take(engines_.at(1), b_sel_.block);
      fired.at(b_sel_.block) = true;
      b_fired = true;
    }

    std::vector<bool> known_read(n);
    std::vector<bool> a_elig(n);
    std::vector<bool> b_elig(n);
    bool b_busy_n = b_busy || b_fired;
    bool write_waiting = false;
    for (uint32_t b = 0; b < n; ++b) {
      known_read.at(b) = in_read.at(b) && !fired.at(b);
      a_elig.at(b) = known_read.at(b) || (in_valid.at(b) && !b_busy_n);
      write_waiting = write_waiting || (in_valid.at(b) && !in_read.at(b) && !fired.at(b));
    }
    int32_t a_next = this->pick(a_elig, arb_last_.at(0));
    for (uint32_t b = 0; b < n; ++b) {
      b_elig.at(b) = known_read.at(b) && ((int32_t)b != a_next);
    }
    int32_t b_next = this->pick(b_elig, arb_last_.at(1));
    a_sel_.valid = (a_next != -1);
    a_sel_.block = (a_next != -1) ? a_next : 0;
    a_sel_.read  = (a_next != -1) && known_read.at(a_next);
    b_sel_.valid = (b_next != -1);
    b_sel_.block = (b_next != -1) ? b_next : 0;
    write_waiting_r_ = write_waiting;
  }

public:
  Impl(LocalMem* simobject, const Config& config)
    : simobject_(simobject)
    , config_(config)
    , ram_(1ull << log2ceil(config.capacity))
    , addr_bits_(log2ceil(config.capacity))
    , lg2_word_(log2ceil(config.word_size))
#if VX_CFG_EXT_A_ENABLED
    , amo_unit_(VX_CFG_AMO_RS_SIZE < 2 ? 2u : (uint32_t)VX_CFG_AMO_RS_SIZE)
#endif
    , stall_cycles_(0)
    , engines_((config.num_blocks > 1) ? 2 : 1)
    , s_rr_(0)
    , wr_merge_(VX_CFG_LMEM_WR_MERGE != 0)
    , wr_row_(VX_CFG_LMEM_WR_ROW != 0)
    , rsp_regs_(config.num_blocks)
    , arb_last_(engines_.size(), 0)
    , write_waiting_r_(false)
    , dma_slot_(false)
    , dma_last_grant_(0)
    , rd_slot_(false)
    , b_held_(false)
    , rd_wait_(false)
  {
    for (auto& e : engines_) {
      e.pending = BitVector<>(config.num_lanes);
    }
    if (wr_row_) {
      store_engines_.resize(config.num_blocks);
      for (auto& e : store_engines_) {
        e.pending = BitVector<>(config.num_lanes);
      }
    }
  }

  virtual ~Impl() {}

  void reset() {
    // perf_stats_ survives: the MPM counters run from device reset.
#if VX_CFG_EXT_A_ENABLED
    amo_unit_.reset();
#endif
    stall_cycles_ = 0;
    for (auto& e : engines_) {
      e.s0.clear();
      e.s1_valid = false;
      e.pending.reset();
      e.rsp_queue.clear();
      e.passed = false;
    }
    for (auto& e : store_engines_) {
      e.s0.clear();
      e.s1_valid = false;
      e.pending.reset();
      e.passed = false;
    }
    s_rr_ = 0;
    for (auto& r : rsp_regs_) {
      r.valid = false;
    }
    std::fill(arb_last_.begin(), arb_last_.end(), config_.num_blocks - 1);
    a_sel_ = Selection();
    b_sel_ = Selection();
    write_waiting_r_ = false;
    dma_slot_ = false;
    dma_last_grant_ = config_.dma_clients ? (config_.dma_clients - 1) : 0;
    rd_slot_ = false;
    b_held_ = false;
    rd_wait_ = false;
    wr_rows_.clear();
    store_rows_.clear();
  }

  void tick() {
    uint32_t num_engines = engines_.size();
    uint64_t now = SimPlatform::instance().cycles();

    // Responses. Each block's response register goes to the block; a free
    // one then takes the first engine's queue head ahead of the read engine's.
    // An engine's slots count the responses in its queue and in the registers.
    std::vector<uint32_t> rsp_count(num_engines);
    for (uint32_t k = 0; k < num_engines; ++k) {
      rsp_count.at(k) = engines_.at(k).rsp_queue.size();
    }
    for (auto& r : rsp_regs_) {
      rsp_count.at(r.engine) += r.valid;
    }
    for (uint32_t b = 0; b < config_.num_blocks; ++b) {
      auto& r = rsp_regs_.at(b);
      if (r.valid && simobject_->RspOut.at(b).try_send(r.rsp, 0)) {
        DT(4, simobject_->name() << "-rsp" << b << ": " << r.rsp);
        r.valid = false;
      }
    }
    for (uint32_t k = 0; k < num_engines; ++k) {
      auto& e = engines_.at(k);
      if (e.rsp_queue.empty() || e.rsp_queue.front().ready > now) {
        continue;
      }
      auto& r = rsp_regs_.at(e.rsp_queue.front().block);
      if (r.valid) {
        continue;
      }
      r.valid  = true;
      r.engine = k;
      r.rsp    = std::move(e.rsp_queue.front().rsp);
      e.rsp_queue.pop_front();
    }

    // the arbiter's view of the engines, as registered at the cycle start
    bool a_write = holds_write(engines_.at(0));
    bool a_busy  = busy(engines_.at(0));
    bool b_busy  = (num_engines > 1) && (!engines_.at(1).s0.empty() || engines_.at(1).s1_valid);
    std::vector<bool> s0_ready(num_engines);
    for (uint32_t k = 0; k < num_engines; ++k) {
      s0_ready.at(k) = (engines_.at(k).s0.size() < S0_SIZE);
    }
    uint32_t num_stores = store_engines_.size();
    bool s_busy = false;
    std::vector<bool> s_ready(config_.num_blocks, false);
    std::vector<bool> s_s1_ready(num_stores);
    std::vector<bool> s_can_pass(num_stores);
    for (uint32_t k = 0; k < num_stores; ++k) {
      auto& e = store_engines_.at(k);
      s_busy = s_busy || busy(e);
      s_ready.at(k) = (e.s0.size() < S0_SIZE);
      s_s1_ready.at(k) = !e.s1_valid;
      s_can_pass.at(k) = e.s1_valid && e.ready;
      if (e.s1_valid && !e.ready) {
        e.ready = true;
      }
    }

    // S1: a DMA access, or the engines' passes. A DMA request seen in one
    // cycle takes the banks the next; no pass is issued while one is seen.
    // An engine whose last pass left lanes pending spends this cycle finding
    // the next winners.
    std::vector<bool> s1_ready(num_engines);
    std::vector<bool> can_pass(num_engines);
    for (uint32_t k = 0; k < num_engines; ++k) {
      auto& e = engines_.at(k);
      s1_ready.at(k) = !e.s1_valid;
      can_pass.at(k) = e.s1_valid && e.ready;
      if (e.s1_valid && !e.ready) {
        e.ready = true;
      }
    }
    // The row read port and the read engine take the second port in turn.
    bool rd_valid = config_.rd_port && this->rd_pending();
    bool b_hold = rd_valid && !b_held_;
    b_held_ = b_hold;
    bool banks_read = config_.rd_port && (engines_.at(1).passed
                   || (engines_.at(0).passed && engines_.at(0).passed_read));
    auto& a_req = engines_.at(0).s1.req;
    bool a_reads = !a_req.is_write() || a_req.is_amo();
    wr_rows_.swap(store_rows_);
    store_rows_.clear();
    for (auto& e : engines_) {
      e.passed = false;
    }
    for (auto& e : store_engines_) {
      e.passed = false;
    }
    bool a_hold = rd_wait_;
    if (stall_cycles_ != 0) {
      --stall_cycles_;
      dma_slot_ = false;
      rd_slot_ = false;
      rd_wait_ = rd_valid;
    } else {
      bool dma_valid = this->dma_pending();
      if (dma_slot_) {
        this->dma_fire();
      }
      dma_slot_ = dma_valid;
      bool row_written = rd_valid && this->rd_row_written();
      bool rd_fired = rd_slot_ && rd_valid && !banks_read && !row_written && this->rd_fire();
      rd_wait_ = rd_valid && !rd_fired && (rd_wait_ || row_written);
      rd_slot_ = rd_valid;
      if (!dma_valid) {
        for (uint32_t k = 0; k < num_engines; ++k) {
          if (can_pass.at(k) && !(k == 0 && (a_hold || (b_hold && a_reads))) && !(k == 1 && b_hold)) {
            s1_ready.at(k) = this->issue_pass(engines_.at(k), rsp_count.at(k));
          }
        }
        if (!a_hold) {
          uint32_t taken = 0;
          bool first = true;
          uint32_t rr = s_rr_;
          for (uint32_t p = 0; p < num_stores; ++p) {
            uint32_t k = (rr + p) % num_stores;
            if (!s_can_pass.at(k)) {
              continue;
            }
            auto& e = store_engines_.at(k);
            uint32_t banks = 0;
            for (uint32_t i = 0; i < config_.num_lanes; ++i) {
              if (e.pending.test(i)) {
                banks |= 1u << this->bank_of(e.s1.req.addrs.at(i));
              }
            }
            if (banks & taken) {
              continue;
            }
            taken |= banks;
            if (first) {
              s_rr_ = (k + 1) % num_stores;
              first = false;
            }
            s_s1_ready.at(k) = this->issue_pass(e, 0);
          }
        }
      }
    }
    for (uint32_t k = 0; k < num_stores; ++k) {
      auto& e = store_engines_.at(k);
      if (s_s1_ready.at(k) && !e.s0.empty()) {
        e.s1 = std::move(e.s0.front());
        e.s0.pop_front();
        e.s1_valid = true;
        e.pending = e.s1.req.mask;
        e.ready = true;
      }
    }

    // S0 -> S1, with the single-pass check
    for (uint32_t k = 0; k < num_engines; ++k) {
      auto& e = engines_.at(k);
      if (s1_ready.at(k) && !e.s0.empty()) {
        e.s1 = std::move(e.s0.front());
        e.s0.pop_front();
        e.s1_valid = true;
        e.pending = e.s1.req.mask;
        e.ready = true;
      }
    }

    if (num_engines == 1) {
      // one block: its request goes straight to the engine
      auto& in = simobject_->ReqIn.at(0);
      if (s0_ready.at(0) && !in.empty()) {
        this->take(engines_.at(0), 0);
      }
    } else {
      this->arbitrate(s0_ready, a_write, a_busy, b_busy, s_ready, s_busy);
    }

    bool idle = (stall_cycles_ == 0) && !dma_slot_ && !a_sel_.valid && !b_sel_.valid
             && !rd_slot_ && !b_held_ && !rd_wait_ && store_rows_.empty();
    for (auto& e : engines_) {
      idle = idle && e.s0.empty() && !e.s1_valid && e.rsp_queue.empty();
    }
    for (auto& e : store_engines_) {
      idle = idle && e.s0.empty() && !e.s1_valid;
    }
    for (auto& r : rsp_regs_) {
      idle = idle && !r.valid;
    }
    // A packet reserved toward an input this cycle is not delivered yet, so
    // empty() does not see it; size() counts it. Sleeping past one would leave
    // it unserved until some later packet happened to wake the unit.
    for (auto& in : simobject_->DmaInputs) {
      idle = idle && (in.size() == 0);
    }
    for (auto& in : simobject_->RdInputs) {
      idle = idle && (in.size() == 0);
    }
    for (auto& in : simobject_->ReqIn) {
      idle = idle && (in.size() == 0);
    }
    if (idle) {
      simobject_->tick_sleep();
    }
  }

  const PerfStats& perf_stats() const {
    return perf_stats_;
  }

  bool holds_store() const {
    for (auto& in : simobject_->ReqIn) {
      if (!in.empty() && !is_plain_read(in.peek())) {
        return true;
      }
    }
    for (auto& e : store_engines_) {
      if (busy(e)) {
        return true;
      }
    }
    return holds_write(engines_.at(0));
  }
};

///////////////////////////////////////////////////////////////////////////////

LocalMem::LocalMem(const SimContext& ctx, const char* name, const Config& config)
  : SimObject<LocalMem>(ctx, name)
  , ReqIn(config.num_blocks, this)
  , RspOut(config.num_blocks, this)
  , DmaInputs(config.dma_clients * DMA_PORTS, this)
  , DmaOutputs(config.dma_clients * DMA_PORTS, this)
  , RdInputs(config.rd_port ? DMA_PORTS : 0, this)
  , RdOutputs(config.rd_port ? DMA_PORTS : 0, this)
  , impl_(new Impl(this, config))
{}

LocalMem::~LocalMem() {
  delete impl_;
}

void LocalMem::on_reset() {
  impl_->reset();
}

void LocalMem::on_tick() {
  impl_->tick();
}

bool LocalMem::holds_store() const {
  return impl_->holds_store();
}

const LocalMem::PerfStats& LocalMem::perf_stats() const {
  return impl_->perf_stats();
}
