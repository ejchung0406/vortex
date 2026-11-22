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
#include <deque>
#include <vector>
#include "types.h"
#include "VX_types.h"

namespace vortex {

struct kmu_req_t {
  uint64_t PC;
  uint64_t entry;
  uint64_t param;
  uint32_t cta_id;
  uint32_t block_idx[3];
  uint32_t block_dim[3];
  uint32_t grid_dim[3];
  uint32_t lmem_size;
  uint32_t tmem_cols;   // TMEM columns a CTA may allocate (0 = none)
  uint32_t block_size;
  uint32_t warp_step[3];
  uint32_t cluster_dim[3];
  bool     is_first_of_cluster;
  // Launch identity: bumped on every start(), so consecutive launches of the
  // same kernel are still distinct contexts.
  uint8_t  ctx_id;
};

// Kernel Management Unit. Holds the in-flight kernel descriptor, walks the
// grid, and pushes one CTA per cycle onto a ready core's launch lane.
class Kmu : public SimObject<Kmu> {
public:
  // Per-core launch lanes. Each lane is bound to a registered boundary stage
  // feeding the core dispatcher's endpoint; readiness is the lane's local
  // full() view — the KMU never reads a consumer's state.
  std::vector<SimChannel<kmu_req_t>> bus_out;

  // Per-core credit returns: the dispatcher sends one message per admitted
  // CTA. Dispatch is additionally gated on the in-flight count so pacing
  // rests on explicit slot accounting, not on transport back-pressure.
  std::vector<SimChannel<uint8_t>> credit_in;

  // Each core's launch buffer holds two CTAs it has not yet admitted. A
  // deeper buffer would commit work to a busy core early and keep it from a
  // core that frees a slot sooner.
  static constexpr uint32_t LAUNCH_CREDITS = 2;

  // Lane c is core c % cores_per_socket of socket c / cores_per_socket, and
  // socket s is socket s % sockets_per_cluster of cluster s / sockets_per_cluster.
  Kmu(const SimContext& ctx, const char* name, uint32_t num_clusters,
      uint32_t sockets_per_cluster, uint32_t cores_per_socket);

  void dcr_write(uint32_t addr, uint32_t value);

  // Called by ProcessorImpl::run() to arm a kernel launch (host context,
  // between cycles).
  void start();

  // True while CTAs remain to be issued.
  bool running() const { return running_; }

  // True while CTAs remain to be issued or are still in the fan-out tree.
  bool busy() const;

  // A grid-less launch is a delegated draw launch: the KMU walks no CTAs and
  // the processor forwards the frame kick to the raster engines.
  bool launch_delegated() const {
    return !(grid_dim_[0] && grid_dim_[1] && grid_dim_[2]);
  }

  // Program image base PC (KMU STARTUP_ADDR) — where every warp begins
  // executing (__vx_cta_entry). Persists across reset; the Fragment Work
  // Distributor reuses it as the launch PC for injected fragment warps.
  uint64_t startup_pc() const { return PC_; }

protected:
  void on_reset();
  void on_tick();

private:
  // A CTA on its way down the fan-out tree; `eop` marks a cluster's last
  // member, which ends the message every router holds its choice for.
  struct beat_t {
    kmu_req_t req;
    bool      eop;
  };

  // One router of the fan-out tree: its round-robin pointer, and the output
  // it holds while a message (a cluster of CTAs) passes.
  struct router_t {
    uint32_t rr;
    bool     locked;
    uint32_t lock_sel;
  };

  // Fill *beat with the next CTA; false when the grid is exhausted.
  bool next_cta(beat_t* beat);

  // Step router `node` of level `lvl` once; occupancy is each link's fill
  // at the start of the cycle.
  void route(uint32_t lvl, uint32_t node, const std::vector<std::vector<uint32_t>>& occupancy);

  uint64_t PC_;
  uint64_t entry_;
  uint64_t param_;
  uint32_t block_dim_[3];
  uint32_t grid_dim_[3];
  uint32_t lmem_size_;
  uint32_t tmem_cols_;
  uint32_t block_size_;
  uint32_t warp_step_[3];
  uint32_t cluster_dim_[3];
  bool     running_;
  uint32_t cta_id_;
  // The launch fan-out tree (device -> clusters -> sockets -> cores), with
  // the levels that have a single output left out: they pass a CTA straight
  // through. fanout_[l] is the outputs of a level-l router, span_[l] the cores
  // below each output. Every output holds LINK_DEPTH CTAs; at the last level
  // that buffer is the lane's credits, above it a link_ queue.
  static constexpr uint32_t LINK_DEPTH = 2;
  std::vector<uint32_t> fanout_;
  std::vector<uint32_t> span_;
  std::vector<std::vector<router_t>> routers_;           // [level][node]
  std::vector<std::vector<std::deque<beat_t>>> links_;  // [level][node] input, level >= 1
  uint8_t  ctx_id_;
  // Nested counter: group_origin advances by cluster_dim per axis;
  // intra_offset walks within the cluster.
  uint32_t group_origin_[3];
  uint32_t intra_offset_[3];
  // CTAs sent but not yet admitted by each core's dispatcher.
  std::vector<uint32_t> inflight_;
  // Cores masked off for launches; kept across launches like the descriptor.
  uint32_t core_disable_[VX_DCR_KMU_CORE_DISABLE_WORDS];

  friend class SimObject<Kmu>;
};

} // namespace vortex
