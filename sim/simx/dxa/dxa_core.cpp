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

#include "dxa_core.h"
#include "dxa_meta.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <vector>
#include "core.h"
#include "socket.h"
#include "socket.h"
#include "mem_block_pool.h"
#include "debug.h"
#include "constants.h"

using namespace vortex;

// response tags index the in-flight slots by masking
static_assert(VX_CFG_DXA_MAX_INFLIGHT >= 2 && (VX_CFG_DXA_MAX_INFLIGHT & (VX_CFG_DXA_MAX_INFLIGHT - 1)) == 0,
              "DXA_MAX_INFLIGHT must be a power of two of at least 2");

namespace {

// GMEM ports: port i joins the arb of the socket's L1 memory port i as a peer,
// and serves workers i, i + kDxaMemPorts, ... with an arbiter of its own.
constexpr uint32_t kDxaMemPorts = VX_CFG_DXA_GMEM_PORTS;
static_assert(kDxaMemPorts >= 1 && kDxaMemPorts <= VX_CFG_NUM_DXA_CORES
           && kDxaMemPorts <= VX_CFG_L1_MEM_PORTS,
              "DXA_GMEM_PORTS: at most one per worker and per socket memory port");

constexpr uint32_t gmem_workers(uint32_t port) {
  return (VX_CFG_NUM_DXA_CORES - port + kDxaMemPorts - 1) / kDxaMemPorts;
}

// LMEM "word" granularity for splitting DXA writes. The LocalMem bank
// model applies byteen relative to a VX_CFG_MEM_BLOCK_SIZE-aligned address,
// so each LineWork's destination must lie within one VX_CFG_MEM_BLOCK_SIZE-aligned
// region. mem_block granularity is the byteen-mask scope.
constexpr uint32_t kLmemWordSize = VX_CFG_MEM_BLOCK_SIZE;

// GMEM line size + mask.
constexpr uint32_t kGmemLineSize = VX_CFG_L1_LINE_SIZE;
constexpr uint64_t kGmemLineMask = ~uint64_t(VX_CFG_L1_LINE_SIZE - 1);

// Cores served by one (socket-resident) DXA engine.
constexpr uint32_t kCoresPerSocket = VX_CFG_SOCKET_SIZE;
// A delivered line reaches its SMEM write through two pipeline registers.
constexpr uint32_t kSmemWrLatency = 2;
// DXA_LMEM_ROW: up to one worker's beat per row part in a cycle.
constexpr bool kLmemRow = (VX_CFG_DXA_LMEM_ROW != 0) && (DxaCore::LMEM_PORTS_PER_CORE > 1);

} // namespace

// ════════════════════════════════════════════════════════════════════
// DxaCore::Impl
// ════════════════════════════════════════════════════════════════════

class DxaCore::Impl {
public:
  // ── Descriptor ──────────────────────────────────────────────────────
  struct Descriptor {
    uint64_t base_addr = 0;
    std::array<uint32_t, 5> sizes = {};
    std::array<uint32_t, 4> strides = {};
    uint32_t meta = 0;
    std::array<uint32_t, 5> element_strides = {};
    std::array<uint16_t, 5> tile_sizes = {};
    uint32_t cfill = 0;
    uint32_t smem_stride = 0;
  };

  // ── Per-line work item produced by addr_gen, consumed by gmem_req ───
  // Each entry = ONE GMEM cache-line read. Row-major writes contribute it to a
  // single contiguous LMEM word write; a K-major or tiled load fans it out to
  // `km_num_elems` per-element destinations, which smem_wr writes one element
  // per beat (one read per cache line, never re-read per element).
  struct LineWork {
    uint64_t gmem_cl_addr;     // CL-aligned global address
    uint64_t smem_word_addr;   // word-aligned SMEM byte address (element 0)
    uint32_t cl_byte_offset;   // start of valid bytes within the CL (element 0)
    uint32_t smem_byte_offset; // destination offset within the SMEM word (element 0)
    uint32_t valid_length;     // valid bytes per write
    uint32_t cfill;            // OOB fill value (lane-replicated)
    bool     oob;              // skip GMEM read; use cfill
    bool     last;             // last work item of the transfer
    bool     scatter;          // K-major/tiled: one element written per beat
    uint32_t km_num_elems;     // K-major scatter fan-out (1 = contiguous write)
    uint32_t km_lane_stride;   // SMEM byte stride between scattered elements
    uint8_t  dest_layout;      // DestLayout (Flat/BlockMajor use tiled_dest_elem)
    uint32_t tile_k_row;       // tiled scatter: K-row (dim1 index) for this span
    uint32_t tile_n_base;      // tiled scatter: N (dim0) index where the span starts
  };

  // ── Inflight slot for a GMEM read ──────────────────────────────────
  // Its state flags live in the worker's SlotSets (allocated, rsp_arrived,
  // oob_pending, deferred), so the per-cycle scans read a few words.
  struct InflightSlot {
    uint64_t  ready_cycle = 0;  // first cycle the line can be written to SMEM
    uint64_t  free_cycle = 0;   // first cycle the slot can carry a new read
    LineWork  work;            // captured at allocation
    std::shared_ptr<mem_block_t> rsp_data;
  };

  // ── A set of in-flight slots, walked in slot order ──────────────────
  struct SlotSet {
    static constexpr uint32_t kSlots = VX_CFG_DXA_MAX_INFLIGHT;
    static constexpr uint32_t kWords = (kSlots + 63) / 64;
    // the bits of word i that name slots
    static constexpr uint64_t valid(uint32_t i) {
      return (kSlots - 64 * i >= 64) ? ~uint64_t(0) : ((uint64_t(1) << (kSlots - 64 * i)) - 1);
    }
    std::array<uint64_t, kWords> bits = {};

    bool test(uint32_t i) const { return (bits[i / 64] >> (i % 64)) & 1; }
    void set(uint32_t i)        { bits[i / 64] |= uint64_t(1) << (i % 64); }
    void clear(uint32_t i)      { bits[i / 64] &= ~(uint64_t(1) << (i % 64)); }
    void reset()                { bits.fill(0); }
    bool any() const {
      for (auto b : bits) { if (b) return true; }
      return false;
    }
    // lowest slot in the set, or UINT32_MAX
    uint32_t first() const {
      for (uint32_t i = 0; i < kWords; ++i) {
        if (bits[i]) return 64 * i + __builtin_ctzll(bits[i]);
      }
      return UINT32_MAX;
    }
  };

  // ── Worker (one per slice) ───────────────────────────────────────────
  enum class WState { IDLE, RUNNING };

  struct Worker {
    WState                  state = WState::IDLE;
    uint32_t                worker_id = 0;
    DxaReq                  req;
    Descriptor              desc;
    bool                    is_multicast = false;
    std::vector<uint32_t>   cta_indices;
    uint32_t                smem_stride = 0;
    uint64_t                issue_cycle = 0;

    // setup engine: the next transfer is set up while this one drains
    bool                    staged = false;
    DxaReq                  staged_req;
    uint64_t                staged_ready = 0;
    uint64_t                started_cycle = 0;

    // addr_gen: pre-enumerated work list (one entry per GMEM CL +
    // multicast replays handled at smem_wr stage).
    std::vector<LineWork>   work_list;
    uint32_t                ag_idx = 0;          // next entry to issue (gmem_req)

    // gmem_req → smem_wr inflight bookkeeping.
    std::array<InflightSlot, VX_CFG_DXA_MAX_INFLIGHT> inflight;
    SlotSet                 allocated;           // outstanding slots
    SlotSet                 rsp_arrived;         // line captured into smem_wr
    SlotSet                 oob_pending;         // OOB line waiting for its turn into smem_wr
    SlotSet                 deferred;            // last line parked until the others drain
    uint32_t                num_issued = 0;      // outstanding slots (allocated's size)
    uint32_t                drain_slot = UINT32_MAX; // slot currently draining

    void clear_slots() {
      allocated.reset();
      rsp_arrived.reset();
      oob_pending.reset();
      deferred.reset();
      num_issued = 0;
    }

    // smem_wr multicast replay state.
    uint32_t                mc_cta_idx = 0;      // 0..cta_indices.size()
    uint32_t                km_elem_idx = 0;     // 0..km_num_elems (K-major scatter)
    uint32_t                writes_emitted = 0;  // count for perf
  };

  // ── Constructor ──────────────────────────────────────────────────────
  explicit Impl(DxaCore* simobject)
    : simobject_(simobject)
    , workers_(VX_CFG_NUM_DXA_CORES)
    , offers_(VX_CFG_NUM_DXA_CORES)
    , cycle_(0)
  {
    for (uint32_t i = 0; i < VX_CFG_NUM_DXA_CORES; ++i)
      workers_[i].worker_id = i;
  }

  void reset() {
    cycle_ = 0;
    queue_.clear();
    lmem_rr_ = VX_CFG_NUM_DXA_CORES - 1;
    // perf_stats_ survives: the MPM counters run from device reset.
    for (auto& w : workers_) {
      w.state = WState::IDLE;
      w.work_list.clear();
      w.cta_indices.clear();
      w.clear_slots();
      w.drain_slot = UINT32_MAX;
      for (auto& s : w.inflight) { s = InflightSlot(); }
      w.staged = false;
      w.ag_idx = 0;
      w.mc_cta_idx = 0;
      w.km_elem_idx = 0;
      w.writes_emitted = 0;
    }
  }

  int dcr_write(uint32_t addr, uint32_t value) {
    uint32_t slot = VX_DCR_DXA_DESC_SLOT(addr);
    uint32_t word = VX_DCR_DXA_DESC_WORD(addr);
    auto& d = descriptors_.at(slot);
    switch (word) {
    case VX_DCR_DXA_DESC_BASE_LO_OFF:   d.base_addr = (d.base_addr & 0xffffffff00000000ull) | value; break;
    case VX_DCR_DXA_DESC_BASE_HI_OFF:   d.base_addr = (d.base_addr & 0x00000000ffffffffull) | (uint64_t(value) << 32); break;
    case VX_DCR_DXA_DESC_SIZE0_OFF:     d.sizes[0] = value; break;
    case VX_DCR_DXA_DESC_SIZE1_OFF:     d.sizes[1] = value; break;
    case VX_DCR_DXA_DESC_SIZE2_OFF:     d.sizes[2] = value; break;
    case VX_DCR_DXA_DESC_SIZE3_OFF:     d.sizes[3] = value; break;
    case VX_DCR_DXA_DESC_SIZE4_OFF:     d.sizes[4] = value; break;
    case VX_DCR_DXA_DESC_STRIDE0_OFF:   d.strides[0] = value; break;
    case VX_DCR_DXA_DESC_STRIDE1_OFF:   d.strides[1] = value; break;
    case VX_DCR_DXA_DESC_STRIDE2_OFF:   d.strides[2] = value; break;
    case VX_DCR_DXA_DESC_STRIDE3_OFF:   d.strides[3] = value; break;
    case VX_DCR_DXA_DESC_META_OFF:      d.meta = value; break;
    case VX_DCR_DXA_DESC_ESTRIDE0_OFF:  d.element_strides[0] = value; break;
    case VX_DCR_DXA_DESC_ESTRIDE1_OFF:  d.element_strides[1] = value; break;
    case VX_DCR_DXA_DESC_ESTRIDE2_OFF:  d.element_strides[2] = value; break;
    case VX_DCR_DXA_DESC_ESTRIDE3_OFF:  d.element_strides[3] = value; break;
    case VX_DCR_DXA_DESC_ESTRIDE4_OFF:  d.element_strides[4] = value; break;
    case VX_DCR_DXA_DESC_TILESIZE01_OFF:
      d.tile_sizes[0] = uint16_t(value & 0xffff);
      d.tile_sizes[1] = uint16_t(value >> 16);
      break;
    case VX_DCR_DXA_DESC_TILESIZE23_OFF:
      d.tile_sizes[2] = uint16_t(value & 0xffff);
      d.tile_sizes[3] = uint16_t(value >> 16);
      break;
    case VX_DCR_DXA_DESC_TILESIZE4_OFF: d.tile_sizes[4] = uint16_t(value & 0xffff); break;
    case VX_DCR_DXA_DESC_CFILL_OFF:     d.cfill = value; break;
    case VX_DCR_DXA_DESC_SMEM_STRIDE_OFF:
      // Round up to the LMEM word size so multicast destination addresses
      // `issuer_addr + r * smem_stride` stay block-aligned. The LMEM model
      // treats requests as block-aligned with a byteen mask; a non-aligned
      // stride would truncate the address and land writes in the wrong
      // block. The dispatcher applies the same rounding to per-CTA LMEM
      // allocations so the host-visible "same offset in receiver LMEM"
      // contract holds end-to-end.
      d.smem_stride = (value + kLmemWordSize - 1u) & ~uint32_t(kLmemWordSize - 1u);
      break;
    default: break;
    }
    return 0;
  }

  void tick() {
    ++cycle_;

    // Drain in reverse-pipeline order (smem_wr → gmem_req → addr_gen →
    // setup → req-arb) so each stage's tick reads what its upstream produced
    // earlier this cycle, mimicking forward stage flow.

    // A line leaving pend for the fill buffer frees pend for a capture in
    // the same cycle. Each GMEM port presents a single head to its worker.
    if (kLmemRow) {
      tick_lmem_row();
    }
    std::array<GmemResponse, VX_CFG_NUM_DXA_CORES> responses{};
    for (uint32_t port = 0; port < kDxaMemPorts; ++port) {
      auto& in = simobject_->gmem_rsp_in.at(port);
      if (in.empty()) {
        continue;
      }
      uint32_t route_bits = log2ceil(gmem_workers(port));
      auto tag = in.peek().tag;
      uint32_t worker = port + kDxaMemPorts * (tag & ((1u << route_bits) - 1));
      responses.at(worker) = {&in, uint32_t(tag >> route_bits)};
    }
    for (auto& w : workers_) {
      if (!kLmemRow && w.state == WState::RUNNING) {
        tick_worker_smem_wr(w);
      }
      capture_line(w, responses.at(w.worker_id));
      if (w.state == WState::RUNNING) {
        tick_worker_gmem_req(w);
      }
    }

    // 3) Start a set-up transfer on an idle worker; otherwise let the
    //    worker's setup engine take the next request.
    //    A request goes to an idle worker before it waits behind a running one.
    for (uint32_t pass = 0; pass < 2; ++pass) {
      for (auto& w : workers_) {
        if (pass == 0 && w.staged && w.state == WState::IDLE && cycle_ >= w.staged_ready) {
          w.staged = false;
          w.started_cycle = cycle_;
          start_worker(w, w.staged_req);
        } else if (!w.staged && !queue_.empty() && w.started_cycle != cycle_
                && (pass == 1 || w.state == WState::IDLE)) {
          w.staged_req   = queue_.front();
          w.staged_ready = cycle_ + this->setup_latency(w.staged_req);
          w.staged       = true;
          queue_.pop_front();
        }
      }
    }

    // 4) Drain DxaUnit channels → req queue (round-robin).
    drain_req_in();
  }

  const DxaCore::PerfStats& perf_stats() const { return perf_stats_; }

private:
  // ── Descriptor helpers ───────────────────────────────────────────────
  static uint32_t desc_rank(uint32_t meta) {
    uint32_t r = (meta >> DXA_DESC_META_DIM_LSB)
                  & ((1u << DXA_DESC_META_DIM_BITS) - 1u);
    return (r == 0) ? 1u : std::min(r, 5u);
  }

  static uint32_t desc_elem_bytes(uint32_t meta) {
    uint32_t enc = (meta >> DXA_DESC_META_ELEMSZ_LSB)
                    & ((1u << DXA_DESC_META_ELEMSZ_BITS) - 1u);
    return 1u << enc;
  }

  // Destination SMEM layout (2-bit LAYOUT meta field). Mirrors dxa.h::Layout.
  enum class DestLayout : uint32_t { RowMajor = 0, KMajor = 1, Flat = 2, BlockMajor = 3 };

  static DestLayout desc_layout(uint32_t meta) {
    uint32_t v = (meta >> DXA_DESC_META_LAYOUT_LSB)
                 & ((1u << DXA_DESC_META_LAYOUT_BITS) - 1u);
    return DestLayout(v);
  }

  // K-major destination layout (TMA style): SMEM addr per element is
  // base + i1 * elem_bytes + e0 * tile1 * elem_bytes (instead of the default
  // row-major base + i1 * tile0 * elem_bytes + e0 * elem_bytes).
  static bool desc_dest_kmajor(uint32_t meta) { return desc_layout(meta) == DestLayout::KMajor; }

  // Flat (sparse) / BlockMajor (dense) reuse the K-major SCATTER datapath
  // (contiguous GMEM read → permuted SMEM write) with a richer per-element
  // destination index. The tile geometry rides ESTRIDE2 (set_tile_geometry).
  static bool desc_dest_tiled(uint32_t meta) {
    DestLayout l = desc_layout(meta);
    return l == DestLayout::Flat || l == DestLayout::BlockMajor;
  }

  // WGMMA tile geometry conveyed by set_tile_geometry() via ESTRIDE2:
  // tcN in [15:0], the dense B block K extent (elements) in [31:16].
  static uint32_t desc_geo_tcn(const Descriptor& d) {
    return std::max<uint32_t>(1u, d.element_strides[2] & 0xffffu);
  }
  static uint32_t desc_geo_bkk(const Descriptor& d) {
    return std::max<uint32_t>(1u, d.element_strides[2] >> 16);
  }

  // SMEM element-index for B element (k = K-row, n = N-col) under the bbuf's
  // native layouts. Mirrors vx_tensor.h::b_sp_flat_idx / b_blockmajor_idx
  // (tcK == tcN for canonical WGMMA configs; ratio = 32-bit-word / elem).
  static uint32_t tiled_dest_elem(DestLayout lay, uint32_t k, uint32_t n,
                                  uint32_t ratio, uint32_t tcN, uint32_t n_steps,
                                  uint32_t bkK) {
    if (lay == DestLayout::Flat) {
      uint32_t b_tcK_words = tcN * 2;
      uint32_t blk_words   = tcN * b_tcK_words;
      uint32_t k_word = k / ratio;
      uint32_t elem   = k % ratio;
      uint32_t k_blk  = k_word / b_tcK_words;
      uint32_t kw_in  = k_word % b_tcK_words;
      uint32_t n_blk  = n / tcN;
      uint32_t n_in   = n % tcN;
      uint32_t word_off = (k_blk * n_steps + n_blk) * blk_words + (kw_in * tcN + n_in);
      return word_off * ratio + elem;
    }
    // BlockMajor (dense): within-block N-outer, K-inner; the block K extent
    // follows the consumer's FEDP depth and is conveyed by the descriptor.
    uint32_t kw          = bkK;
    uint32_t b_blk_elems = kw * tcN;
    uint32_t k_blk = k / kw;
    uint32_t r_in  = k % kw;
    uint32_t n_blk = n / tcN;
    uint32_t n_in  = n % tcN;
    return (k_blk * n_steps + n_blk) * b_blk_elems + n_in * kw + r_in;
  }

  // Parameter setup takes longer for higher-rank descriptors: each extra
  // dimension adds a multiply phase.
  uint32_t setup_latency(const DxaReq& req) const {
    uint32_t rank = (req.desc_slot < VX_DCR_DXA_DESC_COUNT)
                  ? desc_rank(descriptors_.at(req.desc_slot).meta) : 1;
    return 2 * std::max<uint32_t>(rank, 2) + 2;
  }

  // ── Pull from per-core dxa_req_in[] into queue_ (round-robin) ────────
  void drain_req_in() {
    auto& chs = simobject_->dxa_req_in;
    for (uint32_t i = 0; i < chs.size(); ++i) {
      uint32_t cid = (rr_req_ + i) % chs.size();
      auto& ch = chs.at(cid);
      if (ch.empty()) continue;
      if (queue_.size() >= VX_CFG_DXA_QUEUE_SIZE) break;
      queue_.push_back(ch.peek());
      ch.pop();
      rr_req_ = (cid + 1) % chs.size();
      break;
    }
  }

  // ── Setup: pre-enumerate the work list for this transfer ─────────────
  void start_worker(Worker& w, const DxaReq& req) {
    w.req = req;
    w.issue_cycle = cycle_;
    w.work_list.clear();
    w.ag_idx = 0;
    w.clear_slots();
    w.drain_slot = UINT32_MAX;
    for (auto& s : w.inflight) { s.rsp_data.reset(); }
    w.mc_cta_idx = 0;
    w.km_elem_idx = 0;
    w.writes_emitted = 0;

    // Multicast setup.
    w.is_multicast = (__builtin_popcount(req.cta_mask) > 1);
    w.cta_indices.clear();
    if (w.is_multicast) {
      for (uint32_t i = 0; i < 32; ++i) {
        if (req.cta_mask & (1u << i)) w.cta_indices.push_back(i);
      }
    }

    if (req.desc_slot >= VX_DCR_DXA_DESC_COUNT) {
      // Invalid descriptor — release barrier(s) directly without any LMEM write.
      release_all_barriers(w);
      finish_worker(w);
      return;
    }

    w.desc = descriptors_.at(req.desc_slot);
    w.smem_stride = w.desc.smem_stride;

    enumerate_work_list(w);
    if (w.work_list.empty()) {
      // No work — same edge case as above.
      release_all_barriers(w);
      finish_worker(w);
      return;
    }

    // Mark the *last* work item — its multicast replay's last write carries
    // notify_done.
    w.work_list.back().last = true;

    w.state = WState::RUNNING;
    perf_stats_.gmem_reads += w.work_list.size();

    DT(3, simobject_->name() << "[" << w.worker_id << "] start: core="
       << req.core->id() << ", wid=" << req.wid
       << ", slot=" << req.desc_slot
       << ", lines=" << w.work_list.size()
       << ", multicast=" << w.is_multicast
       << ", num_ctas=" << w.cta_indices.size());
  }

  // Enumerate (CL, smem-word, byte-offset, length, oob) tuples — one per
  // GMEM CL the transfer needs to read. Row-major iteration over outer
  // dims, contiguous span over dim-0, dedup of consecutive same-CL.
  void enumerate_work_list(Worker& w) {
    const auto& desc = w.desc;
    uint32_t rank = desc_rank(desc.meta);
    if (rank < 1 || rank > 5) return;

    uint32_t   elem_bytes  = desc_elem_bytes(desc.meta);
    DestLayout layout      = desc_layout(desc.meta);
    bool       dest_kmajor = (layout == DestLayout::KMajor);
    bool       dest_tiled  = desc_dest_tiled(desc.meta);
    std::array<uint32_t, 5> tiles = {};
    for (uint32_t d = 0; d < 5; ++d)
      tiles[d] = (d < rank) ? std::max<uint32_t>(1u, desc.tile_sizes[d]) : 1u;

    // K-major / tiled (Flat, BlockMajor) scatter require rank ≤ 2.
    if ((dest_kmajor || dest_tiled) && rank > 2) {
      dest_kmajor = false;
      dest_tiled  = false;
      layout      = DestLayout::RowMajor;
    }
    // Both share the single-element scatter datapath (one read → per-element
    // strided/permuted writes); row-major writes the span contiguously.
    bool scatter = dest_kmajor || dest_tiled;

    uint32_t total_rows = 1;
    for (uint32_t d = 1; d < rank; ++d) total_rows *= tiles[d];
    uint32_t row_elems = tiles[0];

    // K-major SMEM per-lane stride = tile1 * elem_bytes.
    uint32_t per_lane_stride_bytes = tiles[1] * elem_bytes;

    // Tiled (Flat/BlockMajor) geometry: ratio = 32-bit-word / elem, tcN from
    // ESTRIDE2, n_steps = row_elems(=xtileN) / tcN.
    uint32_t geo_ratio  = dest_tiled ? std::max<uint32_t>(1u, 4u / elem_bytes) : 1u;
    uint32_t geo_tcN    = dest_tiled ? desc_geo_tcn(desc) : 1u;
    uint32_t geo_bkK    = dest_tiled ? desc_geo_bkk(desc) : 1u;
    uint32_t geo_nsteps = dest_tiled ? std::max<uint32_t>(1u, row_elems / geo_tcN) : 1u;

    uint32_t cfill = desc.cfill;
    uint64_t global_prev_cl = ~uint64_t(0);

    for (uint32_t row = 0; row < total_rows; ++row) {
      // Decompose row into outer-dim local offsets (dim1 varies fastest).
      uint32_t outer[4] = {};
      uint32_t rem = row;
      for (uint32_t d = 1; d < rank; ++d) {
        outer[d - 1] = rem % tiles[d];
        rem /= tiles[d];
      }

      // OOB check on outer dims.
      bool row_in_bounds = true;
      for (uint32_t d = 1; d < rank; ++d) {
        if (w.req.coords[d] + outer[d - 1] >= desc.sizes[d]) { row_in_bounds = false; break; }
      }

      // First element GMEM addr in this row.
      uint64_t row_gbase = desc.base_addr + uint64_t(w.req.coords[0]) * elem_bytes;
      for (uint32_t d = 1; d < rank; ++d)
        row_gbase += uint64_t(w.req.coords[d] + outer[d - 1]) * uint64_t(desc.strides[d - 1]);

      // SMEM destination base for this row:
      //   Row-major:  base + row_idx * row_elems * elem_bytes  (i1 outer × i0 inner).
      //   K-major:    base + outer[0] * elem_bytes              (lane stride applies per e0 below).
      //   Tiled:      computed per-element below (smem_wr recomputes the permuted dest).
      uint64_t row_smem_base = dest_kmajor
          ? (w.req.smem_addr + uint64_t(outer[0]) * elem_bytes)
          : (w.req.smem_addr + uint64_t(row * row_elems) * elem_bytes);

      // Walk dim-0 elements; each LineWork carries one MemReq write of at
      // most one LMEM word AND at most one GMEM CL (so byteen fits in the
      // 64-byte mem_block payload). K-major scatters each element to its
      // own SMEM destination (lane stride = tile1 * elem_bytes).
      for (uint32_t e0 = 0; e0 < row_elems; ) {
        uint64_t gaddr_e = row_gbase + uint64_t(e0) * elem_bytes;
        uint64_t saddr_e;
        if (dest_tiled) {
          uint32_t de = tiled_dest_elem(layout, outer[0], e0, geo_ratio, geo_tcN, geo_nsteps, geo_bkK);
          saddr_e = w.req.smem_addr + uint64_t(de) * elem_bytes;
        } else if (dest_kmajor) {
          saddr_e = row_smem_base + uint64_t(e0) * uint64_t(per_lane_stride_bytes);
        } else {
          saddr_e = row_smem_base + uint64_t(e0) * elem_bytes;
        }
        uint64_t cl_addr = gaddr_e & kGmemLineMask;
        uint64_t sword   = saddr_e & ~uint64_t(kLmemWordSize - 1);
        uint32_t cl_off  = uint32_t(gaddr_e - cl_addr);
        uint32_t s_off   = uint32_t(saddr_e - sword);
        uint32_t cl_room  = kGmemLineSize - cl_off;
        uint32_t sw_room  = kLmemWordSize - s_off;
        uint32_t row_room = (row_elems - e0) * elem_bytes;
        // The GMEM read span is ALWAYS coalesced to the cache line: dim-0
        // elements are contiguous in global memory, so one cache-line read
        // covers as many as fit. K-major then SCATTERS those elements to
        // strided SMEM destinations (one read → km_num_elems writes); row-major
        // writes them contiguously (bounded by the SMEM word). This mirrors the
        // hardware address generator, which reads per cache line and never
        // re-reads per element.
        uint32_t gspan = scatter
            ? std::min({cl_room, row_room, uint32_t(VX_CFG_MEM_BLOCK_SIZE)})
            : std::min({cl_room, sw_room, row_room, uint32_t(VX_CFG_MEM_BLOCK_SIZE)});
        // Round to a whole-element multiple — ensures e0 advances by an
        // integer count and avoids off-by-one element splits.
        gspan -= gspan % elem_bytes;
        if (gspan == 0) break;
        uint32_t num_elems = gspan / elem_bytes;

        bool elem_oob = !row_in_bounds || (w.req.coords[0] + e0 >= desc.sizes[0]);
        // OOB is determined per-element at the start of the span; tile widths
        // are typically aligned to dim-0 size, so this is exact for the common case.

        LineWork lw{};
        lw.gmem_cl_addr     = cl_addr;
        lw.smem_word_addr   = sword;
        lw.cl_byte_offset   = cl_off;
        lw.smem_byte_offset = s_off;
        // Row-major: one contiguous write of the whole span. Scatter (K-major
        // or tiled Flat/BlockMajor): each element is its own write (elem_bytes).
        lw.valid_length     = scatter ? elem_bytes : gspan;
        lw.cfill            = cfill;
        lw.oob              = elem_oob;
        lw.last             = false;
        lw.scatter          = scatter;
        lw.km_num_elems     = scatter ? num_elems : 1;
        lw.km_lane_stride   = dest_kmajor ? per_lane_stride_bytes : 0;
        lw.dest_layout      = uint8_t(layout);
        lw.tile_k_row       = outer[0];   // tiled scatter: K-row (dim1)
        lw.tile_n_base      = e0;          // tiled scatter: N (dim0) span start

        // Dedup consecutive same-CL (non-OOB only).
        if (!elem_oob && cl_addr == global_prev_cl) {
          ++perf_stats_.gmem_dedup;
        } else if (!elem_oob) {
          global_prev_cl = cl_addr;
        }
        w.work_list.push_back(lw);

        e0 += num_elems;
      }
    }
  }

  struct GmemResponse {
    SimChannel<MemRsp>* port = nullptr;
    uint32_t slot = 0;
  };

  // ── smem_wr capture: one line per cycle into its line registers ───────
  // A GMEM response has priority over a pending OOB line. smem_wr holds one
  // line being written, one captured behind it, and the transfer's last line
  // while others are outstanding. A response it cannot take stays on the
  // memory response path and holds the responses queued behind it.
  void capture_line(Worker& w, const GmemResponse& response) {
    auto* port = response.port;
    // An OOB line holds an allocated slot.
    if (port == nullptr && w.num_issued == 0) {
      return;
    }
    // Captured lines other than the one draining, by kind.
    bool pend_busy  = false;
    bool defer_busy = false;
    for (uint32_t k = 0; k < SlotSet::kWords; ++k) {
      uint64_t arrived = w.rsp_arrived.bits[k];
      if (w.drain_slot != UINT32_MAX && w.drain_slot / 64 == k) {
        arrived &= ~(uint64_t(1) << (w.drain_slot % 64));
      }
      pend_busy  |= (arrived & ~w.deferred.bits[k]) != 0;
      defer_busy |= (arrived & w.deferred.bits[k]) != 0;
    }
    // DXA_LINE_PER_CYCLE: the held line moves on to the fill buffer this
    // cycle when nothing drains after this cycle's beat, and its register
    // takes the next line in the same cycle.
    if (VX_CFG_DXA_LINE_PER_CYCLE && w.drain_slot == UINT32_MAX) {
      pend_busy = false;
    }
    uint32_t oob_slot = w.oob_pending.first();

    uint32_t slot = oob_slot;
    if (port != nullptr) {
      slot = response.slot & (VX_CFG_DXA_MAX_INFLIGHT - 1);
      if (!w.allocated.test(slot)) {
        port->pop();
        return;
      }
    }
    if (slot == UINT32_MAX) {
      return;
    }

    auto& s = w.inflight[slot];
    bool defer = s.work.last && (w.num_issued > 1);
    if (defer ? defer_busy : pend_busy) {
      return;
    }
    if (port != nullptr) {
      s.rsp_data = port->peek().data;
      port->pop();
    }
    w.oob_pending.clear(slot);
    w.rsp_arrived.set(slot);
    if (defer) {
      w.deferred.set(slot);
    } else {
      w.deferred.clear(slot);
    }
    s.ready_cycle = cycle_ + kSmemWrLatency;
  }

  // ── gmem_req: issue MemReqs with available inflight slots ────────────
  void tick_worker_gmem_req(Worker& w) {
    // One AGU step per cycle: advance ag_idx by at most one, issuing at most
    // one GMEM read to this worker's ReqIn port (or one OOB skip). A while-loop
    // here would issue many reads to a single port in one cycle.
    if (w.ag_idx >= w.work_list.size())
      return;

    // Find a free slot: the lowest unallocated one past its free cycle.
    uint32_t slot = UINT32_MAX;
    for (uint32_t k = 0; k < SlotSet::kWords && slot == UINT32_MAX; ++k) {
      for (uint64_t m = ~w.allocated.bits[k] & SlotSet::valid(k); m != 0; m &= m - 1) {
        uint32_t s = 64 * k + __builtin_ctzll(m);
        if (cycle_ >= w.inflight[s].free_cycle) {
          slot = s;
          break;
        }
      }
    }

    const LineWork& lw = w.work_list[w.ag_idx];

    if (slot == UINT32_MAX) {
      // All slots in flight. Out-of-bounds lines issue no read, so they are
      // excluded: this counts only reads the slot pool actually held back.
      if (!lw.oob) {
        ++perf_stats_.noslot_stalls;
      }
      return;
    }

    // OOB lines skip the GMEM request entirely; smem_wr takes them in a cycle
    // with no GMEM response (rsp data left null — smem_wr will use cfill).
    if (lw.oob) {
      w.allocated.set(slot);
      w.rsp_arrived.clear(slot);
      w.oob_pending.set(slot);
      w.deferred.clear(slot);
      w.inflight[slot].rsp_data.reset();
      w.inflight[slot].work        = lw;
      ++w.num_issued;
      ++w.ag_idx;
      return;
    }

    // The port's request arbiter adds the worker index to the slot tag
    // so the port can route the returning line to the same worker.
    MemReq mreq;
    mreq.addr  = lw.gmem_cl_addr;
    mreq.tag   = slot;
    mreq.hart_id   = w.req.core->id();
    mreq.uuid  = w.req.uuid;

    auto& arb = simobject_->gmem_arbs_.at(w.worker_id % kDxaMemPorts);
    auto& ch = arb->Inputs.at(w.worker_id / kDxaMemPorts);
    if (!ch.try_send(mreq)) return; // arb backpressure

    w.allocated.set(slot);
    w.rsp_arrived.clear(slot);
    w.oob_pending.clear(slot);
    w.deferred.clear(slot);
    w.inflight[slot].rsp_data.reset();
    w.inflight[slot].work        = lw;
    ++w.num_issued;
    ++w.ag_idx;
  }

  // ── smem_wr: drain ready slots, build LMEM MemReqs ───────────────────

  // One block write of a drain beat, and the drain cursors after it.
  struct Emission {
    MemReq   req;
    uint64_t row       = 0;      // aligned LMEM row the write lands in
    uint32_t half      = 0;      // row part (LMEM DMA channel of the core)
    uint32_t slot      = 0;
    uint32_t km_next   = 0;
    uint32_t mc_next   = 0;
    bool     slot_done = false;  // the slot's last write: release it
    bool     scatter   = false;  // a scatter beat writes one element
    bool     notify    = false;  // carries the transfer's completion
  };

  // Pick the line the fill buffer drains next. False when nothing drains.
  bool pick_drain_slot(Worker& w) {
    if (w.num_issued == 0) {
      // Nothing in flight — if all addr_gen done, transfer is finished.
      if (w.ag_idx == w.work_list.size()) finish_worker(w);
      return false;
    }

    // Match the RTL's OoO direct-drain behavior: select any ready non-last
    // slot without waiting for older requests. Once selected, keep draining
    // the same slot because the multicast/scatter cursors are worker-local.
    // The last work item carries notify_done and must remain deferred until
    // it is the only outstanding slot.
    uint32_t slot = w.drain_slot;
    if (slot == UINT32_MAX) {
      for (uint32_t k = 0; k < SlotSet::kWords && slot == UINT32_MAX; ++k) {
        for (uint64_t m = w.allocated.bits[k] & w.rsp_arrived.bits[k]; m != 0; m &= m - 1) {
          uint32_t i = 64 * k + __builtin_ctzll(m);
          const auto& candidate = w.inflight[i];
          if (!candidate.work.last && cycle_ >= candidate.ready_cycle) {
            slot = i;
            break;
          }
        }
      }

      if (slot == UINT32_MAX && w.num_issued == 1) {
        uint32_t last_slot = w.allocated.first();
        const auto& candidate = w.inflight[last_slot];
        if (w.rsp_arrived.test(last_slot) && cycle_ >= candidate.ready_cycle)
          slot = last_slot;
      }

      if (slot == UINT32_MAX) return false;
      w.drain_slot = slot;
    }
    return true;
  }

  // The block write `slot` emits from scatter cursor `km` and multicast
  // cursor `mc`. One cache-line read fans out to its scattered writes, one
  // element per beat: the scatter destinations are not contiguous, so each
  // element is its own byte-masked write; mc replays the whole group to each
  // multicast receiver.
  Emission make_emission(const Worker& w, uint32_t slot, uint32_t km, uint32_t mc) const {
    constexpr uint64_t kRowMask = ~uint64_t(DxaCore::LMEM_ROW_SIZE - 1);
    const auto& s = w.inflight[slot];
    const LineWork& lw = s.work;

    const uint32_t num_elems = lw.km_num_elems ? lw.km_num_elems : 1;
    const uint32_t e0        = km;
    const uint32_t wlen      = lw.valid_length;

    uint32_t cta_warp_idx = 0;
    uint64_t cta_off = 0;
    if (w.is_multicast) {
      cta_warp_idx = w.cta_indices.at(mc);
      cta_off = uint64_t(cta_warp_idx) * w.smem_stride;
    }

    // Per-element SMEM byte destination. K-major: uniform lane stride from a
    // span base. Tiled (Flat/BlockMajor): permuted index from the WGMMA
    // B-buffer layout formula (idx = within-span dim-0 offset → N = base+idx).
    const bool     tiled    = desc_dest_tiled(w.desc.meta);
    const uint32_t t_eb     = desc_elem_bytes(w.desc.meta);
    const uint32_t t_ratio  = tiled ? std::max<uint32_t>(1u, 4u / t_eb) : 1u;
    const uint32_t t_tcN    = tiled ? desc_geo_tcn(w.desc) : 1u;
    const uint32_t t_bkK    = tiled ? desc_geo_bkk(w.desc) : 1u;
    const uint32_t t_nsteps = tiled ? std::max<uint32_t>(1u, uint32_t(w.desc.tile_sizes[0]) / t_tcN) : 1u;
    auto dest_byte_of = [&](uint32_t idx) -> uint64_t {
      if (tiled) {
        uint32_t de = tiled_dest_elem(DestLayout(lw.dest_layout), lw.tile_k_row,
                                      lw.tile_n_base + idx, t_ratio, t_tcN, t_nsteps, t_bkK);
        return w.req.smem_addr + uint64_t(de) * t_eb + cta_off;
      }
      return lw.smem_word_addr + lw.smem_byte_offset
           + uint64_t(idx) * lw.km_lane_stride + cta_off;
    };

    Emission e;
    e.slot    = slot;
    e.scatter = lw.scatter;

    // The block targeted this write = the block of scatter element e0.
    uint64_t base_byte0 = dest_byte_of(e0);
    uint64_t dword = base_byte0 & ~uint64_t(kLmemWordSize - 1);
    uint64_t row = base_byte0 & kRowMask;
    e.row = row;
    // Each row part has its own LMEM DMA channel.
    e.half = (DxaCore::LMEM_PORTS_PER_CORE > 1)
        ? uint32_t((dword / kLmemWordSize) & (DxaCore::LMEM_PORTS_PER_CORE - 1))
        : 0u;

    // Build LMEM MemReq with TLM payload.
    MemReq& req = e.req;
    req.addr   = dword;
    req.op = MemOp::ST;
    req.tag    = w.req.core->id();           // routing tag
    req.hart_id = w.req.core->id();
    req.uuid   = w.req.uuid;
    uint64_t byteen = 0;
    auto blk = make_mem_block();
    const uint32_t pat = lw.cfill;

    // A contiguous span is one write; a scatter span writes element e0.
    const uint32_t e_end = lw.scatter ? (e0 + 1) : num_elems;
    uint32_t ee = e0;
    for (; ee < e_end; ++ee) {
      uint64_t dest_byte = dest_byte_of(ee);
      if ((dest_byte & ~uint64_t(kLmemWordSize - 1)) != dword) break;
      if ((dest_byte & kRowMask) != row) {
        break;
      }
      uint32_t doff  = uint32_t(dest_byte - dword);
      uint64_t emask = (wlen >= 64) ? ~uint64_t(0) : ((uint64_t(1) << wlen) - 1ull);
      byteen |= emask << doff;
      if (s.rsp_data) {
        std::memcpy(blk->data() + doff,
                    s.rsp_data->data() + lw.cl_byte_offset + ee * wlen,
                    wlen);
      } else {
        for (uint32_t b = 0; b < wlen; ++b)
          (*blk)[doff + b] = uint8_t((pat >> ((b & 3) * 8)) & 0xff);
      }
    }
    req.byteen = byteen;
    req.data = blk;

    // notify_done on the LAST block write of the transfer — when the write
    // reached the last scatter element of the last work item, per receiver.
    bool is_last_elem   = (ee == num_elems);
    bool is_last_work   = lw.last;
    bool is_last_replay = !w.is_multicast || (mc + 1 == w.cta_indices.size());
    if (is_last_work && is_last_elem && (w.is_multicast || is_last_replay)) {
      req.flags.dxa_notify_done   = 1;
      req.flags.dxa_notify_bar_id = w.req.bar_id + (w.is_multicast ? cta_warp_idx : 0u);
      e.notify = true;
    }

    // Cursors after it: the next unwritten element, then the next receiver,
    // then the slot is done.
    if (!is_last_elem) {
      e.km_next = ee;
      e.mc_next = mc;
    } else {
      e.km_next = 0;
      if (w.is_multicast && (mc + 1) < w.cta_indices.size()) {
        e.mc_next = mc + 1;
      } else {
        e.mc_next = 0;
        e.slot_done = true;
      }
    }
    return e;
  }

  void commit_emission(Worker& w, const Emission& e) {
    ++w.writes_emitted;
    w.km_elem_idx = e.km_next;
    w.mc_cta_idx  = e.mc_next;
    if (e.slot_done) {
      auto& s = w.inflight[e.slot];
      w.allocated.clear(e.slot);
      w.rsp_arrived.clear(e.slot);
      w.deferred.clear(e.slot);
      s.free_cycle = cycle_ + 1;
      s.rsp_data.reset();
      --w.num_issued;
      w.drain_slot = UINT32_MAX;
    }
  }

  void tick_worker_smem_wr(Worker& w) {
    if (!this->pick_drain_slot(w))
      return;

    // Determine destination core's LMEM port group.
    uint32_t socket_local_cid = w.req.core->id() % kCoresPerSocket;

    // One drain beat per cycle covers one aligned LMEM row
    // (DxaCore::LMEM_ROW_SIZE bytes — the full banked row the RTL writes in
    // a single cycle). A row wider than the 64B mem_block byteen scope is
    // emitted as LMEM_PORTS_PER_CORE same-cycle block writes, one per row
    // half on its own LMEM DMA channel. A row narrower than a block
    // (small NT) bounds the per-beat gather instead.

    // A banked-row write is committed atomically, byte enables aside, so a row is
    // never half-written. The row spans LMEM_PORTS_PER_CORE channels that fill
    // independently, so gate the beat on the whole group being ready instead of
    // stranding the half already sent.
    for (uint32_t p = 0; p < DxaCore::LMEM_PORTS_PER_CORE; ++p) {
      if (simobject_->lmem_req_out.at(socket_local_cid * DxaCore::LMEM_PORTS_PER_CORE + p).full())
        return;
    }

    uint64_t beat_row = UINT64_MAX;

    for (uint32_t emitted = 0; emitted < DxaCore::LMEM_PORTS_PER_CORE; ++emitted) {
      if (w.drain_slot == UINT32_MAX) {
        break; // slot released mid-beat
      }
      Emission e = this->make_emission(w, w.drain_slot, w.km_elem_idx, w.mc_cta_idx);

      // Row constraint: every write of this beat stays in one aligned LMEM row.
      if (beat_row == UINT64_MAX) {
        beat_row = e.row;
      } else if (e.row != beat_row) {
        break; // next block lands in a different row — next cycle's beat
      }

      auto& lmem_ch = simobject_->lmem_req_out.at(
          socket_local_cid * DxaCore::LMEM_PORTS_PER_CORE + e.half);
      if (lmem_ch.full()) {
        break; // backpressure
      }

      lmem_ch.send(e.req);
      // Count write transactions, not port requests: one per banked-row beat.
      // A beat spans LMEM_PORTS_PER_CORE ports of the same row, so only its
      // first emission is counted.
      if (0 == emitted) {
        ++perf_stats_.lmem_writes;
      }
      this->commit_emission(w, e);
      if (e.scatter) {
        break; // a scatter beat writes one element
      }
    } // per-tick row-beat emission loop
  }

  // ── DXA_LMEM_ROW: one shared-memory write per cycle, a bank row ───────
  // A worker offers its next drain beat (the writes of one row, one per row
  // part it touches). Mirrors VX_dxa_lmem_arb: the round-robin lead, plus per
  // part the lead leaves empty the first worker after the lead whose beat
  // lies wholly in that part, for the same core, neither carrying the
  // completion. Every part channel of the core carries the write, an empty
  // part as a write with no byte enabled, so the parts stay in step.
  struct Offer {
    bool     valid = false;
    bool     last  = false;
    uint32_t mask  = 0;
    uint32_t core  = 0;
    uint32_t count = 0;
    std::array<Emission, DxaCore::LMEM_PORTS_PER_CORE> em;
  };

  void tick_lmem_row() {
    const uint32_t N = workers_.size();
    constexpr uint32_t P = DxaCore::LMEM_PORTS_PER_CORE;
    auto& offers = offers_;
    for (uint32_t i = 0; i < N; ++i) {
      auto& w = workers_[i];
      auto& o = offers[i];
      o.valid = false;
      o.last  = false;
      o.mask  = 0;
      o.count = 0;
      if (w.state != WState::RUNNING || !this->pick_drain_slot(w))
        continue;
      uint32_t slot = w.drain_slot;
      uint32_t km   = w.km_elem_idx;
      uint32_t mc   = w.mc_cta_idx;
      for (uint32_t k = 0; k < P; ++k) {
        Emission e = this->make_emission(w, slot, km, mc);
        if (k != 0 && e.row != o.em[0].row)
          break;
        o.mask |= 1u << e.half;
        o.last |= e.notify;
        o.em[o.count++] = e;
        if (e.slot_done || e.scatter)
          break;
        km = e.km_next;
        mc = e.mc_next;
      }
      o.valid = true;
      o.core  = w.req.core->id() % kCoresPerSocket;
    }

    uint32_t lead = UINT32_MAX;
    for (uint32_t k = 1; k <= N; ++k) {
      uint32_t i = (lmem_rr_ + k) % N;
      if (offers[i].valid) {
        lead = i;
        break;
      }
    }
    if (lead == UINT32_MAX)
      return;
    const auto& lo = offers[lead];
    for (uint32_t p = 0; p < P; ++p) {
      if (simobject_->lmem_req_out.at(lo.core * P + p).full())
        return;
    }

    std::array<uint32_t, P> owner;
    owner.fill(UINT32_MAX);
    for (uint32_t p = 0; p < P; ++p) {
      if (lo.mask & (1u << p)) {
        owner[p] = lead;
      } else if (!lo.last) {
        for (uint32_t k = 1; k < N; ++k) {
          uint32_t i = (lead + k) % N;
          const auto& o = offers[i];
          if (o.valid && o.mask == (1u << p) && !o.last && o.core == lo.core) {
            owner[p] = i;
            break;
          }
        }
      }
    }

    for (uint32_t p = 0; p < P; ++p) {
      auto& ch = simobject_->lmem_req_out.at(lo.core * P + p);
      if (owner[p] == UINT32_MAX) {
        MemReq nop;
        nop.addr    = lo.em[0].row + p * kLmemWordSize;
        nop.op      = MemOp::ST;
        nop.tag     = lo.em[0].req.tag;
        nop.hart_id = lo.em[0].req.hart_id;
        nop.uuid    = lo.em[0].req.uuid;
        nop.byteen  = 0;
        ch.send(nop);
        continue;
      }
      const auto& o = offers[owner[p]];
      for (uint32_t k = 0; k < o.count; ++k) {
        if (o.em[k].half == p) {
          ch.send(o.em[k].req);
        }
      }
    }

    for (uint32_t i = 0; i < N; ++i) {
      bool granted = (i == lead);
      for (uint32_t p = 0; p < P; ++p) {
        granted |= (owner[p] == i);
      }
      if (!granted)
        continue;
      ++perf_stats_.lmem_writes;
      auto& w = workers_[i];
      for (uint32_t k = 0; k < offers[i].count; ++k) {
        this->commit_emission(w, offers[i].em[k]);
      }
    }
    lmem_rr_ = lead;
  }

  void release_all_barriers(Worker& w) {
    // bar_id is RAW (encoded); decode at release call site.
    if (w.is_multicast) {
      for (uint32_t cta : w.cta_indices) {
        uint32_t decoded = bar_decode_id(w.req.bar_id + cta, VX_CFG_NUM_BARRIERS);
        w.req.core->barrier_event_release(decoded);
      }
    } else {
      uint32_t decoded = bar_decode_id(w.req.bar_id, VX_CFG_NUM_BARRIERS);
      w.req.core->barrier_event_release(decoded);
    }
  }

  void finish_worker(Worker& w) {
    if (w.state == WState::RUNNING || w.state == WState::IDLE) {
      uint64_t latency = cycle_ - w.issue_cycle;
      ++perf_stats_.transfers;
      perf_stats_.total_latency += latency;
      DT(3, simobject_->name() << "[" << w.worker_id << "] complete: core="
         << w.req.core->id() << ", bar=" << w.req.bar_id
         << ", writes=" << w.writes_emitted << ", latency=" << latency);
    }
    w.state = WState::IDLE;
    w.work_list.clear();
    w.cta_indices.clear();
    w.clear_slots();
    w.drain_slot = UINT32_MAX;
    w.ag_idx = 0;
    w.mc_cta_idx = 0;
    w.km_elem_idx = 0;
    w.writes_emitted = 0;
  }

  // ── Members ──────────────────────────────────────────────────────────
  DxaCore*    simobject_;

  std::array<Descriptor, VX_DCR_DXA_DESC_COUNT> descriptors_;
  std::deque<DxaReq>     queue_;
  std::vector<Worker>    workers_;
  std::vector<Offer>     offers_;
  uint32_t               lmem_rr_ = VX_CFG_NUM_DXA_CORES - 1;
  uint32_t               rr_req_ = 0;
  uint64_t               cycle_;
  DxaCore::PerfStats     perf_stats_;
};

// ════════════════════════════════════════════════════════════════════
// DxaCore — wrappers
// ════════════════════════════════════════════════════════════════════

DxaCore::DxaCore(const SimContext& ctx, const char* name, Socket* socket)
  : SimObject<DxaCore>(ctx, name)
  , dxa_req_in(kCoresPerSocket, this)
  , gmem_req_out(kDxaMemPorts, this)
  , gmem_rsp_in(kDxaMemPorts, this)
  , lmem_req_out(kCoresPerSocket * LMEM_PORTS_PER_CORE, this)
{
  __unused(socket);

  // Build the GMEM arbiters, one per L2-facing port: port i takes workers
  // i, i + kDxaMemPorts, ..., so a dispatch that fills the lowest idle
  // workers first spreads its transfers over the ports. Each worker's tag is
  // its inflight slot; the arbiter adds its input index for the response.
  char sname[100];
  for (uint32_t i = 0; i < kDxaMemPorts; ++i) {
    uint32_t num_workers = gmem_workers(i);
    if (kDxaMemPorts == 1) {
      snprintf(sname, 100, "%s-gmem-arb", name);
    } else {
      snprintf(sname, 100, "%s-gmem-arb%d", name, i);
    }
    auto arb = GmemReqArbiter::Create(sname, ArbiterType::RoundRobin, num_workers, 1);
    auto* output = &gmem_req_out.at(i);
  #ifdef SIMX_SOCKET_UNIT_MMU
    // The unit MMU's lookup starts from a registered request: the DXA owns it.
    snprintf(sname, 100, "%s-gmem-out%d", name, i);
    auto out_reg = MemReqSlice::Create(sname, 1);
    out_reg->Out.bind(output);
    output = &out_reg->In;
    gmem_out_regs_.push_back(out_reg);
  #endif
    uint32_t route_bits = log2ceil(num_workers);
    arb->Outputs.at(0).bind(output, [route_bits](const GmemReqArbiter::RspType& routed) {
      MemReq req(routed.data);
      req.tag = (req.tag << route_bits) | routed.input;
      return req;
    });
    gmem_arbs_.push_back(arb);
  }

  impl_ = new Impl(this);
}

DxaCore::~DxaCore() {
  delete impl_;
}

void DxaCore::on_reset() { impl_->reset(); }
void DxaCore::on_tick()  { impl_->tick(); }

int DxaCore::dcr_write(uint32_t addr, uint32_t value) {
  return impl_->dcr_write(addr, value);
}

const DxaCore::PerfStats& DxaCore::perf_stats() const {
  return impl_->perf_stats();
}
