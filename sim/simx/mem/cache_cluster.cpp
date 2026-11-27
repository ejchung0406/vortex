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

#include "cache_cluster.h"

using namespace vortex;

CacheCluster::CacheCluster(const SimContext& ctx,
                           const char* name,
                           uint32_t num_inputs,
                           uint32_t num_units,
                           const Cache::Config& cache_config)
	: SimObject(ctx, name)
	, core_req_in(num_inputs, std::vector<SimChannel<MemReq>>(cache_config.num_inputs, this))
	, core_rsp_out(num_inputs, std::vector<SimChannel<MemRsp>>(cache_config.num_inputs, this))
	, mem_req_out(cache_config.mem_ports, this)
	, mem_rsp_in(cache_config.mem_ports, this)
	, caches_(__MAX(num_units, 0x1))
{
	Cache::Config cache_config2(cache_config);
	if (0 == num_units) {
		num_units = 1;
		cache_config2.bypass = true;
	}

	char sname[100];

	// Arbitrate incoming core interfaces. Arbitrating cores onto fewer caches
	// registers the request, and the response once more when the cache
	// registers its core-response port.
	const bool shared = (num_inputs != num_units);
	const uint32_t arb_req_delay = shared ? 1 : 0;
	const uint32_t arb_rsp_delay = (shared && cache_config.core_out_buf) ? 1 : 0;
	std::vector<MemArbiter::Ptr> input_arbs(cache_config.num_inputs);
	for (uint32_t i = 0; i < cache_config.num_inputs; ++i) {
		snprintf(sname, 100, "%s-input-arb%d", name, i);
		input_arbs.at(i) = MemArbiter::Create(sname, ArbiterType::RoundRobin, num_inputs, num_units,
		                                      arb_req_delay, arb_rsp_delay);
		for (uint32_t j = 0; j < num_inputs; ++j) {
			this->core_req_in.at(j).at(i).bind(&input_arbs.at(i)->ReqIn.at(j));
			input_arbs.at(i)->RspOut.at(j).bind(&this->core_rsp_out.at(j).at(i));
		}
	}

	// Arbitrate outgoing memory interfaces
	mem_arbs_.resize(cache_config.mem_ports);
	for (uint32_t i = 0; i < cache_config.mem_ports; ++i) {
		snprintf(sname, 100, "%s-mem-arb%d", name, i);
		auto& arb = mem_arbs_.at(i);
		arb = MemArbiter::Create(sname, ArbiterType::RoundRobin, num_units, 1,
		                         unsigned(num_units > 2), unsigned(num_units > 1));
		arb->ReqOut.at(0).bind(&this->mem_req_out.at(i));
		this->mem_rsp_in.at(i).bind(&arb->RspIn.at(0));
		if (num_units > 1) {
			if (cache_config.mem_rsp_slice && i != 0) {
				arb->RspIn[0].retain_boundary_source();
			} else {
				arb->RspIn[0].set_registered_ready(true);
			}
		}
	}

	// Connect caches
	for (uint32_t i = 0; i < num_units; ++i) {
		snprintf(sname, 100, "%s%d", name, i);
		caches_.at(i) = Cache::Create(sname, cache_config2);

		for (uint32_t j = 0; j < cache_config.num_inputs; ++j) {
			input_arbs.at(j)->ReqOut.at(i).bind(&caches_.at(i)->core_req_in.at(j));
			caches_.at(i)->core_rsp_out.at(j).bind(&input_arbs.at(j)->RspIn.at(i));
		}

		for (uint32_t j = 0; j < cache_config.mem_ports; ++j) {
			caches_.at(i)->mem_req_out.at(j).bind(&mem_arbs_.at(j)->ReqIn.at(i));
			auto& out = mem_arbs_.at(j)->RspOut.at(i);
			out.bind(&caches_.at(i)->mem_rsp_in.at(j));
			if (num_units > 1) {
				auto* sink = &out;
				while (sink->sink()) {
					sink = static_cast<SimChannel<MemRsp>*>(sink->sink());
				}
				sink->set_registered_ready(true);
			}
		}
	}
}

CacheCluster::~CacheCluster() {}

bool CacheCluster::bind_response_peer(unsigned port, SimChannel<MemRsp>& peer) {
  if (caches_.size() == 1) {
    return caches_.front()->bind_response_peer(port, peer);
  }
  mem_arbs_.at(port)->bind_response_peer(peer);
  return true;
}

Cache::PerfStats CacheCluster::perf_stats() const {
	Cache::PerfStats perf;
	for (auto cache : caches_) {
		perf += cache->perf_stats();
	}
	return perf;
}

void CacheCluster::flush_begin(uint32_t fence_op) {
	for (auto cache : caches_) {
		cache->flush_begin(fence_op);
	}
}

bool CacheCluster::flush_done() const {
	for (auto cache : caches_) {
		if (!cache->flush_done()) return false;
	}
	return true;
}
