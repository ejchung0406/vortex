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

#include "core.h"

#include <vector>
#include <list>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <cstring>

#include "scheduler.h"
#include "decode.h"
#include "decompressor.h"
#include "sequencer.h"
#include "cache.h"
#include "local_mem.h"
#include "local_mem_switch.h"
#include "dxa_core.h"
#include "lsu_mem_adapter.h"
#include "scoreboard.h"
#include "operands.h"
#include "dispatcher.h"
#include "func_unit.h"
#include "alu_unit.h"
#include "fpu_unit.h"
#include "lsu_unit.h"
#include "sfu_unit.h"
#include "csr_unit.h"
#include "mem_coalescer.h"
#include "socket.h"
#include "cluster.h"
#include "processor_impl.h"
#include "kmu.h"
#include "debug.h"
#ifdef VX_CFG_EXT_TCU_ENABLE
#include "tcu_unit.h"
#endif
#ifdef VX_CFG_VM_ENABLE
#include "mem/mmu.h"
#include <mem.h>
#endif

using namespace vortex;

#if (VX_CFG_FETCH_REQ_BUF == 0) && defined(VX_CFG_EXT_C_ENABLE)
#error "VX_CFG_FETCH_REQ_BUF=0 requires EXT_C off: the decompressor takes its fetches from the scheduler's buffer"
#endif

// Units whose output channel already holds every register ahead of the commit
// arbiter except the lane gather's.
static bool commit_single_hop(uint32_t fu) {
  switch ((FUType)fu) {
  case FUType::LSU:
  case FUType::SFU:
  case FUType::FPU:
#ifdef VX_CFG_EXT_TCU_ENABLE
  case FUType::TCU:
#endif
    return true;
  default:
    return false;
  }
}

// Lanes per pass of a unit whose lane gather merges the passes of one SIMD
// slice of an instruction into a single commit beat; 0 when the unit takes a
// whole SIMD slice per pass.
static uint32_t gather_merge_lanes(uint32_t fu) {
  uint32_t lanes = 0;
  switch ((FUType)fu) {
  case FUType::ALU: lanes = VX_CFG_NUM_ALU_LANES; break;
  case FUType::LSU: lanes = VX_CFG_NUM_LSU_LANES; break;
  case FUType::SFU: lanes = VX_CFG_NUM_SFU_LANES; break;
  case FUType::FPU: lanes = VX_CFG_NUM_FPU_LANES; break;
#ifdef VX_CFG_EXT_TCU_ENABLE
  case FUType::TCU: lanes = VX_CFG_NUM_TCU_LANES; break;
#endif
  default: break;
  }
  return (lanes < VX_CFG_SIMD_WIDTH) ? lanes : 0;
}

class Core::Impl {
public:
  Impl(const SimContext& ctx, Core* simobject)
    : simobject_(simobject)
    , sequencers_(VX_CFG_NUM_WARPS)
    , mpm_class_(0)
    , ibuffers_(VX_CFG_NUM_WARPS)
    , operands_(VX_CFG_ISSUE_WIDTH)
    , dispatchers_((uint32_t)FUType::Count)
    , func_units_((uint32_t)FUType::Count)
    , lmem_switch_(VX_CFG_NUM_LSU_BLOCKS)
    , mem_coalescers_(VX_CFG_NUM_LSU_BLOCKS)
    // As the scoreboard's out_arb: a matrix arbiter past 8 warps per slot.
    , ibuffer_arbs_(VX_CFG_ISSUE_WIDTH, {(PER_ISSUE_WARPS > 8) ? ArbiterType::GreedyMatrix : ArbiterType::GreedyRR, PER_ISSUE_WARPS})
    , issue_lock_owner_(VX_CFG_ISSUE_WIDTH, kLockOpen)
    , issue_yield_(VX_CFG_ISSUE_WIDTH, kNoYield)
    , fu_credits_(VX_CFG_ISSUE_WIDTH, std::vector<uint32_t>((uint32_t)FUType::Count, 0))
    , fu_full_(VX_CFG_ISSUE_WIDTH, BitVector<>((uint32_t)FUType::Count, 0))
    , fu_full_seen_(VX_CFG_ISSUE_WIDTH, BitVector<>((uint32_t)FUType::Count, 0))
    , ibuf_inflight_(VX_CFG_NUM_WARPS, 0)
    , ibuf_queued_(VX_CFG_NUM_WARPS, 0)
    , last_issue_(VX_CFG_NUM_WARPS, 0)
    , staged_since_(VX_CFG_NUM_WARPS, 0)
    , head_staged_(VX_CFG_NUM_WARPS, false)
  {
    const std::string& name = simobject_->name();
    char sname[100];

    // per frontend slice: scheduler-to-fetch and fetch-to-decode latches, and the
    // icache tags of the slice's in-flight fetches
    for (uint32_t f = 0; f < VX_CFG_FETCH_WIDTH; ++f) {
      snprintf(sname, 100, "fetch_latch%d", f);
      fetch_latch_.emplace_back(std::make_unique<TFifo<instr_trace_t*>>(ctx, sname, 1, 2));
      snprintf(sname, 100, "decode_latch%d", f);
      decode_latch_.emplace_back(std::make_unique<TFifo<instr_trace_t*>>(ctx, sname, 1, 2));
      pending_icache_.emplace_back(PER_FETCH_WARPS * VX_CFG_FETCH_INFLIGHT);
      for (uint32_t w = f; w < VX_CFG_NUM_WARPS; w += VX_CFG_FETCH_WIDTH) {
        fetch_warps_[f].set(w);
      }
    }

    // create scheduler (SimObject) — also creates the CTA dispatcher.
    snprintf(sname, 100, "%s-scheduler", name.c_str());
    scheduler_ = SimPlatform::instance().create_object<Scheduler>(sname, simobject_);

    // create decoder (SimObject)
    snprintf(sname, 100, "%s-decoder", name.c_str());
    decoder_ = SimPlatform::instance().create_object<Decoder>(sname, instr_pool_);

  #ifdef VX_CFG_EXT_C_ENABLE
    // create RVC fetch decompressor (SimObject) — owns per-warp half-word
    // buffer + refetch queue. Resets via SimObject::on_reset.
    for (uint32_t f = 0; f < VX_CFG_FETCH_WIDTH; ++f) {
      snprintf(sname, 100, "%s-decompressor%d", name.c_str(), f);
      decompressors_.push_back(SimPlatform::instance().create_object<Decompressor>(sname, VX_CFG_NUM_WARPS));
    }
  #endif

    // create scoreboard (SimObject)
    snprintf(sname, 100, "%s-scoreboard", name.c_str());
    scoreboard_ = SimPlatform::instance().create_object<Scoreboard>(sname);

    // create per-warp sequencers (SimObject)
    for (uint32_t w = 0; w < VX_CFG_NUM_WARPS; ++w) {
      snprintf(sname, 100, "%s-sequencer%d", name.c_str(), w);
      sequencers_.at(w) = SimPlatform::instance().create_object<Sequencer>(sname, simobject_, instr_pool_);
    }

    // create operands
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      snprintf(sname, 100, "%s-operand%d", name.c_str(), iw);
      operands_.at(iw) = Operands::Create(sname, simobject_);
    }

    // create ibuffers
    for (uint32_t i = 0; i < ibuffers_.size(); ++i) {
      snprintf(sname, 100, "%s-ibuffer%d", name.c_str(), i);
      // IBUF_SIZE entries plus the head, which stands for the warp's staging
      // register once it has moved there
      ibuffers_.at(i) = TFifo<instr_trace_t*>::Create(sname, 1, VX_CFG_IBUF_SIZE + 1);
    }

    // The LSU ticks ahead of the coalescer, the lane adapters and the lmem
    // switch. The switch routes a request into its output buffers in the cycle
    // the LSU hands it over, as the RTL's combinational split does. The
    // switch's one-entry response buffer is drained by the LSU before the
    // coalescer or an adapter refills it, its ready being combinational in the
    // RTL.
    snprintf(sname, 100, "%s-lsu", name.c_str());
    func_units_.at((int)FUType::LSU) = SimPlatform::instance().create_object<LsuUnit>(sname, simobject_);

    // create the memory coalescer, or the lane adapter that replaces it.
    const bool dcache_coalescing = (VX_CFG_NUM_LSU_LANES > 1) && (DCACHE_WORD_SIZE > LSU_WORD_SIZE);
    std::vector<LsuMemAdapter::Ptr>& lsu_dcache_adapter = lsu_dcache_adapters_;
    lsu_dcache_adapter.resize(VX_CFG_NUM_LSU_BLOCKS);
    for (uint32_t b = 0; b < VX_CFG_NUM_LSU_BLOCKS; ++b) {
      snprintf(sname, 100, "%s-coalescer%d", name.c_str(), b);
      // a batch crosses the coalescer's output register, then the lane
      // adapter's request buffer, on its way to the data cache
      mem_coalescers_.at(b) = MemCoalescer::Create(sname, LSU_CHANNELS, DCACHE_CHANNELS, DCACHE_WORD_SIZE, LSU_QUEUE_OUT_SIZE, 2);
      if (!dcache_coalescing) {
        snprintf(sname, 100, "%s-lsu_dcache_adapter%d", name.c_str(), b);
        lsu_dcache_adapter.at(b) = LsuMemAdapter::Create(sname, DCACHE_CHANNELS, 1, 1);
      }
    }

    // create local memory: it takes each LSU block's warp requests whole.
    // The DXA writes and the TCU tile-buffer reads share the row-wide DMA
    // port, round-robin, unless the reads use the banks' second port
    // (LMEM_TCU_RDPORT), which several LSU blocks bring.
    static_assert(uint64_t(VX_CFG_LMEM_SIZE) <= (uint64_t(1) << VX_CFG_LMEM_LOG_SIZE)
               && 2 * uint64_t(VX_CFG_LMEM_SIZE) > (uint64_t(1) << VX_CFG_LMEM_LOG_SIZE),
                  "LMEM_LOG_SIZE must be clog2(LMEM_SIZE)");
  #ifdef VX_CFG_LMEM_ENABLE
    static_assert(VX_CFG_LMEM_TCU_RDPORT == 0 || VX_CFG_NUM_LSU_BLOCKS > 1,
                  "LMEM_TCU_RDPORT=1 needs the banks' read-only port: NUM_LSU_BLOCKS > 1");
    static_assert(VX_CFG_LMEM_WR_ROW == 0 || VX_CFG_NUM_LSU_BLOCKS > 1,
                  "LMEM_WR_ROW=1 shares the banks between LSU blocks: NUM_LSU_BLOCKS > 1");
    static_assert(VX_CFG_LMEM_RD_ROW == 0 || VX_CFG_LMEM_WR_ROW != 0,
                  "LMEM_RD_ROW=1 sends the loads through the blocks' store engines: LMEM_WR_ROW=1");
    static_assert(0 == (uint64_t(VX_CFG_LMEM_SIZE) % (VX_CFG_MEM_BLOCK_SIZE * VX_CFG_LMEM_NUM_BANKS)),
                  "LMEM_SIZE must be a whole number of block rows across the banks");
    // The window ends at the next alignment boundary above its base.
    static_assert(0 == (uint64_t(VX_MEM_LMEM_BASE_ADDR) % (uint64_t(1) << VX_CFG_LMEM_LOG_SIZE)),
                  "LMEM exceeds its address window");
  #endif
    snprintf(sname, 100, "%s-lmem", name.c_str());
    local_mem_ = LocalMem::Create(sname, LocalMem::Config{
      VX_CFG_LMEM_SIZE,
      LSU_WORD_SIZE,
      VX_CFG_NUM_LSU_BLOCKS,
      LSU_CHANNELS,
      log2ceil(VX_CFG_LMEM_NUM_BANKS),
      VX_CFG_EXT_DXA_ENABLED + ((VX_CFG_LMEM_TCU_RDPORT != 0) ? 0 : VX_CFG_TCU_WGMMA_ENABLED),
      (VX_CFG_LMEM_TCU_RDPORT != 0) && (VX_CFG_TCU_WGMMA_ENABLED != 0)
    });

    // create lmem switch
    for (uint32_t b = 0; b < VX_CFG_NUM_LSU_BLOCKS; ++b) {
      snprintf(sname, 100, "%s-lmem_switch%d", name.c_str(), b);
      lmem_switch_.at(b) = LocalMemSwitch::Create(sname, 1);
    }

    // connect lmem switch to local memory
    for (uint32_t b = 0; b < VX_CFG_NUM_LSU_BLOCKS; ++b) {
      lmem_switch_.at(b)->ReqOutLmem.bind(&local_mem_->ReqIn.at(b));
      local_mem_->RspOut.at(b).bind(&lmem_switch_.at(b)->RspInLmem);
    }

    // Per-channel request/response endpoints facing the dcache ports.
    std::vector<SimChannel<MemReq>*> dc_req_out(VX_CFG_NUM_LSU_BLOCKS * DCACHE_CHANNELS);
    std::vector<SimChannel<MemRsp>*> dc_rsp_in(VX_CFG_NUM_LSU_BLOCKS * DCACHE_CHANNELS);

    if (dcache_coalescing) {
      // connect memory coalescer; its memory side drives the dcache
      // channels directly (combinational lane fan-out/fan-in).
      for (uint32_t b = 0; b < VX_CFG_NUM_LSU_BLOCKS; ++b) {
        lmem_switch_.at(b)->ReqOutDC.bind(&mem_coalescers_.at(b)->ReqIn);
        // the coalescer's response reaches the switch's response buffer
        // combinationally, ahead of a local-memory response
        mem_coalescers_.at(b)->RspOut.bind(&lmem_switch_.at(b)->RspOut);
        for (uint32_t c = 0; c < DCACHE_CHANNELS; ++c) {
          dc_req_out.at(b * DCACHE_CHANNELS + c) = &mem_coalescers_.at(b)->ReqOut.at(c);
          dc_rsp_in.at(b * DCACHE_CHANNELS + c) = &mem_coalescers_.at(b)->RspIn.at(c);
        }
      }
    } else {
      // bypass memory coalescer: per-block lane adapter (channel-fused
      // pass-through when DCACHE_CHANNELS == 1)
      for (uint32_t b = 0; b < VX_CFG_NUM_LSU_BLOCKS; ++b) {
        lmem_switch_.at(b)->ReqOutDC.bind(&lsu_dcache_adapter.at(b)->ReqIn);
        // A multi-channel adapter packs its response combinationally into the
        // switch's response buffer; a single-channel one passes the cache's
        // own output buffer through, which the switch then drains.
        if (DCACHE_CHANNELS > 1) {
          lsu_dcache_adapter.at(b)->RspOut.bind(&lmem_switch_.at(b)->RspOut);
        } else {
          lsu_dcache_adapter.at(b)->RspOut.bind(&lmem_switch_.at(b)->RspInDC);
        }
        for (uint32_t c = 0; c < DCACHE_CHANNELS; ++c) {
          dc_req_out.at(b * DCACHE_CHANNELS + c) = &lsu_dcache_adapter.at(b)->ReqOut.at(c);
          dc_rsp_in.at(b * DCACHE_CHANNELS + c) = &lsu_dcache_adapter.at(b)->RspIn.at(c);
        }
      }
    }

    // A data-cache port carrying the cache-flush injection goes through its
    // arbiter, which buffers and registers its requests once more. A private
    // dcache takes the injection on the first port only; a shared one on every
    // port, each carrying its own token behind its stores.
    constexpr bool dcache_private = (VX_CFG_SOCKET_SIZE == VX_CFG_NUM_DCACHES);
    dcache_flush_bufs_.resize(dcache_private ? 1 : dc_req_out.size());
    for (uint32_t p = 0; p < dcache_flush_bufs_.size(); ++p) {
      snprintf(sname, 100, "%s-dcache_flush_buf%d", name.c_str(), p);
      dcache_flush_bufs_.at(p) = TxBuffer<MemReq>::Create(sname, 2);
      dc_req_out.at(p)->bind(&dcache_flush_bufs_.at(p)->In);
      dc_req_out.at(p) = &dcache_flush_bufs_.at(p)->Out;
    }

    // The first icache port does the same for the icache, unless its arbiter
    // merges without a buffer.
    if (VX_CFG_FLUSH_REQ_BUF) {
      snprintf(sname, 100, "%s-icache_flush_buf", name.c_str());
      icache_flush_buf_ = TxBuffer<MemReq>::Create(sname, 2);
    }

  #ifdef VX_CFG_VM_ENABLE
    // Per-core L1 TLB stages: hits translate in place, misses park in
    // the miss station and resolve through the shared cluster TLB/PTW
    // over the TlbMissOut/TlbFillIn channels (bound at cluster level).
    uint32_t cores_per_cluster = NUM_SOCKETS * VX_CFG_SOCKET_SIZE;
    uint32_t local_core = simobject_->id() % cores_per_cluster;
    snprintf(sname, 100, "%s-dcache_mmu", name.c_str());
    dcache_mmu_ = Mmu::Create(sname, VX_CFG_DCACHE_NUM_REQS,
                              VX_CFG_DTLB_SIZE, local_core * 2 + 0, false,
                              false, true, false, true);

    // Per-core icache MMU, one port per frontend slice. Fetch reads/writes its
    // upstream channels directly; the downstream side is bound to the Core's
    // icache ports below.
    snprintf(sname, 100, "%s-icache_mmu", name.c_str());
    icache_mmu_ = Mmu::Create(sname, ICACHE_NUM_REQS,
                              VX_CFG_ITLB_SIZE, local_core * 2 + 1, true,
                              false, true, false, true);

    // LSU memory side -> dcache MMU -> core's dcache port
    for (uint32_t p = 0; p < VX_CFG_NUM_LSU_BLOCKS * DCACHE_CHANNELS; ++p) {
      dc_req_out.at(p)->bind(&dcache_mmu_->ReqIn.at(p));
      dcache_mmu_->RspOut.at(p).bind(dc_rsp_in.at(p));
      dcache_mmu_->ReqOut.at(p).bind(&simobject_->dcache_req_out.at(p));
      simobject_->dcache_rsp_in.at(p).bind(&dcache_mmu_->RspIn.at(p));
    }

    // icache MMU downstream side -> core's icache ports. Upstream side is
    // consumed directly by fetch() in on_tick.
    for (uint32_t f = 0; f < ICACHE_NUM_REQS; ++f) {
      icache_mmu_->ReqOut.at(f).bind(&simobject_->icache_req_out.at(f));
      simobject_->icache_rsp_in.at(f).bind(&icache_mmu_->RspIn.at(f));
    }
    if (icache_flush_buf_ != nullptr) {
      icache_flush_buf_->Out.bind(&icache_mmu_->ReqIn.at(0));
    }
  #else
    // No-VM: direct passthrough.
    for (uint32_t p = 0; p < VX_CFG_NUM_LSU_BLOCKS * DCACHE_CHANNELS; ++p) {
      dc_req_out.at(p)->bind(&simobject_->dcache_req_out.at(p));
      simobject_->dcache_rsp_in.at(p).bind(dc_rsp_in.at(p));
    }
    if (icache_flush_buf_ != nullptr) {
      icache_flush_buf_->Out.bind(&simobject_->icache_req_out.at(0));
    }
  #endif

    // initialize execute units. A full-bandwidth ALU or FPU takes its op
    // straight off the dispatch queue, in the cycle the queue releases it, so
    // it is created (and ticks) after its dispatcher; the others take their
    // input before the dispatcher fills it in the same cycle.
    auto create_alu = [&]() {
      snprintf(sname, 100, "%s-alu", name.c_str());
      func_units_.at((int)FUType::ALU) = SimPlatform::instance().create_object<AluUnit>(sname, simobject_);
    };
    auto create_fpu = [&]() {
      snprintf(sname, 100, "%s-fpu", name.c_str());
      func_units_.at((int)FUType::FPU) = SimPlatform::instance().create_object<FpuUnit>(sname, simobject_);
    };
    if (AluUnit::kGather) {
      create_alu();
    }
    if (FpuUnit::kGather) {
      create_fpu();
    }
    snprintf(sname, 100, "%s-sfu", name.c_str());
    func_units_.at((int)FUType::SFU) = SimPlatform::instance().create_object<SfuUnit>(sname, simobject_);
    std::static_pointer_cast<SfuUnit>(func_units_.at((int)FUType::SFU))->warp_ctl_out.bind(&simobject_->warp_ctl_in);
    scheduler_->barrier_unit().unlock_out.bind(&simobject_->bar_unlock_in);
  #ifdef VX_CFG_EXT_TCU_ENABLE
    snprintf(sname, 100, "%s-tcu", name.c_str());
    tcu_unit_ = SimPlatform::instance().create_object<TcuUnit>(sname, simobject_);
    func_units_.at((int)FUType::TCU) = tcu_unit_;
    // Created after the TCU so it ticks after it: a fill the TCU starts sends
    // its first read in the same cycle, seen by the LMEM the next.
    snprintf(sname, 100, "%s-tcu-tbuf", name.c_str());
    tcu_unit_->set_tbuf(TcuTbuf::Create(sname));
  #ifdef VX_CFG_TCU_TMEM_ENABLE
    scheduler_->cta_dispatcher()->cta_exit_out.bind(&tcu_unit_->cta_exit_in);
  #endif

  #ifdef VX_CFG_TCU_WGMMA_ENABLE
    // Bind the TCU tile-buffer subsystem (TcuTbuf) to the LMEM DMA port,
    // as the client after the DXA, or to the row read port.
    {
      auto& tbuf = tcu_unit_->tbuf();
      uint32_t base = VX_CFG_EXT_DXA_ENABLED * LocalMem::DMA_PORTS;
      for (uint32_t p = 0; p < LocalMem::DMA_PORTS; ++p) {
        if (VX_CFG_LMEM_TCU_RDPORT != 0) {
          tbuf->lmem_req_out.at(p).bind(&local_mem_->RdInputs.at(p));
          local_mem_->RdOutputs.at(p).bind(&tbuf->lmem_rsp_in.at(p));
        } else {
          tbuf->lmem_req_out.at(p).bind(&local_mem_->DmaInputs.at(base + p));
          local_mem_->DmaOutputs.at(base + p).bind(&tbuf->lmem_rsp_in.at(p));
        }
      }
    }
  #endif
  #ifdef TCU_META_ENABLE
    // Bind the TCU metadata AGU ports to the LSU client ports.
    {
      auto lsu = std::static_pointer_cast<LsuUnit>(func_units_.at((int)FUType::LSU));
      for (uint32_t p = 0; p < TCU_AGU_PORTS; ++p) {
        tcu_unit_->agu_req_out.at(p).bind(&lsu->TcuReqIn.at(p));
        lsu->TcuRspOut.at(p).bind(&tcu_unit_->agu_rsp_in.at(p));
      }
    }
  #endif
  #endif

    // initialize dispatchers after the buffered units they feed, so such a
    // unit takes its input before the dispatcher fills it in the same cycle.
    {
      // A reduced-width ALU or FPU takes the op through its lane-dispatch
      // buffer; a full-bandwidth one takes it as the queue releases it.
      uint32_t alu_dly = AluUnit::kGather ? 1 : 0;
      uint32_t fpu_dly = FpuUnit::kGather ? 1 : 0;
      dispatchers_.at((int)FUType::ALU) = SimPlatform::instance().create_object<Dispatcher>(name.c_str(), simobject_, VX_CFG_DISPATCH_QUEUE_SIZE, VX_CFG_NUM_ALU_BLOCKS, VX_CFG_NUM_ALU_LANES, alu_dly, ((VX_CFG_LANE_DISPATCH_GAPLESS >> 0) & 1) != 0);
      dispatchers_.at((int)FUType::FPU) = SimPlatform::instance().create_object<Dispatcher>(name.c_str(), simobject_, VX_CFG_DISPATCH_QUEUE_SIZE, VX_CFG_NUM_FPU_BLOCKS, VX_CFG_NUM_FPU_LANES, fpu_dly, ((VX_CFG_LANE_DISPATCH_GAPLESS >> 3) & 1) != 0);
      dispatchers_.at((int)FUType::LSU) = SimPlatform::instance().create_object<Dispatcher>(name.c_str(), simobject_, VX_CFG_DISPATCH_QUEUE_SIZE, VX_CFG_NUM_LSU_BLOCKS, VX_CFG_NUM_LSU_LANES, 1, ((VX_CFG_LANE_DISPATCH_GAPLESS >> 1) & 1) != 0);
      dispatchers_.at((int)FUType::SFU) = SimPlatform::instance().create_object<Dispatcher>(name.c_str(), simobject_, VX_CFG_DISPATCH_QUEUE_SIZE, VX_CFG_NUM_SFU_BLOCKS, VX_CFG_NUM_SFU_LANES, 1, ((VX_CFG_LANE_DISPATCH_GAPLESS >> 2) & 1) != 0);
    #ifdef VX_CFG_EXT_TCU_ENABLE
      dispatchers_.at((int)FUType::TCU) = SimPlatform::instance().create_object<Dispatcher>(name.c_str(), simobject_, VX_CFG_DISPATCH_QUEUE_SIZE, VX_CFG_NUM_TCU_BLOCKS, VX_CFG_NUM_TCU_LANES, 1, ((VX_CFG_LANE_DISPATCH_GAPLESS >> 4) & 1) != 0);
    #endif
      if (!AluUnit::kGather) {
        create_alu();
        auto& dispatch = dispatchers_.at((int)FUType::ALU);
        dispatch->set_release_on_accept();
        std::static_pointer_cast<AluUnit>(func_units_.at((int)FUType::ALU))->ReleaseOut.bind(&dispatch->ReleaseOut);
      }
      if (!FpuUnit::kGather) {
        create_fpu();
        auto& dispatch = dispatchers_.at((int)FUType::FPU);
        dispatch->set_release_on_accept();
        std::static_pointer_cast<FpuUnit>(func_units_.at((int)FUType::FPU))->ReleaseOut.bind(&dispatch->ReleaseOut);
      }
      std::static_pointer_cast<AluUnit>(func_units_.at((int)FUType::ALU))->branch_ctl_out.bind(&simobject_->branch_ctl_in);
      for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
        auto& dispatch = dispatchers_.at(fu);
        auto& func_unit = func_units_.at(fu);
        for (uint32_t b = 0; b < func_unit->num_blocks(); ++b) {
          dispatch->Outputs.at(b).bind(&func_unit->input(b));
        }
      }
    }

    // commit queues — per-iw, per-FU staging fed at runtime in commit() by
    // routing per-block FU outputs on trace->wid (no static binding because
    // the iw is not knowable at setup time when NUM_*_BLOCKS <
    // VX_CFG_ISSUE_WIDTH). Selection happens inside commit() itself so a
    // granted beat retires one registered stage after the FU presents it.
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      auto& queues = commit_queues_.emplace_back();
      for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
        // A unit's lane gather holds two results. The LSU's, the SFU's, the
        // FPU's and the TCU's cross one register into it; the ALU's crosses
        // two and takes a third slot so that it still retires one result per
        // cycle.
        uint32_t capacity = commit_single_hop(fu) ? 2 : 3;
        queues.emplace_back(std::make_unique<SimChannel<instr_trace_t*>>(simobject_, capacity));
        if (fu == (uint32_t)FUType::LSU) {
          // the LSU's gather buffer registers its ready, as its result path's
          // buffers do
          queues.back()->set_registered_ready(true);
        }
      }
    }
  #ifdef VX_CFG_EXT_TCU_ENABLE
    // one TCU block per issue slot, as VX_tcu_unit requires
    static_assert(VX_CFG_NUM_TCU_BLOCKS == VX_CFG_ISSUE_WIDTH, "one TCU block per issue slot");
    for (uint32_t b = 0; b < VX_CFG_NUM_TCU_BLOCKS; ++b) {
      tcu_stall_out_.emplace_back(std::make_unique<SimChannel<uint64_t>>(simobject_));
      tcu_stall_out_.back()->bind(&tcu_unit_->result_stall_in.at(b));
    }
  #endif
    gather_holds_.resize(VX_CFG_ISSUE_WIDTH, std::vector<GatherHold>((uint32_t)FUType::Count));

    // The channels the core polls but other units own wake a quiescent core
    // as a packet reaches them: a unit's result can come after its
    // instruction has left the pending count, as a split LSU instruction's
    // last response can arrive after its end-of-packet beat retired.
    for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
      auto& func_unit = func_units_.at(fu);
      for (uint32_t b = 0; b < func_unit->num_blocks(); ++b) {
        func_unit->output(b).tx_callback([this](instr_trace_t* const&, uint64_t) {
          this->wake();
        });
      }
      dispatchers_.at(fu)->ReleaseOut.tx_callback([this](instr_trace_t* const&, uint64_t) {
        this->wake();
      });
    }
    for (auto& operand : operands_) {
      operand->Output.tx_callback([this](instr_trace_t* const&, uint64_t) {
        this->wake();
      });
    }

    this->reset();
  }

  void reset() {
    // a reset re-arms every object; the ticks skipped up to it are counted
    if (asleep_) {
      this->account_sleep(SimPlatform::instance().cycles());
    }
    asleep_ = false;
    for (auto& arb : ibuffer_arbs_) {
      arb.reset();
    }
    for (auto& fc : fu_credits_) {
      std::fill(fc.begin(), fc.end(), 0);
    }
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      fu_full_.at(iw).reset();
      fu_full_seen_.at(iw).reset();
    }
    std::fill(issue_lock_owner_.begin(), issue_lock_owner_.end(), kLockOpen);
    std::fill(issue_yield_.begin(), issue_yield_.end(), kNoYield);
    std::fill(last_issue_.begin(), last_issue_.end(), 0);
    std::fill(staged_since_.begin(), staged_since_.end(), 0);
    std::fill(head_staged_.begin(), head_staged_.end(), false);
    std::fill(scrb_waits_.begin(), scrb_waits_.end(), ScrbWait());

    std::fill(pending_instrs_.begin(), pending_instrs_.end(), 0);
    pending_total_ = 0;
    pending_ifetches_ = 0;

    for (auto& holds : gather_holds_) {
      std::fill(holds.begin(), holds.end(), GatherHold());
    }

    std::fill(ibuf_inflight_.begin(), ibuf_inflight_.end(), 0);
    std::fill(ibuf_queued_.begin(), ibuf_queued_.end(), 0);
    ibuf_full_.reset();
    ibuf_full_r_.reset();
    ibuf_queue_full_.reset();
    ibuf_queue_full_r_.reset();
    for (uint32_t w = 0; w < VX_CFG_NUM_WARPS; ++w) {
      ibuf_held_.set(w, ibuffers_.at(w)->size() != 0);
    }

    // perf_stats_ deliberately survives: these are the MPM counters, which
    // accumulate from device reset. A kernel launch re-enters reset() here but
    // does not reset the hardware, so clearing them would make each dump report
    // only the last launch -- an app that dumps between launches would then see
    // per-launch counts where the device reports a running total.
  }

  void tick() {
    if (asleep_) {
      this->account_sleep(SimPlatform::instance().cycles());
      asleep_ = false;
    }
    this->commit();
    this->return_dispatch_credits();
    this->issue();
    this->update_queue_full();
    this->decode();
    this->fetch();
    this->schedule();

    // The cycle counter advances only while the core has work -- a CTA
    // launching, a warp active, or an instruction in flight -- so cycles it
    // spends idle before its first CTA, after its last warp, or while the host
    // moves data are not charged to it.
    if (scheduler_->running() || pending_total_ != 0) {
      ++perf_stats_.cycles;
    }
    DPN(2, std::flush);

    if (this->quiescent()) {
      asleep_ = true;
      asleep_after_ = SimPlatform::instance().cycles();
      asleep_active_ = scheduler_->active_warps().count();
      asleep_stalled_ = scheduler_->stalled_warps().count();
      simobject_->tick_sleep();
    }
  }

  // ----- Quiescence -----
  //
  // A core with no warp, no CTA on its way and nothing in its pipeline sleeps:
  // each tick it skips would only count an idle scheduler cycle and its
  // (inactive) stalled warps, which the next tick, a reset or a counter read
  // adds in at once. Anything that can give it work wakes it: a packet sent
  // toward one of its channels (its CTA launch lane among them), a packet
  // reaching a unit output, credit return or collector output it polls, or a
  // call that changes its warps' state.

  // The next tick would change nothing but those counters.
  bool quiescent() const {
    if (pending_total_ != 0 || pending_ifetches_ != 0 || ibuf_held_.any()) {
      return false;
    }
    if (!scheduler_->quiescent()) {
      return false;
    }
    if (simobject_->branch_ctl_in.size() != 0
     || simobject_->warp_ctl_in.size() != 0
     || simobject_->bar_unlock_in.size() != 0) {
      return false;
    }
    for (uint32_t f = 0; f < VX_CFG_FETCH_WIDTH; ++f) {
      if (fetch_latch_.at(f)->size() != 0 || decode_latch_.at(f)->size() != 0
       || fetch_drop_.at(f).valid) {
        return false;
      }
    #ifdef VX_CFG_VM_ENABLE
      if (icache_mmu_->RspOut.at(f).size() != 0) {
        return false;
      }
    #else
      if (simobject_->icache_rsp_in.at(f).size() != 0) {
        return false;
      }
    #endif
    #ifdef VX_CFG_EXT_C_ENABLE
      if (!decompressors_.at(f)->quiescent()) {
        return false;
      }
    #endif
    }
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      if (operands_.at(iw)->Output.size() != 0
       || issue_yield_.at(iw) != kNoYield
       || fu_full_seen_.at(iw) != fu_full_.at(iw)) {
        return false;
      }
      for (auto& queue : commit_queues_.at(iw)) {
        if (queue->size() != 0) {
          return false;
        }
      }
    }
    for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
      if (dispatchers_.at(fu)->ReleaseOut.size() != 0) {
        return false;
      }
      auto& func_unit = func_units_.at(fu);
      for (uint32_t b = 0, nb = func_unit->num_blocks(); b < nb; ++b) {
        if (func_unit->output(b).size() != 0) {
          return false;
        }
      }
    }
    return true;
  }

  // Count the ticks skipped since the core fell asleep, those of the cycles
  // before `now`, as the scheduler would have: idle, with the warp counts it
  // had then (a launch resets the scheduler before the core).
  void account_sleep(uint64_t now) {
    if (now <= asleep_after_ + 1) {
      return; // read in the cycle the core fell asleep, or the one after
    }
    uint64_t skipped = now - 1 - asleep_after_;
    perf_stats_.sched_idle    += skipped;
    perf_stats_.active_warps  += skipped * asleep_active_;
    perf_stats_.stalled_warps += skipped * asleep_stalled_;
    asleep_after_ = now - 1;
  }

  void wake() {
    simobject_->tick_wake();
  }

  // ----- Pipeline stages (private) -----

  // Abort when a warp's staged instruction has not issued for STALL_TIMEOUT
  // cycles, naming the registers it still waits on.
  void check_issue_timeout(uint64_t now, uint32_t iw, uint32_t wid, instr_trace_t* trace, instr_trace_t* uop_trace) {
    uint64_t staged = std::max(last_issue_[wid], trace->ibuf_time);
    uint64_t stall_cycles = now - staged;
    if (__builtin_expect(stall_cycles < VX_DBG_STALL_TIMEOUT, 1)) {
      return;
    }
    std::cerr << "*** " << simobject_->name() << " issue timeout: wid=" << wid
              << " (issue" << iw << "), PC=0x" << std::hex << uop_trace->PC << std::dec
              << ", tmask=" << uop_trace->tmask << " (lane 0 first), cycles=" << stall_cycles
              << ", inuse={";
    if (scoreboard_->in_use(uop_trace)) {
      auto uses = scoreboard_->get_uses(uop_trace);
      for (uint32_t j = 0; j < uses.size(); ++j) {
        auto& use = uses.at(j);
        std::cerr << (j ? ", " : "") << use.reg_type << use.reg_id << " (#" << use.uuid << ")";
      }
    }
    std::cerr << "}, " << *uop_trace << std::endl;
    std::abort();
  }

  void schedule() {
    // profiling
    perf_stats_.active_warps += scheduler_->active_warps().count();
    perf_stats_.stalled_warps += scheduler_->stalled_warps().count();

    scheduler_->schedule_begin();

    // The all-full bypass is limited to single-fetch mode. With fetch ahead,
    // each pick reserves a slot so responses can drain without waiting on issue.
    bool all_full = (VX_CFG_ICACHE_ENABLED || VX_CFG_DCACHE_ENABLED)
                 && (VX_CFG_FETCH_INFLIGHT == 1)
                 && (ibuf_full_r_.count() == VX_CFG_NUM_WARPS);
    // the cycle is idle when no slice has a warp to fetch, its latch free or not
    bool any_cand = false;
    for (uint32_t f = 0; f < VX_CFG_FETCH_WIDTH; ++f) {
      auto& fetch_latch = fetch_latch_.at(f);
      WarpMask room_mask = fetch_warps_[f] & ~ibuf_full_r_;
      WarpMask warp_mask = all_full ? fetch_warps_[f] : room_mask;

      // Without the scheduler's buffer the pick goes straight into the
      // fetch request buffer, and waits for its room.
      bool held = VX_CFG_FETCH_REQ_BUF ? fetch_latch->full()
                                       : (this->fetch_req_port(f).full() || pending_icache_.at(f).full());
      if (held) {
        bool ready = scheduler_->has_ready(warp_mask, room_mask);
        if (!VX_CFG_FETCH_REQ_BUF && ready) {
          ++perf_stats_.fetch_stalls;
        }
        any_cand |= ready;
        continue;
      }

      auto trace = scheduler_->schedule(warp_mask, room_mask);
      if (trace == nullptr) {
        continue;
      }
      any_cand = true;

      DT(3, simobject_->name() << "-pipeline schedule: " << *trace);
      if (VX_CFG_FETCH_REQ_BUF) {
        fetch_latch->push(trace);
      } else {
        bool sent = this->send_fetch(f, trace, trace->PC);
        __assert(sent, "a fetch pick was made with its request port full");
      }
      // with fetch ahead an instruction is pending once decode accepts it: a
      // fetch that will be dropped is not one
      if (VX_CFG_FETCH_INFLIGHT == 1) {
        this->pending_add(trace->wid);
      }
      ++ibuf_inflight_.at(trace->wid);
      this->ibuf_count_changed(trace->wid);
      perf_stats_.issued_warps += 1;
      perf_stats_.issued_threads += trace->tmask.count();
    }
    if (!any_cand) {
      ++perf_stats_.sched_idle;
    }
    ibuf_full_r_ = ibuf_full_;
    ibuf_queue_full_r_ = ibuf_queue_full_;

    scheduler_->schedule_end();
  }

  void fetch() {
    perf_stats_.ifetch_latency += pending_ifetches_;
    for (uint32_t f = 0; f < VX_CFG_FETCH_WIDTH; ++f) {
      this->fetch_slice(f);
    }
  }

  // Frontend slice f: its icache response into its decode latch, then its next
  // fetch request out of its fetch latch, on icache port f.
  void fetch_slice(uint32_t f) {
    auto& fetch_latch    = *fetch_latch_.at(f);
    auto& decode_latch   = *decode_latch_.at(f);
    auto& pending_icache = pending_icache_.at(f);
  #ifdef VX_CFG_EXT_C_ENABLE
    auto& decompressor   = decompressors_.at(f);
  #endif

  #ifdef VX_CFG_VM_ENABLE
    // With VM, fetch reads/writes the icache MMU's upstream channels.
    // The MMU translates and forwards to simobject_->icache_req_out
    // downstream (driven by SimChannel binding in the constructor).
    auto& icache_rsp = icache_mmu_->RspOut.at(f);
  #else
    auto& icache_rsp = simobject_->icache_rsp_in.at(f);
  #endif

    // Fetch ahead: a response is decoded only as the next instruction of its
    // warp's current stream. One of a squashed stream is dropped; one that
    // overtook an older fetch is dropped and squashes the stream, which then
    // restarts at the overtaken fetch. A dropped response needs no buffer slot;
    // the scheduler learns of it a cycle later, as of a decoded one.
    bool rsp_dropped = false;
    if (VX_CFG_FETCH_INFLIGHT > 1 && !icache_rsp.empty()) {
      auto& mem_rsp = icache_rsp.peek();
      auto trace = pending_icache.at(mem_rsp.tag);
      auto& fd = fetch_dec_.at(trace->wid);
      bool stale = (trace->fetch_strm != fd.strm);
      // a fetch that follows in a run is the next instruction only if the run
      // is the one whose first fetch was decoded last, with no squash since
      bool in_run = fd.open && (trace->fetch_run == fd.run);
      uint32_t ahead = (trace->fetch_seq + VX_CFG_FETCH_INFLIGHT - (in_run ? fd.head : 0)) % VX_CFG_FETCH_INFLIGHT;
      bool overtook = !stale && !trace->fetch_first && (!in_run || ahead != 0);
      if (stale || overtook) {
        DT(3, simobject_->name() << " icache-rsp drop: " << *trace);
        if (overtook) {
          // the stream is sequential: the overtaken fetch is `ahead` words back
          assert(trace->PC - 4 * ahead == scheduler_->warp(trace->wid).PC);
          fd.strm ^= 1;
          fd.open = false;
        }
        auto& drop = fetch_drop_.at(f);
        assert(!drop.valid);
        drop = {true, trace->wid, overtook};
        pending_icache.release(mem_rsp.tag);
        icache_rsp.pop();
        --pending_ifetches_;
        trace->~instr_trace_t();
        trace_pool_.deallocate(trace, 1);
        rsp_dropped = true;
      }
    }

    // handle icache response — extract instruction word from the TLM line payload
    if (!rsp_dropped && !icache_rsp.empty()){
      auto& mem_rsp = icache_rsp.peek();
      auto trace = pending_icache.at(mem_rsp.tag);
      assert(mem_rsp.data && "icache response must carry line payload");

      // Decode passes the response straight into its warp's buffer, so a full
      // buffer holds it at the icache output, in front of the responses behind
      // it. The buffer's full flag is registered: a slot it frees this cycle
      // takes an instruction from the next.
      bool ibuf_room = !ibuf_queue_full_r_.test(trace->wid);
      __assert(ibuf_room || VX_CFG_FETCH_INFLIGHT == 1, "a fetch ahead waits for its warp's buffer");
    #ifdef VX_CFG_EXT_C_ENABLE
      // Decompressor classifies the rsp: emit-ready (sets trace->code) or
      // queues a refetch internally for cross-word 32-bit completion.
      bool emit = decompressor->on_icache_rsp(trace, *mem_rsp.data);
      bool progressed = !emit || (ibuf_room && decode_latch.try_push(trace));
      if (emit && progressed) {
        decompressor->on_emit(trace);
      }
    #else
      bool emit = true;
      bool progressed = false;
      if (ibuf_room && decode_latch.try_push(trace)) {
        uint32_t offset = trace->PC & (VX_CFG_MEM_BLOCK_SIZE - 1);
        std::memcpy(&trace->code, mem_rsp.data->data() + offset, sizeof(uint32_t));
        progressed = true;
      }
    #endif
      if (emit && !ibuf_room) {
        if (!trace->log_once(true)) {
          DT(4, simobject_->name() << " ibuffer-stall: " << *trace);
        }
        ++perf_stats_.ibuf_stalls;
      }
      if (progressed) {
        if (emit) {
          trace->log_once(false);
          ++ibuf_queued_.at(trace->wid);
          this->ibuf_count_changed(trace->wid);
          if (VX_CFG_FETCH_INFLIGHT > 1) {
            auto& fd = fetch_dec_.at(trace->wid);
            fd.head = (trace->fetch_seq + 1) % VX_CFG_FETCH_INFLIGHT;
            if (trace->fetch_first) {
              fd.run  = trace->fetch_run;
              fd.open = true;
            }
          }
          DP(1, "Fetch: code=0x" << std::hex << trace->code << std::dec << ", cid=" << trace->cid
                 << ", wid=" << trace->wid << ", tmask=" << trace->tmask
                 << ", PC=0x" << std::hex << trace->PC << " (#" << std::dec << trace->uuid << ")");
        }
        DT(3, simobject_->name() << " icache-rsp: addr=0x" << std::hex << trace->PC << ", tag=0x" << mem_rsp.tag << std::dec << ", " << *trace);
        pending_icache.release(mem_rsp.tag);
        icache_rsp.pop();
        --pending_ifetches_;
      }
    }

    // send icache request — under EXT_C the FSM drains pending refetches
    // (cross-word completions) before fresh schedule traces.
    instr_trace_t* trace = nullptr;
    Word           req_addr = 0;
  #ifdef VX_CFG_EXT_C_ENABLE
    bool           from_refetch = false;
    {
      auto pick = decompressor->pick_request(fetch_latch.empty() ? nullptr
                                                              : fetch_latch.peek());
      trace        = pick.trace;
      req_addr     = pick.addr;
      from_refetch = pick.from_refetch;
    }
  #else
    if (!fetch_latch.empty()) {
      trace    = fetch_latch.peek();
      req_addr = trace->PC;
    }
  #endif
    if (trace == nullptr)
      return;

    // Avoid leaking icache tags when the request port back-pressures.
    if (pending_icache.full()) {
      ++perf_stats_.fetch_stalls;
      return;
    }

    if (this->send_fetch(f, trace, req_addr)) {
    #ifdef VX_CFG_EXT_C_ENABLE
      decompressor->commit_request(from_refetch);
      if (!from_refetch) { fetch_latch.pop(); }
    #else
      fetch_latch.pop();
    #endif
    } else {
      ++perf_stats_.fetch_stalls;
    }
  }

  // Frontend slice f's icache request port: the first one passes the cache
  // flush injector's buffer, if it has one.
  SimChannel<MemReq>& fetch_req_port(uint32_t f) {
    if (f == 0 && icache_flush_buf_ != nullptr) {
      return icache_flush_buf_->In;
    }
  #ifdef VX_CFG_VM_ENABLE
    return icache_mmu_->ReqIn.at(f);
  #else
    return simobject_->icache_req_out.at(f);
  #endif
  }

  // The fetch request buffer registers the request on its way to the icache.
  bool send_fetch(uint32_t f, instr_trace_t* trace, Word addr) {
    auto& pending_icache = pending_icache_.at(f);
    MemReq mem_req;
    // Address goes downstream as VA. The icache MMU (under VX_CFG_VM_ENABLE)
    // substitutes PA before forwarding; with VM off, this is just the PA.
    mem_req.addr    = addr;
    // op defaults to MemOp::LD — icache request is a load.
    uint32_t tag    = pending_icache.allocate(trace);
    mem_req.tag     = tag;
    mem_req.hart_id = trace->cid;
    mem_req.uuid    = trace->uuid;
    if (!this->fetch_req_port(f).try_send(mem_req, 1)) {
      pending_icache.release(tag);
      return false;
    }
    DT(3, simobject_->name() << " icache-req: addr=0x" << std::hex << mem_req.addr << ", tag=0x" << mem_req.tag << std::dec << ", " << *trace);
    ++perf_stats_.ifetches;
    ++pending_ifetches_;
    return true;
  }

  void decode() {
    for (uint32_t f = 0; f < VX_CFG_FETCH_WIDTH; ++f) {
      this->decode_slice(f, *decode_latch_.at(f));
    }
  }

  // One frontend slice's decoder: it feeds the issue slice of each decoded warp.
  void decode_slice(uint32_t f, TFifo<instr_trace_t*>& decode_latch) {
    // a fetch dropped at the icache response last cycle: its buffer slot
    // returns, and the scheduler sees it leave now
    auto& drop = fetch_drop_.at(f);
    if (drop.valid) {
      assert(decode_latch.empty());
      scheduler_->fetch_leave(drop.wid, drop.squash);
      assert(ibuf_inflight_.at(drop.wid) > 0);
      --ibuf_inflight_.at(drop.wid);
      this->ibuf_count_changed(drop.wid);
      drop.valid = false;
    }

    if (decode_latch.empty()) {
      return;
    }

    auto trace = decode_latch.peek();

    // the icache response took the buffer slot this instruction lands in
    auto& ibuffer = ibuffers_.at(trace->wid);
    assert(!ibuffer->full());

    // Decode: fill trace metadata from instruction bits. The decoder
    // recognises RVC encoding (low 2 bits != 11), expands internally, and
    // stamps IntrBrArgs.is_rvc.
    auto instr = decoder_->decode(trace->code, trace->uuid);
    DP(1, "Instr: " << *instr << " (#" << trace->uuid << ")");
    trace->instr_ptr   = instr;
    trace->fu_type     = instr->get_fu_type();
    trace->op_type     = instr->get_op_type();
    trace->dst_reg     = instr->get_dest_reg();
    trace->dst_bytesel = instr->get_dst_bytesel();
    for (uint32_t i = 0; i < NUM_SRC_REGS; ++i) {
      trace->src_regs[i] = instr->get_src_reg(i);
    }
    // Filter x0 for integer dest: writes to x0 are silent in RISC-V, so don't
    // reserve a scoreboard slot — the unit's writeback skips x0 too.
    auto dst = instr->get_dest_reg();
    trace->wb = (dst.type != RegType::None)
             && !(dst.type == RegType::Integer && dst.idx == 0);
    trace->fetch_stall = instr->is_wstall();
    // A warp blocked on a stalling instruction is released when that
    // instruction commits. Default to releasing every stalling instruction;
    // warp-control (SFU) refines this for ops whose warp is instead released by
    // the barrier/spawn machinery (see SfuUnit::on_tick).
    trace->resume_warp = trace->fetch_stall;

    // Advance the warp's PC by the instruction's true size (2 bytes for RVC,
    // 4 bytes otherwise). Branch/JAL/JALR commit later overrides warp.PC
    // with the resolved target.
    {
      bool is_rvc = (trace->code & 0x3u) != 0x3u;
      scheduler_->advance_pc(trace, is_rvc ? 2 : 4);
    }

    // Resume warp for non-stalling instructions (ALU, FPU);
    // stalling instructions (LSU, SFU, TCU, branches) stay suspended until commit
    if (VX_CFG_FETCH_INFLIGHT > 1) {
      // A stalling instruction holds its warp from here, and what was fetched
      // behind it is squashed.
      this->pending_add(trace->wid);
      scheduler_->fetch_leave(trace->wid, trace->fetch_stall);
      if (trace->fetch_stall) {
        fetch_dec_.at(trace->wid).strm ^= 1;
        fetch_dec_.at(trace->wid).open = false;
        scheduler_->suspend(trace->wid);
      }
    } else if (!trace->fetch_stall) {
      scheduler_->resume(trace->wid);
    }

    DT(3, simobject_->name() << "-pipeline decode: " << *trace);

    // insert to ibuffer
    trace->ibuf_time = SimPlatform::instance().cycles();
    ibuffer->push(trace);
    ibuf_held_.set(trace->wid);

    // An instruction that arrives with the warp's buffer and staging register
    // both empty moves into the register at once, and its buffer slot frees
    // this cycle; issue() stages the ones that wait behind another.
    if (!head_staged_.at(trace->wid) && ibuf_queued_.at(trace->wid) == 1 && !instr->is_macro_op()) {
      head_staged_.at(trace->wid) = true;
      this->ibuf_release(trace->wid);
    }

    decode_latch.pop();
  }

  // Whether the scoreboard blocks the warp's staged micro-op. A micro-op found
  // blocked stays blocked until the scoreboard frees one of the warp's
  // registers, so the answer is kept until then.
  bool scrb_blocked(uint32_t wid, instr_trace_t* uop_trace) {
    auto& wait = scrb_waits_[wid];
    uint32_t gen = scoreboard_->release_gen(wid);
    if (wait.uop == uop_trace && wait.gen == gen) {
      return true;
    }
    bool blocked = scoreboard_->in_use(uop_trace);
    wait.uop = blocked ? uop_trace : nullptr;
    wait.gen = gen;
    return blocked;
  }

  void issue() {
    const uint64_t now = SimPlatform::instance().cycles();
    // dispatch operands
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      auto& operand = operands_.at(iw);
      if (operand->Output.empty())
        continue;
      auto trace = operand->Output.peek();
      // The collector's output register adds a cycle before the slot queue.
      if (dispatchers_.at((int)trace->fu_type)->Inputs.at(iw).try_send(trace, 2)) {
        operand->Output.pop();
      } else {
        switch (trace->fu_type) {
        case FUType::ALU: ++perf_stats_.alu_stalls; break;
        case FUType::FPU: ++perf_stats_.fpu_stalls; break;
        case FUType::LSU: ++perf_stats_.lsu_stalls; break;
        case FUType::SFU: ++perf_stats_.sfu_stalls; break;
      #ifdef VX_CFG_EXT_TCU_ENABLE
        case FUType::TCU: ++perf_stats_.tcu_stalls; break;
      #endif
        default: assert(false);
        }
      }
    }

    // issue ibuffer instructions
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      bool any_scrb_blocked = false;
      BitVector<> ready_set(PER_ISSUE_WARPS);
      BitVector<> yield_set(PER_ISSUE_WARPS);
      auto yielded = issue_yield_.at(iw);
      issue_yield_.at(iw) = kNoYield;
      for (uint32_t w = 0; w < PER_ISSUE_WARPS; ++w) {
        uint32_t wid = w * VX_CFG_ISSUE_WIDTH + iw;
        if (!ibuf_held_.test(wid))
          continue;
        auto& ibuffer = ibuffers_[wid];
        if (ibuffer->empty())
          continue;

        // check scoreboard against sequencer micro-op (handles uop WAW/RAW)
        auto trace = ibuffer->peek();

        auto& seq = sequencers_[wid];
        auto uop_trace = seq->get(trace);  // returns cached uop or generates next

        // The head moves into the warp's staging register, freeing its buffer
        // slot, once it has arrived and the warp's previous instruction has
        // issued; a macro-op leaves the buffer with its last micro-op. The
        // scheduler sees the freed slot the cycle after.
        if (!head_staged_.at(wid)
         && (!trace->instr_ptr->is_macro_op() || seq->last())
         && now > std::max(last_issue_.at(wid), trace->ibuf_time)) {
          head_staged_.at(wid) = true;
          this->ibuf_release(wid);
        }

        // The uop sequencer starts a macro-op once it reaches the front of the
        // buffer behind the staged instruction, and offers its first uop the
        // cycle after. A macro that arrives in an empty buffer, or behind an
        // instruction that issues at once, waits that extra cycle.
        bool seq_starting = false;
        if (seq->starting()) {
          uint64_t ready = std::max(staged_since_.at(wid), trace->ibuf_time) + 2;
          seq_starting = (now < ready);
        }

        this->check_issue_timeout(now, iw, wid, trace, uop_trace);

        if (this->scrb_blocked(wid, uop_trace)) {
          if (!uop_trace->log_once(true)) {
          #ifndef NDEBUG
            // the dependents are listed only for the trace
            auto uses = scoreboard_->get_uses(uop_trace);
            DTH(4, "*** scoreboard-stall: dependents={");
            for (uint32_t j = 0, n = uses.size(); j < n; ++j) {
              auto& use = uses.at(j);
              __unused (use);
              if (j) DTN(4, ", ");
              DTN(4, use.reg_type << use.reg_id << " (#" << use.uuid << ")");
            }
            DTN(4, "}, " << *uop_trace << std::endl);
          #endif
          }
          // Count once per cycle per issue slice regardless of how many
          // warps are blocked.
          any_scrb_blocked = true;
        } else {
          uop_trace->log_once(false);
          if (seq_starting) {
            continue;
          }
          // While a warp holds the issue lock, no other warp of the slot issues,
          // whatever its unit.
          auto lock_owner = issue_lock_owner_.at(iw);
          if (lock_owner != kLockOpen && lock_owner != w) {
            continue;
          }
          if (yielded == w) {
            continue;
          }
          auto fu = (int)uop_trace->fu_type;
          // FU dispatch queue near full: the warp does not request. Credits also
          // count ops still in operand collection. The flag reaches issue through
          // two registers, so a warp sees the count as of two cycles back.
          if (fu_full_seen_.at(iw).test(fu)) {
            continue;
          }
        #ifdef VX_CFG_EXT_RTU_ENABLE
          // A TRACE macro must hold a ray-pool slot before its head uop enters
          // the SFU, or it stalls at the head of that unit's queue and starves
          // the WAIT that would release one. Claiming it here, rather than
          // testing for a free one, also keeps another core from taking the
          // last slot between issue and execute.
          if (!this->rtu_trace_reserve(uop_trace)) {
            continue; // ray pool full
          }
        #endif
          ready_set.set(w); // mark instruction as ready
          if (uop_trace->instr_ptr->get_issue_yield()) {
            yield_set.set(w);
          }
        }
      }

      // The arbiter advances only on an issue that fires.
      if (ready_set.any() && !operands_.at(iw)->Input.full()) {
        // select one instruction from ready set
        auto w = ibuffer_arbs_.at(iw).select(ready_set);
        uint32_t wid = w * VX_CFG_ISSUE_WIDTH + iw;
        auto& ibuffer = ibuffers_.at(wid);
        auto trace = ibuffer->peek();

        // Retrieve the (already-generated) micro-op from the sequencer
        auto& seq = sequencers_.at(wid);
        auto uop_trace = seq->get(trace);

        // to operand stage
        if (operands_.at(iw)->Input.try_send(uop_trace)) {
          ibuffer_arbs_.at(iw).commit(w);
          // capture register operands at issue (Operands owns regfile)
          operands_.at(iw)->fetch_operands(uop_trace);
          // A macro leaves the pending count once its last micro-op issues, so
          // its micro-ops are pending until they commit, as in RTL.
          if (uop_trace != trace) {
            this->pending_add(uop_trace->wid);
          }
          // spend a dispatch credit for the target FU
          ++fu_credits_.at(iw).at((int)uop_trace->fu_type);
          // An issue candidate is staged from the cycle after both the warp's
          // previous issue and its own arrival in the buffer.
          staged_since_.at(wid) = std::max(last_issue_.at(wid), trace->ibuf_time) + 1;
          last_issue_.at(wid) = now;
          DT(3, simobject_->name() << "-pipeline issue: " << *uop_trace);
          if (uop_trace->wb) {
            // update scoreboard
            scoreboard_->reserve(uop_trace);
          }
          if (uop_trace->instr_ptr->fcsr_writes()) {
            scoreboard_->reserve_fcsr(uop_trace);
          }
          // Issue lock: a sequence's first uop (lock without unlock) takes it
          // for its warp, the last (unlock) reopens the slot from the next cycle.
          {
            bool fl = uop_trace->instr_ptr->get_fu_lock();
            bool ful = uop_trace->instr_ptr->get_fu_unlock();
            if (fl && !ful) {
              issue_lock_owner_.at(iw) = w;
            } else if (ful) {
              issue_lock_owner_.at(iw) = kLockOpen;
            }
          }
          // A yielding uop issued while another warp also requested with one
          // masks its own warp for the next cycle, so the greedy grant moves on.
          if (uop_trace->instr_ptr->get_issue_yield() && (ready_set & yield_set).count() >= 2) {
            issue_yield_.at(iw) = w;
          }
          // Advance sequencer; pop ibuffer only when all micro-ops issued
          if (seq->advance()) {
            if (trace->instr_ptr->is_macro_op()) {
              // A macro that stalled fetch at decode releases its warp once
              // its last micro-op issues.
              if (trace->instr_ptr->is_wstall()) {
                scheduler_->resume(trace->wid);
              }
              // Macro trace never reaches commit (only micro-ops do),
              // so remove it from pending tracking and deallocate here.
              this->pending_remove(trace->wid);
              trace->~instr_trace_t();
              trace_pool_.deallocate(trace, 1);
            }
            ibuffer->pop();
            if (ibuffer->size() == 0) {
              ibuf_held_.reset(wid);
            }
            if (!head_staged_.at(wid)) {
              this->ibuf_release(wid);
            }
            head_staged_.at(wid) = false;
            // The staging register takes the next instruction in the cycle it
            // hands this one off, so its buffer slot frees now.
            if (!ibuffer->empty()) {
              auto next = ibuffer->peek();
              if (!next->instr_ptr->is_macro_op()
               && now > next->ibuf_time) {
                head_staged_.at(wid) = true;
                this->ibuf_release(wid);
              }
            }
          } else if (seq->last() && !head_staged_.at(wid)) {
            // The macro's last micro-op takes the staging register as this one
            // leaves it, and the macro leaves the buffer with it.
            head_staged_.at(wid) = true;
            this->ibuf_release(wid);
          }
        }
      }

      // track scoreboard stalls
      if (any_scrb_blocked) {
        ++perf_stats_.scrb_stalls;
      }
    }
  }

  void ibuf_release(uint32_t wid) {
    assert(ibuf_inflight_.at(wid) > 0);
    --ibuf_inflight_.at(wid);
    assert(ibuf_queued_.at(wid) > 0);
    --ibuf_queued_.at(wid);
    this->ibuf_count_changed(wid);
  }

  // Exact equality ends the depth-one bypass after a warp overfetches.
  void ibuf_count_changed(uint32_t wid) {
    __assert(VX_CFG_FETCH_INFLIGHT == 1 || ibuf_inflight_[wid] <= VX_CFG_IBUF_SIZE,
             "fetch ahead picked past the instruction buffer");
    ibuf_full_.set(wid, ibuf_inflight_[wid] == VX_CFG_IBUF_SIZE);
    ibuf_queue_full_.set(wid, ibuf_queued_[wid] >= VX_CFG_IBUF_SIZE);
  }

  void update_queue_full() {
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      fu_full_seen_.at(iw) = fu_full_.at(iw);
      for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
        bool full = (fu_credits_.at(iw).at(fu) >= VX_CFG_DISPATCH_QUEUE_SIZE - 1);
        if (full) {
          fu_full_.at(iw).set(fu);
        } else {
          fu_full_.at(iw).reset(fu);
        }
      }
    }
  }

  void return_dispatch_credits() {
    for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
      auto& release = dispatchers_.at(fu)->ReleaseOut;
      while (!release.empty()) {
        uint32_t iw = release.peek()->wid % VX_CFG_ISSUE_WIDTH;
        assert(fu_credits_.at(iw).at(fu) > 0);
        --fu_credits_.at(iw).at(fu);
        release.pop();
      }
    }
  }

  void commit() {
    // A resolved branch or warp-control op releases its warp, or retires it
    // when it disabled all its threads, a cycle after it resolves; the
    // scheduler observes the change the cycle after that.
    auto& branch_ctl = simobject_->branch_ctl_in;
    while (!branch_ctl.empty()) {
      scheduler_->resume(branch_ctl.peek());
      branch_ctl.pop();
    }
    auto& warp_ctl = simobject_->warp_ctl_in;
    while (!warp_ctl.empty()) {
      auto& ctl = warp_ctl.peek();
      if (ctl.exit) {
        scheduler_->setTmask(ctl.wid, ThreadMask(VX_CFG_NUM_THREADS));
      } else {
        scheduler_->resume(ctl.wid);
      }
      warp_ctl.pop();
    }
    auto& bar_unlock = simobject_->bar_unlock_in;
    while (!bar_unlock.empty()) {
      scheduler_->resume(bar_unlock.peek());
      bar_unlock.pop();
    }

    // Fan-in: route per-block FU outputs to per-iw commit queues by trace->wid.
    // Each FU has NUM_*_BLOCKS outputs; the original iw was lost during
    // dispatcher aggregation, so we recover it from the warp id and try_send
    // into the matching commit queue.
    for (uint32_t fu = 0; fu < (uint32_t)FUType::Count; ++fu) {
      if (fu == (uint32_t)FUType::FPU && !FpuUnit::kGather)
        continue; // commit reads the FPU response ports directly
      auto& func_unit = func_units_.at(fu);
      uint32_t nb = func_unit->num_blocks();
      for (uint32_t b = 0; b < nb; ++b) {
        auto& fu_out = func_unit->output(b);
        if (fu_out.empty())
          continue;
        auto trace = fu_out.peek();
        uint32_t iw = trace->wid % VX_CFG_ISSUE_WIDTH;
        auto& arb_in = *commit_queues_.at(iw).at(fu);
        // The unit's output registers ahead of the commit arbiter that its
        // output channel does not already carry: the LSU's response buffer and
        // result arbiter, the SFU's result buffer and response switch, the
        // FPU's output register and the TCU's result landing queue are in
        // their channels, leaving each its lane gather.
        uint32_t commit_delay = commit_single_hop(fu) ? 1 : 2;
        if (arb_in.try_send(trace, commit_delay)) {
          // Release the warp as soon as its stalling instruction's result leaves
          // the functional unit — the branch target / fence / warp-control is
          // resolved at that point. The release lands in stalled_warps so the
          // scheduler observes it next cycle.
          if (trace->eop && trace->resume_warp) {
            scheduler_->resume(trace->wid);
          }
          fu_out.pop();
        }
      }
    }

    // process completed instructions: one beat per issue slice per cycle,
    // granted fixed-priority: the FPU first, then ALU, LSU, SFU and TCU.
    // Without a lane gather an FPU result keeps its tag until granted, and the
    // FPU has few, so waiting behind ALU or LSU results would stall its issue.
    // The same tags bound the beats the FPU can take.
    //
    // A narrow unit's lane gather holds the passes of one SIMD slice of an
    // instruction and commits them in the beat of the pass that completes the
    // slice. Taking a pass into the hold costs no beat; a hold that the next
    // result does not continue leaves in a beat of its own, ahead of that
    // result.
    static constexpr FUType kCommitPrio[] = {
      FUType::FPU, FUType::ALU, FUType::LSU, FUType::SFU,
    #ifdef VX_CFG_EXT_TCU_ENABLE
      FUType::TCU,
    #endif
    };
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      auto& holds = gather_holds_.at(iw);
      SimChannel<instr_trace_t*>* granted = nullptr;
      FUType granted_fu = FUType::ALU;
      for (auto fu : kCommitPrio) {
        // Without a lane-gather stage the FPU response port, one block per
        // issue slot, feeds the arbiter: a result holds its FPU tag until
        // granted.
        auto& queue = (fu == FUType::FPU && !FpuUnit::kGather)
                    ? func_units_.at((uint32_t)fu)->output(iw)
                    : *commit_queues_.at(iw).at((uint32_t)fu);
        if (queue.empty()) {
          continue;
        }
        uint32_t lanes = gather_merge_lanes((uint32_t)fu);
        if (lanes != 0) {
          auto trace = queue.peek();
          auto& hold = holds.at((uint32_t)fu);
          if (!this->gather_completes(trace, lanes)
           && (!hold.valid || hold.continues(trace, lanes))) {
            // the pass joins the hold this cycle, without a beat
            hold.take(trace, lanes);
            this->retire(trace, iw);
            queue.pop();
            continue;
          }
        }
        if (!granted) {
          granted = &queue;
          granted_fu = fu;
        }
      }
      if (!granted)
        continue;
      auto trace = granted->peek();
      if (granted_fu == FUType::FPU && !FpuUnit::kGather
       && trace->eop && trace->resume_warp) {
        scheduler_->resume(trace->wid);
      }

      uint32_t lanes = gather_merge_lanes((uint32_t)granted_fu);
      if (lanes != 0) {
        auto& hold = holds.at((uint32_t)granted_fu);
        if (hold.valid && !hold.continues(trace, lanes)) {
          // the beat drains the hold alone; a pass that does not complete its
          // instruction takes the hold's place in the same cycle, one that
          // does waits for the next beat
          hold.valid = false;
          if (!this->gather_completes(trace, lanes)) {
            hold.take(trace, lanes);
            this->retire(trace, iw);
            granted->pop();
          }
          continue;
        }
        hold.valid = false;
      }

      this->retire(trace, iw);
      granted->pop();
    }

  #ifdef VX_CFG_EXT_TCU_ENABLE
    // A TCU block's results enter its slot's lane-gather buffer, whose ready
    // is a register: a buffer that ends the cycle full refuses the next
    // cycle's result, and the block holds admission while one completes.
    uint64_t next = SimPlatform::instance().cycles() + 1;
    for (uint32_t b = 0; b < VX_CFG_NUM_TCU_BLOCKS; ++b) {
      if (commit_queues_.at(b).at((uint32_t)FUType::TCU)->full()) {
        tcu_stall_out_.at(b)->send(next, 1);
      }
    }
  #endif
  }

  // A pass completes its commit beat when it is the last pass of its SIMD
  // slice, or when nothing of the instruction can follow it.
  bool gather_completes(const instr_trace_t* trace, uint32_t lanes) const {
    uint32_t passes = VX_CFG_SIMD_WIDTH / lanes;
    if ((uint32_t)trace->pid % passes == passes - 1) {
      return true;
    }
    // responses complete out of order: the last one to arrive ends the
    // instruction
    if (trace->rsp_tracked) {
      return trace->rsp_last;
    }
    return trace->eop;
  }

  void retire(instr_trace_t* trace, uint32_t iw) {
    // advance to commit stage
    DT(3, simobject_->name() << "-pipeline commit: " << *trace);
    assert(trace->cid == simobject_->id());

    // Per-pid writeback: dispatcher splits multi-pid traces into copies
    // each carrying its own lane-subset tmask + dst_data; every pid pass
    // writes back independently. Scoreboard release counts COMMITS via
    // num_pkts (set by dispatcher at sop) rather than gating on eop —
    // cache responses can complete out of order, so a SIMD-split eop
    // packet may reach commit before its peers; releasing on eop would
    // expose stale lanes to the next dependent instruction's
    // fetch_operands. Perf accounting still gates on eop.
    if (trace->wb) {
      operands_.at(iw)->writeback(trace);
      if (scoreboard_->commit_packet(trace)) {
        scoreboard_->release(trace);
      }
    }
    if (trace->eop && trace->instr_ptr->fcsr_writes()) {
      scoreboard_->release_fcsr(trace);
    }

    if (trace->eop) {

      // instruction mix profiling
      switch (trace->fu_type) {
      case FUType::ALU: ++perf_stats_.alu_instrs; break;
      case FUType::FPU: ++perf_stats_.fpu_instrs; break;
      case FUType::LSU: ++perf_stats_.lsu_instrs; break;
      case FUType::SFU: ++perf_stats_.sfu_instrs; break;
    #ifdef VX_CFG_EXT_TCU_ENABLE
      case FUType::TCU: ++perf_stats_.tcu_instrs; break;
    #endif
      default: assert(false);
      }
      // track committed instructions
      perf_stats_.instrs += 1;

      // instruction completed
      this->pending_remove(trace->wid);
    }

    trace->~instr_trace_t();
    trace_pool_.deallocate(trace, 1);
  }

  // ----- Public-facing API (called from outer Core trampolines) -----

  bool running() const {
    // committed stores wait in the LSU request queue until sent to memory
    if (scheduler_->running() || pending_total_ != 0 || !this->lsu_drained()) {
      return true;
    }
    // fetches of a squashed stream are still on their way back
    if (VX_CFG_FETCH_INFLIGHT > 1 && scheduler_->fetch_pending()) {
      return true;
    }
  #ifdef VX_CFG_VM_ENABLE
    // Parked translations hold no in-flight channel packet, so they are
    // invisible to SimPlatform::idle(); completion must wait for them.
    if (!dcache_mmu_->empty() || !icache_mmu_->empty()) {
      return true;
    }
  #endif
    return false;
  }

  bool has_pending_instrs(uint32_t wid) const {
    return pending_instrs_.at(wid) > 1; // the asking instruction counts itself
  }

  void pending_add(uint32_t wid) {
    ++pending_instrs_.at(wid);
    ++pending_total_;
  }

  void pending_remove(uint32_t wid) {
    assert(pending_instrs_.at(wid) > 0 && pending_total_ > 0);
    --pending_instrs_.at(wid);
    --pending_total_;
  }

  bool lsu_drained() const {
    return static_cast<const LsuUnit*>(func_units_.at((int)FUType::LSU).get())->drained();
  }

  bool lsu_sent() const {
    return static_cast<const LsuUnit*>(func_units_.at((int)FUType::LSU).get())->sent();
  }

  // No request is in flight past the LSU blocks' request queues: the local
  // memory switches, the data-cache coalescers or lane adapters, the path on
  // to the dcache and the local memory hold none. The hardware reads these
  // stages in the same cycle; every one is in the socket's execution domain,
  // whose tick order is fixed, so the read is deterministic at any thread count.
  bool lsu_stores_landed() const {
    for (uint32_t b = 0; b < VX_CFG_NUM_LSU_BLOCKS; ++b) {
      if (lmem_switch_.at(b)->ReqIn.size() != 0) {
        return false;
      }
      if (lsu_dcache_adapters_.at(b)) {
        auto& adapter = *lsu_dcache_adapters_.at(b);
        if (adapter.ReqIn.size() != 0) {
          return false;
        }
        for (auto& out : adapter.ReqOut) {
          if (out.size() != 0) {
            return false;
          }
        }
      } else if (!mem_coalescers_.at(b)->req_idle()) {
        return false;
      }
    }
    // Past them: a flush port's arbiter buffer, the dcache MMU and the link
    // to the dcache.
    for (auto& buf : dcache_flush_bufs_) {
      if (buf->Out.size() != 0) {
        return false;
      }
    }
  #ifdef VX_CFG_VM_ENABLE
    if (!dcache_mmu_->empty()) {
      return false;
    }
  #endif
    for (auto& out : simobject_->dcache_req_out) {
      if (out.size() != 0) {
        return false;
      }
    }
    // A link hands its head to the cache's input, where it waits for the
    // cache as it would in the link's output register.
    for (auto* out : dcache_link_outs_) {
      if (out->size() != 0) {
        return false;
      }
    }
    return !local_mem_->holds_store();
  }

  void watch_dcache_link(uint32_t port, const SimChannelBase* link_out) {
    __assert(port < VX_CFG_DCACHE_NUM_REQS, "invalid dcache port");
    dcache_link_outs_.push_back(link_out);
  }

  int dcr_read(uint32_t addr, uint32_t tag, uint32_t* value) {
    // tag arrives as (mpm_class << 6) | mpm_tag_idx after socket strips core_id
    switch (addr) {
    case VX_DCR_BASE_CACHE_FLUSH:
      *value = 0;
      break;
    case VX_DCR_BASE_MPM_VALUE: {
      uint32_t mpm_class   = tag >> 6;
      uint32_t mpm_tag_idx = tag & 0x3f;
      bool     is_hi       = (mpm_tag_idx >> 5) & 1;
      uint32_t idx         = mpm_tag_idx & 0x1f;
      uint32_t csr_addr    = is_hi ? (VX_CSR_MPM_BASE_H + idx) : (VX_CSR_MPM_BASE + idx);
      // the counters a quiescent core has skipped are counted first
      if (asleep_) {
        this->account_sleep(SimPlatform::instance().cycles());
      }
      auto saved_class = mpm_class_;
      mpm_class_ = mpm_class;
      *value = static_cast<uint32_t>(this->csr_unit().get_csr(csr_addr, 0, 0));
      mpm_class_ = saved_class;
      break;
    }
    default:
      break;
    }
    return 0;
  }

  PerfStats& perf_stats() {
    if (asleep_) {
      this->account_sleep(SimPlatform::instance().cycles());
    }
    perf_stats_.opds_stalls = 0;
    for (uint32_t iw = 0; iw < VX_CFG_ISSUE_WIDTH; ++iw) {
      perf_stats_.opds_stalls += operands_.at(iw)->total_stalls();
    }
    return perf_stats_;
  }

  PerfStats& perf_counters() {
    return perf_stats_;
  }

  int get_exitcode() const {
    return operands_.at(0)->get_exit_code();
  }

  // DTM debug-only accessors. The simx debug stack reads and writes warp PC
  // and integer registers directly; these live in Scheduler and Operands
  // respectively. Single-hart debug uses lane=0.
  Word dtm_get_pc(uint32_t wid) const {
    return scheduler_->warp(wid).PC;
  }
  void dtm_set_pc(uint32_t wid, Word pc) {
    scheduler_->warp(wid).PC = pc;
    if (VX_CFG_FETCH_INFLIGHT > 1) {
      // the warp's fetches in flight are of the old PC
      fetch_dec_.at(wid).strm ^= 1;
      fetch_dec_.at(wid).open = false;
      scheduler_->fetch_redirect(wid);
    }
  }
  Word dtm_get_ireg(uint32_t wid, uint32_t reg) {
    return operands_.at(wid % VX_CFG_ISSUE_WIDTH)->dtm_ireg(wid, reg);
  }
  void dtm_set_ireg(uint32_t wid, uint32_t reg, Word val) {
    operands_.at(wid % VX_CFG_ISSUE_WIDTH)->dtm_ireg(wid, reg) = val;
  }

  Scheduler*    scheduler() { return scheduler_.get(); }
  Scoreboard*   scoreboard() { return scoreboard_.get(); }
  // CSR is a sub-unit of SFU; reach it through SfuUnit.
  CsrUnit&      csr_unit()  { return static_cast<SfuUnit*>(func_units_.at((int)FUType::SFU).get())->csr_unit(); }
  uint32_t      mpm_class() const { return mpm_class_; }
#ifdef VX_CFG_EXT_TCU_ENABLE
  std::shared_ptr<TcuUnit>& tcu_unit() { return tcu_unit_; }
#endif
  std::shared_ptr<SfuUnit>  sfu_unit() {
    return std::static_pointer_cast<SfuUnit>(func_units_.at((int)FUType::SFU));
  }

#ifdef VX_CFG_EXT_RTU_ENABLE
  // Only a TRACE macro head claims a ray-pool slot; every other uop holds no
  // new resource. The claim is per-warp and idempotent, so a warp that passes
  // the ready-scan but loses arbitration keeps its slot for the next cycle.
  bool rtu_trace_reserve(const instr_trace_t* uop_trace) {
    // TRACE is a WINDOW op here, not an RTU op: the P2 inversion made the RTU the
    // bus master and the window a passive slot file, so the macro lives in
    // GfxwType/IntrGfxwArgs. Upstream still had a separate RtuType.
    auto gfxw_p = std::get_if<GfxwType>(&uop_trace->op_type);
    if (gfxw_p == nullptr || *gfxw_p != GfxwType::TRACE) {
      return true;
    }
    auto args = std::get<IntrGfxwArgs>(uop_trace->instr_ptr->get_args());
    if (args.uop != 0) {
      return true;
    }
    return this->sfu_unit()->rtu_trace2_reserve_slot(uop_trace->wid);
  }
#endif

  const std::shared_ptr<LocalMem>& local_mem() const { return local_mem_; }
  const std::shared_ptr<MemCoalescer>& mem_coalescer(uint32_t idx) const { return mem_coalescers_.at(idx); }
  const std::shared_ptr<LocalMemSwitch>& lmem_switch(uint32_t idx) const { return lmem_switch_.at(idx); }

#ifdef VX_CFG_VM_ENABLE
  // Device satp (from the DCR, via Cluster::set_mmu_satp) fans out to both
  // per-core MMUs (dcache + icache). They each maintain an independent TLB
  // but share the same PT base.
  void set_satp(uint64_t satp) {
    dcache_mmu_->set_satp(satp);
    icache_mmu_->set_satp(satp);
  }

  // Combined icache + dcache MMU counters (one bank per core).
  Core::MmuPerfStats mmu_perf_stats() const {
    Core::MmuPerfStats stats;
    stats.tlb_reads     = dcache_mmu_->tlb_reads()     + icache_mmu_->tlb_reads();
    stats.tlb_hits      = dcache_mmu_->tlb_hits()      + icache_mmu_->tlb_hits();
    stats.tlb_misses    = dcache_mmu_->tlb_misses()    + icache_mmu_->tlb_misses();
    stats.tlb_evictions = dcache_mmu_->tlb_evictions() + icache_mmu_->tlb_evictions();
    return stats;
  }

  SimChannel<TlbReq>& tlb_miss_out(uint32_t which) {
    return (which == 0) ? dcache_mmu_->TlbMissOut : icache_mmu_->TlbMissOut;
  }

  SimChannel<TlbRsp>& tlb_fill_in(uint32_t which) {
    return (which == 0) ? dcache_mmu_->TlbFillIn : icache_mmu_->TlbFillIn;
  }

#endif

  PoolAllocator<instr_trace_t, 64>& trace_pool() { return trace_pool_; }

private:
  Core* simobject_;

#ifdef VX_CFG_EXT_TCU_ENABLE
  TcuUnit::Ptr tcu_unit_;
#endif

  PoolAllocator<Instr, 64> instr_pool_;
  Decoder::Ptr decoder_;
  std::vector<Sequencer::Ptr> sequencers_;
  uint32_t    mpm_class_;
  Scheduler::Ptr scheduler_;

  std::vector<TFifo<instr_trace_t*>::Ptr> ibuffers_;
  Scoreboard::Ptr scoreboard_;
  std::vector<Operands::Ptr> operands_;
  std::vector<Dispatcher::Ptr> dispatchers_;
  std::vector<std::shared_ptr<FuncUnitBase>> func_units_;
  LocalMem::Ptr local_mem_;
  std::vector<LocalMemSwitch::Ptr> lmem_switch_;
  std::vector<MemCoalescer::Ptr> mem_coalescers_;
  std::vector<LsuMemAdapter::Ptr> lsu_dcache_adapters_;
  std::vector<TxBuffer<MemReq>::Ptr> dcache_flush_bufs_;
  std::vector<const SimChannelBase*> dcache_link_outs_;
  TxBuffer<MemReq>::Ptr icache_flush_buf_;

  std::vector<std::unique_ptr<TFifo<instr_trace_t*>>> fetch_latch_;
  std::vector<std::unique_ptr<TFifo<instr_trace_t*>>> decode_latch_;

  std::vector<HashTable<instr_trace_t*>> pending_icache_;

  // Fetch ahead, the decode side: per warp, the current stream,
  // the run whose first fetch was decoded last, whether no squash followed
  // it, and the position of the next instruction in it; per frontend slice, the
  // response dropped last cycle.
  struct FetchDec {
    uint32_t strm = 0;
    uint32_t run  = 0;
    bool     open = false;
    uint32_t head = 0;
  };
  struct FetchDrop {
    bool     valid = false;
    uint32_t wid = 0;
    bool     squash = false;
  };
  std::vector<FetchDec>  fetch_dec_  = std::vector<FetchDec>(VX_CFG_NUM_WARPS);
  std::vector<FetchDrop> fetch_drop_ = std::vector<FetchDrop>(VX_CFG_FETCH_WIDTH);
  // Scheduled, not yet committed: a macro until its last micro-op issues, then
  // each micro-op until it commits.
  std::vector<uint32_t> pending_instrs_ = std::vector<uint32_t>(VX_CFG_NUM_WARPS, 0);
  uint32_t pending_total_ = 0;

  uint64_t pending_ifetches_;

  mutable PerfStats perf_stats_;

  // [iw][fu] commit staging: FU outputs land here one registered stage
  // before retirement; commit() grants among them fixed-priority.
  std::vector<std::vector<std::unique_ptr<SimChannel<instr_trace_t*>>>> commit_queues_;

#ifdef VX_CFG_EXT_TCU_ENABLE
  // [block] the TCU lane-gather buffer's ready, to the block's admission
  std::vector<std::unique_ptr<SimChannel<uint64_t>>> tcu_stall_out_;
#endif

  // A lane gather's hold: the SIMD slice of an instruction whose passes wait
  // for the beat of the pass that completes it. Micro-ops of one instruction
  // share its PC and differ in destination.
  struct GatherHold {
    bool     valid   = false;
    uint32_t wid     = 0;
    uint32_t sid     = 0;
    uint64_t PC      = 0;
    uint32_t dst     = 0;
    uint32_t bytesel = 0;
    static uint32_t slice_of(const instr_trace_t* trace, uint32_t lanes) {
      return (uint32_t)trace->pid / (VX_CFG_SIMD_WIDTH / lanes);
    }
    bool continues(const instr_trace_t* trace, uint32_t lanes) const {
      return valid && trace->wid == wid && slice_of(trace, lanes) == sid
          && trace->PC == PC && trace->dst_reg.id() == dst
          && trace->dst_bytesel == bytesel;
    }
    void take(const instr_trace_t* trace, uint32_t lanes) {
      valid   = true;
      wid     = trace->wid;
      sid     = slice_of(trace, lanes);
      PC      = trace->PC;
      dst     = trace->dst_reg.id();
      bytesel = trace->dst_bytesel;
    }
  };
  std::vector<std::vector<GatherHold>> gather_holds_; // [iw][fu]

  std::vector<Arbiter> ibuffer_arbs_;

  static constexpr uint32_t kLockOpen = ~0u;
  static constexpr uint32_t kNoYield = ~0u;
  std::vector<uint32_t> issue_lock_owner_; // [iw] slot-local warp holding the issue lock
  std::vector<uint32_t> issue_yield_;      // [iw] slot-local warp that yields the next cycle's issue
  std::vector<std::vector<uint32_t>> fu_credits_; // [iw][fu] in-flight dispatch credits
  std::vector<BitVector<>> fu_full_;      // [iw] credits at or past the near-full mark
  std::vector<BitVector<>> fu_full_seen_; // [iw] that flag one cycle later, as issue sees it

  std::vector<uint32_t> ibuf_inflight_;
  std::vector<uint32_t> ibuf_queued_;      // [wid] instructions past the icache, not yet staged
  WarpMask ibuf_full_;          // [wid] full flag from ibuf_inflight_
  WarpMask ibuf_full_r_;        // ... registered
  WarpMask ibuf_queue_full_;    // [wid] full flag from ibuf_queued_
  WarpMask ibuf_queue_full_r_;  // ... registered
  WarpMask ibuf_held_;          // [wid] the buffer holds an instruction or one on its way in
  std::array<WarpMask, VX_CFG_FETCH_WIDTH> fetch_warps_; // [slice] the warps it fetches for
  std::vector<uint64_t> last_issue_;   // [wid] cycle of the warp's last issue
  std::vector<uint64_t> staged_since_; // [wid] cycle the last issued candidate was staged
  std::vector<bool>     head_staged_;  // [wid] the buffer head has left for staging
  // [wid] the micro-op the scoreboard last blocked, and the warp's release
  // generation then
  struct ScrbWait {
    const instr_trace_t* uop = nullptr;
    uint32_t gen = 0;
  };
  std::vector<ScrbWait> scrb_waits_ = std::vector<ScrbWait>(VX_CFG_NUM_WARPS);

  PoolAllocator<instr_trace_t, 64> trace_pool_;

  // asleep since the end of the tick of cycle asleep_after_ (see quiescent())
  bool     asleep_ = false;
  uint64_t asleep_after_ = 0;
  uint64_t asleep_active_ = 0;
  uint64_t asleep_stalled_ = 0;

#ifdef VX_CFG_EXT_C_ENABLE
  // RVC fetch SimObject per frontend slice (per-warp halfword buffer + refetch
  // queue + pick/commit_request helpers). See decompressor.h.
  std::vector<Decompressor::Ptr> decompressors_;
#endif
#ifdef VX_CFG_VM_ENABLE
  Mmu::Ptr dcache_mmu_;
  Mmu::Ptr icache_mmu_;
#endif
};

///////////////////////////////////////////////////////////////////////////////

Core::Core(const SimContext& ctx,
           const char* name,
           uint32_t core_id,
           Socket* socket
           )
  : SimObject(ctx, name)
  , icache_req_out(ICACHE_NUM_REQS, this)
  , icache_rsp_in(ICACHE_NUM_REQS, this)
  , dcache_req_out(VX_CFG_DCACHE_NUM_REQS, this)
  , dcache_rsp_in(VX_CFG_DCACHE_NUM_REQS, this)
  , branch_ctl_in(this, VX_CFG_NUM_WARPS)
  , warp_ctl_in(this, VX_CFG_NUM_WARPS)
  , bar_unlock_in(this, VX_CFG_NUM_WARPS)
  , gbar_arrive_out(this)
  , gbar_resume_in(this)
#ifdef VX_CFG_EXT_RASTER_ENABLE
  , fwd_arm_in(this)
  , fwd_done_out(this)
#endif
  , core_id_(core_id)
  , socket_(socket)
  , impl_(new Impl(ctx, this))
{
  gbar_resume_in.bind(this, &Core::on_gbar_resume);
#ifdef VX_CFG_EXT_RASTER_ENABLE
  fwd_arm_in.bind(this, &Core::on_fwd_arm);
#endif
}

Core::~Core() {
  delete impl_;
}

void Core::on_reset() {
  impl_->reset();
}

void Core::on_tick() {
  impl_->tick();
}

ProcessorImpl* Core::processor() const {
  return socket_->cluster()->processor();
}

bool Core::running() const {
  return impl_->running();
}

bool Core::has_pending_instrs(uint32_t wid) const {
  return impl_->has_pending_instrs(wid);
}

bool Core::lsu_drained() const {
  return impl_->lsu_drained();
}

bool Core::lsu_sent() const {
  return impl_->lsu_sent();
}

bool Core::lsu_stores_landed() const {
  return impl_->lsu_stores_landed();
}

void Core::resume(uint32_t wid) {
  impl_->wake();
  impl_->scheduler()->resume(wid);
}

void Core::barrier_arrive(uint32_t bar_id, uint32_t count, uint32_t wid, bool is_sync_bar) {
  impl_->wake();
  impl_->scheduler()->barrier_unit().arrive(bar_id, count, wid, is_sync_bar);
}

void Core::barrier_wait(uint32_t bar_id, uint32_t phase, uint32_t wid) {
  impl_->wake();
  impl_->scheduler()->barrier_unit().wait(bar_id, phase, wid);
}

void Core::global_barrier_resume(uint32_t bar_id) {
  impl_->wake();
  impl_->scheduler()->barrier_unit().global_resume(bar_id);
}

void Core::on_gbar_resume(const GbarResume& msg) {
  this->global_barrier_resume(msg.bar_id);
}

#ifdef VX_CFG_EXT_RASTER_ENABLE
void Core::on_fwd_arm(const FwdArm& msg) {
  impl_->wake();
  impl_->scheduler()->fwd_arm(msg.frag_entry, msg.frag_param);
}
#endif

void Core::barrier_event_attach(uint32_t bar_id, uint32_t count) {
  impl_->wake();
  impl_->scheduler()->barrier_unit().event_attach(bar_id, count);
}

void Core::barrier_event_release(uint32_t bar_id) {
  impl_->wake();
  impl_->scheduler()->barrier_unit().event_release(bar_id);
}

bool Core::wspawn(uint32_t num_warps, Word nextPC) {
  impl_->wake();
  return impl_->scheduler()->wspawn(num_warps, nextPC);
}

bool Core::setTmask(uint32_t wid, const ThreadMask& tmask) {
  impl_->wake();
  return impl_->scheduler()->setTmask(wid, tmask);
}

int Core::dcr_write(uint32_t addr, uint32_t value) {
  __unused(addr);
  __unused(value);
  // KMU DCRs are handled at ProcessorImpl level and never reach here.
  return 0;
}

void Core::watch_dcache_link(uint32_t port, const SimChannelBase* link_out) {
  impl_->watch_dcache_link(port, link_out);
}

int Core::dcr_read(uint32_t addr, uint32_t tag, uint32_t* value) {
  return impl_->dcr_read(addr, tag, value);
}

Core::PerfStats& Core::perf_stats() {
  return impl_->perf_stats();
}

const Core::PerfStats& Core::perf_stats() const {
  return impl_->perf_stats();
}

Core::PerfStats& Core::perf_counters() {
  return impl_->perf_counters();
}

int Core::get_exitcode() const {
  return impl_->get_exitcode();
}

Scheduler& Core::scheduler() { return *impl_->scheduler(); }
Scoreboard& Core::scoreboard() { return *impl_->scoreboard(); }
CsrUnit& Core::csr_unit() { return impl_->csr_unit(); }
uint32_t Core::mpm_class() const { return impl_->mpm_class(); }

Word Core::dtm_get_pc(uint32_t wid) const         { return impl_->dtm_get_pc(wid); }
void Core::dtm_set_pc(uint32_t wid, Word pc)      { impl_->wake(); impl_->dtm_set_pc(wid, pc); }
Word Core::dtm_get_ireg(uint32_t wid, uint32_t reg)            { return impl_->dtm_get_ireg(wid, reg); }
void Core::dtm_set_ireg(uint32_t wid, uint32_t reg, Word val)  { impl_->wake(); impl_->dtm_set_ireg(wid, reg, val); }

#ifdef VX_CFG_EXT_TCU_ENABLE
std::shared_ptr<TcuUnit>& Core::tcu_unit() {
  return impl_->tcu_unit();
}
#endif

std::shared_ptr<SfuUnit> Core::sfu_unit() {
  return impl_->sfu_unit();
}

const std::shared_ptr<LocalMem>& Core::local_mem() const {
  return impl_->local_mem();
}

const std::shared_ptr<MemCoalescer>& Core::mem_coalescer(uint32_t idx) const {
  return impl_->mem_coalescer(idx);
}

const std::shared_ptr<LocalMemSwitch>& Core::lmem_switch(uint32_t idx) const {
  return impl_->lmem_switch(idx);
}

#ifdef VX_CFG_VM_ENABLE
Core::MmuPerfStats Core::mmu_perf_stats() const {
  return impl_->mmu_perf_stats();
}

SimChannel<TlbReq>& Core::tlb_miss_out(uint32_t which) {
  return impl_->tlb_miss_out(which);
}

SimChannel<TlbRsp>& Core::tlb_fill_in(uint32_t which) {
  return impl_->tlb_fill_in(which);
}

#endif

PoolAllocator<instr_trace_t, 64>& Core::trace_pool() {
  return impl_->trace_pool();
}

#ifdef VX_CFG_VM_ENABLE
void Core::set_satp(uint64_t satp) { impl_->set_satp(satp); }
#endif
