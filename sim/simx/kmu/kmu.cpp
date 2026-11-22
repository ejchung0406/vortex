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

#include "kmu.h"
#include <cassert>

using namespace vortex;

Kmu::Kmu(const SimContext& ctx, const char* name, uint32_t num_clusters,
         uint32_t sockets_per_cluster, uint32_t cores_per_socket)
  : SimObject<Kmu>(ctx, name)
  , bus_out(num_clusters * sockets_per_cluster * cores_per_socket, this)
  , credit_in(num_clusters * sockets_per_cluster * cores_per_socket, this)
  , PC_(0)
  , entry_(0)
  , param_(0)
  , lmem_size_(0)
  , tmem_cols_(0)
  , block_size_(0)
  , running_(false)
  , cta_id_(0)
  , ctx_id_(0)
  , inflight_(num_clusters * sockets_per_cluster * cores_per_socket, 0)
{
  __assert(inflight_.size() <= 32 * VX_DCR_KMU_CORE_DISABLE_WORDS, "the core-disable mask is too narrow");
  for (auto& w : core_disable_) {
    w = 0;
  }
  for (uint32_t n : {num_clusters, sockets_per_cluster, cores_per_socket}) {
    if (n > 1) {
      fanout_.push_back(n);
    }
  }
  if (fanout_.empty()) {
    fanout_.push_back(1);
  }
  uint32_t nodes = 1;
  span_.resize(fanout_.size());
  routers_.resize(fanout_.size());
  links_.resize(fanout_.size());
  for (uint32_t l = 0; l < fanout_.size(); ++l) {
    routers_[l].assign(nodes, router_t{0, false, 0});
    if (l != 0) {
      links_[l].resize(nodes);
    }
    nodes *= fanout_[l];
  }
  for (uint32_t l = 0, below = nodes; l < fanout_.size(); ++l) {
    below /= fanout_[l];
    span_[l] = below;
  }
  block_dim_[0]    = block_dim_[1]    = block_dim_[2]    = 1;
  grid_dim_[0]     = grid_dim_[1]     = grid_dim_[2]     = 1;
  cluster_dim_[0]  = cluster_dim_[1]  = cluster_dim_[2]  = 1;
  warp_step_[0]    = warp_step_[1]    = warp_step_[2]    = 1;
  group_origin_[0] = group_origin_[1] = group_origin_[2] = 0;
  intra_offset_[0] = intra_offset_[1] = intra_offset_[2] = 0;
}

void Kmu::on_reset() {
  // Reset only the per-run progression state. The kernel descriptor (PC,
  // param, dims, block_size, lmem_size, warp_step, cluster_dim) is set
  // by dcr_write() before run() and must persist across
  // SimPlatform::on_reset().
  // ctx_id_ is excluded: it is what distinguishes one launch from the next, so
  // re-zeroing it here would pin it to the same value on every launch and
  // destroy the distinction. It is free-running from device reset, and
  // zero-initialized at construction.
  running_         = false;
  cta_id_          = 0;
  for (auto& level : routers_) {
    level.assign(level.size(), router_t{0, false, 0});
  }
  for (auto& level : links_) {
    for (auto& link : level) {
      link.clear();
    }
  }
  group_origin_[0] = group_origin_[1] = group_origin_[2] = 0;
  intra_offset_[0] = intra_offset_[1] = intra_offset_[2] = 0;
  inflight_.assign(inflight_.size(), 0);
}

void Kmu::dcr_write(uint32_t addr, uint32_t value) {
  switch (addr) {
  case VX_DCR_KMU_STARTUP_ADDR0: PC_    = (PC_    & ~uint64_t(0xFFFFFFFF)) | value; break;
  case VX_DCR_KMU_STARTUP_ADDR1: PC_    = (PC_    &  uint64_t(0xFFFFFFFF)) | (uint64_t(value) << 32); break;
  case VX_DCR_KMU_KERNEL_ENTRY0: entry_ = (entry_ & ~uint64_t(0xFFFFFFFF)) | value; break;
  case VX_DCR_KMU_KERNEL_ENTRY1: entry_ = (entry_ &  uint64_t(0xFFFFFFFF)) | (uint64_t(value) << 32); break;
  case VX_DCR_KMU_STARTUP_ARG0:  param_ = (param_ & ~uint64_t(0xFFFFFFFF)) | value; break;
  case VX_DCR_KMU_STARTUP_ARG1:  param_ = (param_ &  uint64_t(0xFFFFFFFF)) | (uint64_t(value) << 32); break;
  case VX_DCR_KMU_BLOCK_DIM_X:   block_dim_[0] = value; break;
  case VX_DCR_KMU_BLOCK_DIM_Y:   block_dim_[1] = value; break;
  case VX_DCR_KMU_BLOCK_DIM_Z:   block_dim_[2] = value; break;
  case VX_DCR_KMU_GRID_DIM_X:    grid_dim_[0]  = value; break;
  case VX_DCR_KMU_GRID_DIM_Y:    grid_dim_[1]  = value; break;
  case VX_DCR_KMU_GRID_DIM_Z:    grid_dim_[2]  = value; break;
  case VX_DCR_KMU_LMEM_SIZE:     lmem_size_    = value; break;
  case VX_DCR_KMU_TMEM_COLS:     tmem_cols_    = value; break;
  case VX_DCR_KMU_BLOCK_SIZE:    block_size_   = value; break;
  case VX_DCR_KMU_WARP_STEP_X:   warp_step_[0] = value; break;
  case VX_DCR_KMU_WARP_STEP_Y:   warp_step_[1] = value; break;
  case VX_DCR_KMU_WARP_STEP_Z:   warp_step_[2] = value; break;
  case VX_DCR_KMU_CLUSTER_DIM_X: cluster_dim_[0] = value; break;
  case VX_DCR_KMU_CLUSTER_DIM_Y: cluster_dim_[1] = value; break;
  case VX_DCR_KMU_CLUSTER_DIM_Z: cluster_dim_[2] = value; break;
  default:
    if (addr >= VX_DCR_KMU_CORE_DISABLE0
     && addr < VX_DCR_KMU_CORE_DISABLE0 + VX_DCR_KMU_CORE_DISABLE_WORDS) {
      core_disable_[addr - VX_DCR_KMU_CORE_DISABLE0] = value;
    }
    break;
  }
}

void Kmu::start() {
  running_ = (block_size_ > 0)
           && (grid_dim_[0] > 0)
           && (grid_dim_[1] > 0)
           && (grid_dim_[2] > 0)
           && (cluster_dim_[0] > 0)
           && (cluster_dim_[1] > 0)
           && (cluster_dim_[2] > 0);
  ctx_id_ += 1;
  if (running_) {
    cta_id_          = 0;
    group_origin_[0] = group_origin_[1] = group_origin_[2] = 0;
    intra_offset_[0] = intra_offset_[1] = intra_offset_[2] = 0;
    this->tick_wake();
  }
}

bool Kmu::busy() const {
  if (running_) {
    return true;
  }
  for (auto& level : links_) {
    for (auto& link : level) {
      if (!link.empty()) {
        return true;
      }
    }
  }
  return false;
}

void Kmu::on_tick() {
  // Collect credits returned by dispatcher admissions.
  uint32_t num_cores = bus_out.size();
  for (uint32_t c = 0; c < num_cores; ++c) {
    auto& credits = credit_in.at(c);
    while (!credits.empty()) {
      credits.pop();
      assert(inflight_[c] != 0);
      --inflight_[c];
    }
  }
  if (!this->busy()) {
    this->tick_sleep();
    return;
  }
  // Each router moves at most one CTA a cycle. The links are registered: a
  // router sees the room a link had at the start of the cycle, and a CTA
  // pushed into a link this cycle leaves it next cycle, so the levels step
  // from the cores up.
  std::vector<std::vector<uint32_t>> occupancy(links_.size());
  for (uint32_t l = 1; l < links_.size(); ++l) {
    for (auto& link : links_[l]) {
      occupancy[l].push_back(link.size());
    }
  }
  for (uint32_t l = fanout_.size(); l-- > 0;) {
    for (uint32_t node = 0; node < routers_[l].size(); ++node) {
      this->route(l, node, occupancy);
    }
  }
}

void Kmu::route(uint32_t lvl, uint32_t node, const std::vector<std::vector<uint32_t>>& occupancy) {
  const bool last = (lvl + 1 == fanout_.size());
  if (lvl == 0 ? !running_ : links_[lvl][node].empty()) {
    return;
  }
  // An output takes a new message while some core below it is unmasked; a
  // last-level output (a lane) is ready when its transport has room and its
  // core holds fewer than LAUNCH_CREDITS CTAs it has not yet admitted, a link
  // when it had room at the start of the cycle.
  const uint32_t n = fanout_[lvl];
  auto enabled = [&](uint32_t child) {
    for (uint32_t c = child * span_[lvl]; c < (child + 1) * span_[lvl]; ++c) {
      if (((core_disable_[c / 32] >> (c % 32)) & 1) == 0) {
        return true;
      }
    }
    return false;
  };
  auto ready = [&](uint32_t child) {
    if (last) {
      return !bus_out.at(child).full() && inflight_[child] < LAUNCH_CREDITS;
    }
    return occupancy[lvl + 1][child] < LINK_DEPTH;
  };
  // A message's later members follow its first; a new message goes to the
  // nearest ready, enabled output from the round-robin pointer, so a core that
  // drains its work sooner takes the next cluster instead of the launch
  // waiting on a busy neighbour.
  auto& r = routers_[lvl][node];
  uint32_t sel = n;
  if (r.locked) {
    if (ready(node * n + r.lock_sel)) {
      sel = r.lock_sel;
    }
  } else {
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t o = (r.rr + k) % n;
      if (enabled(node * n + o) && ready(node * n + o)) {
        sel = o;
        break;
      }
    }
  }
  if (sel == n) {
    return;
  }
  beat_t beat;
  if (lvl == 0) {
    if (!this->next_cta(&beat)) {
      return;
    }
  } else {
    beat = links_[lvl][node].front();
    links_[lvl][node].pop_front();
  }
  uint32_t child = node * n + sel;
  if (last) {
    // the lane slice ticks after the KMU and registers the CTA into the core
    bus_out.at(child).send(beat.req, 0);
    ++inflight_[child];
  } else {
    links_[lvl + 1][child].push_back(beat);
  }
  r.locked   = !beat.eop;
  r.lock_sel = sel;
  if (beat.eop) {
    r.rr = (sel + 1) % n;
  }
}

bool Kmu::next_cta(beat_t* beat) {
  if (!running_) return false;
  kmu_req_t* req = &beat->req;

  // A cluster's CTAs must all land on one core: they address each other's local
  // memory by rank and rendezvous on a per-core barrier slot. So a cluster is
  // one message, and every router holds its first member's output until the
  // last member passes.
  beat->eop = ((intra_offset_[0] + 1) == cluster_dim_[0])
           && ((intra_offset_[1] + 1) == cluster_dim_[1])
           && ((intra_offset_[2] + 1) == cluster_dim_[2]);

  // Effective block_idx = group_origin + intra_offset.
  uint32_t block_idx[3] = {
    group_origin_[0] + intra_offset_[0],
    group_origin_[1] + intra_offset_[1],
    group_origin_[2] + intra_offset_[2],
  };

  req->PC           = PC_;
  req->entry        = entry_;
  req->param        = param_;
  req->cta_id       = cta_id_;
  req->ctx_id       = ctx_id_;
  req->block_idx[0] = block_idx[0];
  req->block_idx[1] = block_idx[1];
  req->block_idx[2] = block_idx[2];
  req->block_dim[0] = block_dim_[0];
  req->block_dim[1] = block_dim_[1];
  req->block_dim[2] = block_dim_[2];
  req->grid_dim[0]  = grid_dim_[0];
  req->grid_dim[1]  = grid_dim_[1];
  req->grid_dim[2]  = grid_dim_[2];
  req->lmem_size    = lmem_size_;
  req->tmem_cols    = tmem_cols_;
  req->block_size   = block_size_;
  req->warp_step[0] = warp_step_[0];
  req->warp_step[1] = warp_step_[1];
  req->warp_step[2] = warp_step_[2];
  req->cluster_dim[0] = cluster_dim_[0];
  req->cluster_dim[1] = cluster_dim_[1];
  req->cluster_dim[2] = cluster_dim_[2];
  req->is_first_of_cluster = (intra_offset_[0] == 0)
                          && (intra_offset_[1] == 0)
                          && (intra_offset_[2] == 0);

  // Advance the intra-cluster offset first (fills the cluster), then the
  // group_origin in (X, Y, Z) order when the inner loop wraps.
  ++cta_id_;
  bool intra_x_wrap = (intra_offset_[0] + 1 == cluster_dim_[0]);
  if (!intra_x_wrap) {
    intra_offset_[0]++;
  } else {
    intra_offset_[0] = 0;
    bool intra_y_wrap = (intra_offset_[1] + 1 == cluster_dim_[1]);
    if (!intra_y_wrap) {
      intra_offset_[1]++;
    } else {
      intra_offset_[1] = 0;
      bool intra_z_wrap = (intra_offset_[2] + 1 == cluster_dim_[2]);
      if (!intra_z_wrap) {
        intra_offset_[2]++;
      } else {
        intra_offset_[2] = 0;
        // Cluster complete — advance group_origin in (X, Y, Z) order.
        uint32_t ox = group_origin_[0] + cluster_dim_[0];
        if (ox == grid_dim_[0]) {
          group_origin_[0] = 0;
          uint32_t oy = group_origin_[1] + cluster_dim_[1];
          if (oy == grid_dim_[1]) {
            group_origin_[1] = 0;
            uint32_t oz = group_origin_[2] + cluster_dim_[2];
            if (oz == grid_dim_[2]) {
              group_origin_[2] = 0;
              running_ = false;
            } else {
              group_origin_[2] = oz;
            }
          } else {
            group_origin_[1] = oy;
          }
        } else {
          group_origin_[0] = ox;
        }
      }
    }
  }

  return true;
}
