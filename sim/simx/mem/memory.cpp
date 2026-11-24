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

#include <VX_types.h>
#include "memory.h"
#include <vector>
#include <deque>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <iostream>
#include <stdlib.h>
#include <dram_sim.h>

#include "mem_block_pool.h"
#include "constants.h"
#include "types.h"
#include "debug.h"
#include "VX_config.h"

using namespace vortex;

class Memory::Impl {
private:
	// A completed read is not visible to the device the same cycle it leaves
	// the DRAM model: the platform stages the response and completes a
	// registered valid/ready handshake at the device boundary, adding two
	// cycles beyond the response register.
	static constexpr uint64_t RSP_BOUNDARY_DELAY = 3;

	Memory*   simobject_;
	Config    config_;
	MemCrossBar::Ptr mem_xbar_;
	struct CompletedRead {
		MemRsp response;
		uint64_t ready_cycle;
	};
	std::vector<std::deque<CompletedRead>> completed_reads_;
	DramSim   dram_sim_;
	RAM*      ram_;
	Memory::PreSendHook pre_send_hook_;
	mutable PerfStats perf_stats_;
	struct DramCallbackArgs {
		Memory::Impl* memsim;
		MemReq request;
		uint32_t bank_id;
		std::shared_ptr<mem_block_t> rsp_data;  // captured at request time for reads
	};

public:
	Impl(Memory* simobject, const Config& config)
		: simobject_(simobject)
		, config_(config)
		, dram_sim_(config.dram)
		, ram_(nullptr)
	{
		if (config.dram.slice_bind) {
			completed_reads_.resize(config.num_ports);
			__assert(config.num_ports == config.dram.num_channels && config.num_ports <= 64,
			         "slice binding needs one port per DRAM channel and at most 64 channels");
			for (auto& lane : simobject_->mem_req_in) {
				lane.set_registered_ready(true);
			}
			return;
		}
		char sname[100];
		snprintf(sname, 100, "%s-xbar", simobject->name().c_str());
		mem_xbar_ = MemCrossBar::Create(sname, ArbiterType::RoundRobin, config.num_ports, config.dram.num_channels,
			[dram = config.dram](const MemCrossBar::ReqType& req) {
			return dram.locate(req.addr).channel;
		});
		for (uint32_t i = 0; i < config.num_ports; ++i) {
			simobject->mem_req_in.at(i).bind(&mem_xbar_->ReqIn.at(i));
			mem_xbar_->RspOut.at(i).bind(&simobject->mem_rsp_out.at(i));
		}
	}

	~Impl() {}

	const PerfStats& perf_stats() const {
		perf_stats_.bank_stalls = mem_xbar_ ? mem_xbar_->collisions() : 0;
		return perf_stats_;
	}

	void reset() {
		dram_sim_.reset();
		for (auto& reads : completed_reads_) {
			reads.clear();
		}
	}

	void tick() {
		uint32_t num_banks = config_.dram.num_channels;
		const bool finite_binding = config_.dram.slice_bind && config_.dram.queue_size != 0;
		uint64_t ready_channels = 0;
		if (finite_binding) {
			// The device samples ready before the DRAM tick can free an entry.
			for (uint32_t i = 0; i < num_banks; ++i) {
				ready_channels |= uint64_t(dram_sim_.can_accept(i)) << i;
			}
		}
		dram_sim_.tick();

		uint64_t now = SimPlatform::instance().cycles();
		for (uint32_t channel = 0; channel < completed_reads_.size(); ++channel) {
			auto& reads = completed_reads_.at(channel);
			if (!reads.empty() && reads.front().ready_cycle <= now
			 && simobject_->mem_rsp_out.at(channel).try_send(reads.front().response, 1)) {
				DT(3, simobject_->name() << " mem-rsp" << channel << ": " << reads.front().response);
				reads.pop_front();
			}
		}

		for (uint32_t i = 0; i < num_banks; ++i) {
			auto& req_in = mem_xbar_ ? mem_xbar_->ReqOut.at(i) : simobject_->mem_req_in.at(i);
			if (req_in.empty())
				continue;
			if (!(finite_binding ? ((ready_channels >> i) & 1) : dram_sim_.can_accept(i)))
				continue;

			auto& mem_req = req_in.peek();
			auto loc = config_.dram.locate(mem_req.addr);
			__assert(!config_.dram.slice_bind || loc.channel == i, "request arrived at the wrong bound DRAM channel");

			std::shared_ptr<mem_block_t> rsp_data;
			if (ram_) {
				uint64_t line_addr = mem_req.addr & ~uint64_t(VX_CFG_MEM_BLOCK_SIZE - 1);
				// Cache fills/writebacks are simulator-internal traffic and
				// don't carry the kernel's intent (e.g. a write-back cache
				// reads a write-only buffer to fill the line on write-miss,
				// since memory buses lack per-region read/write permissions).
				// Suppress ACL for the duration; ACL still guards upload/download.
				ram_->enable_acl(false);
				if (mem_req.is_write()) {
					// Apply byte-enabled write to RAM at request arrival.
					// IO_COUT-range bytes are tapped to the print buffer and
					// not stored in RAM.
					if (mem_req.data) {
						for (uint32_t b = 0; b < VX_CFG_MEM_BLOCK_SIZE; ++b) {
							if (mem_req.byteen & (1ull << b)) {
								uint8_t value = (*mem_req.data)[b];
								ram_->write(&value, line_addr + b, 1);
							}
						}
					}
				} else {
					// Capture the line at request time; response carries it back.
					rsp_data = make_mem_block();
					ram_->read(rsp_data->data(), line_addr, VX_CFG_MEM_BLOCK_SIZE);
				}
				ram_->enable_acl(true);
			}

			if (pre_send_hook_) {
				pre_send_hook_(mem_req);
			}

			// enqueue the request to the memory system
			auto req_args = new DramCallbackArgs{this, mem_req, i, rsp_data};
			dram_sim_.send_request(
				i,
				loc.addr,
				mem_req.is_write(),
				[](void* arg)->bool {
					auto rsp_args = reinterpret_cast<const DramCallbackArgs*>(arg);
					if (rsp_args->request.is_write()) {
						delete rsp_args;
						return true;
					} else {
						MemRsp mem_rsp{rsp_args->request.tag, rsp_args->request.hart_id, rsp_args->request.uuid};
						mem_rsp.data = rsp_args->rsp_data;
						auto& memory = *rsp_args->memsim;
						uint32_t channel = rsp_args->bank_id;
						if (memory.config_.dram.slice_bind) {
							// The native harness retires DRAM completions before waiting on the device.
							uint64_t ready = SimPlatform::instance().cycles() + RSP_BOUNDARY_DELAY - 1;
							memory.completed_reads_.at(channel).push_back({std::move(mem_rsp), ready});
							delete rsp_args;
							return true;
						}
						auto& rsp_out = memory.mem_xbar_->RspIn.at(channel);
						if (rsp_out.try_send(mem_rsp, RSP_BOUNDARY_DELAY)) {
							DT(3, rsp_args->memsim->simobject_->name() << " mem-rsp" << rsp_args->bank_id << ": " << mem_rsp);
							delete rsp_args;
							return true;
						}
					}
					return false; // stall
				},
				req_args
			);

			DT(3, simobject_->name() << " mem-req" << i << ": " << mem_req);
			req_in.pop();
		}
	}

	void attach_ram(RAM* ram) {
		ram_ = ram;
	}

	void set_pre_send_hook(Memory::PreSendHook hook) {
		pre_send_hook_ = std::move(hook);
	}
};

///////////////////////////////////////////////////////////////////////////////

Memory::Memory(const SimContext& ctx, const char* name, const Config& config)
	: SimObject<Memory>(ctx, name)
	, mem_req_in(config.num_ports, this)
	, mem_rsp_out(config.num_ports, this)
	, impl_(new Impl(this, config))
{}

Memory::~Memory() {
  delete impl_;
}

void Memory::on_reset() {
  impl_->reset();
}

void Memory::on_tick() {
  impl_->tick();
}

void Memory::attach_ram(RAM* ram) {
  impl_->attach_ram(ram);
}

void Memory::set_pre_send_hook(PreSendHook hook) {
  impl_->set_pre_send_hook(std::move(hook));
}

const Memory::PerfStats &Memory::perf_stats() const {
	return impl_->perf_stats();
}
