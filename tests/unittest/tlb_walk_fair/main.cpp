// Copyright © 2019-2026
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0

#include "mem/tlb_l2.h"
#include <algorithm>
#include <cstdio>
#include <vector>

using namespace vortex;

int main() {
  constexpr uint32_t slots = VX_CFG_L2_TLB_MSHR_SIZE;
  constexpr uint32_t grants = 3 * slots;
  constexpr uint64_t base_vpn = 0x1000;
  auto tlb = L2Tlb::Create("tlb_walk_fair", slots);
  auto& sim = SimPlatform::instance();
  sim.reset();

  std::vector<uint64_t> pending(slots);
  auto request = [&](uint32_t client, uint32_t round) {
    TlbReq req;
    req.client_id = client;
    req.vpn = base_vpn + uint64_t(round) * slots + client;
    pending[client] = req.vpn;
    tlb->ReqIn[client].send(req);
  };
  auto settle_lookup = [&]() {
    for (uint32_t i = 0; i < VX_CFG_L2_TLB_LATENCY + 3; ++i) {
      sim.tick();
    }
  };

  // Keep the walker backpressured until every MSHR has a distinct 4 KiB miss.
  for (uint32_t id = 0; id < slots; ++id) {
    request(id, 0);
    settle_lookup();
  }

  uint32_t max_wait = 0;
  for (uint32_t i = 0; i < grants + slots; ++i) {
    auto& port = tlb->PtwReqOut;
    if (port.empty()) {
      std::fprintf(stderr, "walker stopped before grant %u\n", i + 1);
      return 1;
    }
    const auto walk = port.peek();
    port.pop();
    const uint32_t client = (walk.vpn - base_vpn) % slots;
    if (walk.vpn != pending[client] || walk.slot >= slots) {
      std::fprintf(stderr, "unexpected walker VPN or slot on grant %u\n", i + 1);
      return 1;
    }
    pending[client] = 0;
    const uint32_t prior_grants = (walk.vpn - base_vpn) / slots;
    max_wait = std::max(max_wait, i + 1 - prior_grants);

    TlbRsp fill;
    fill.ppn = walk.vpn;
    fill.flags = 0x53; // Valid, readable, user, accessed.
    fill.slot = walk.slot;
    tlb->PtwRspIn.send(fill);

    settle_lookup();
    auto& response = tlb->RspOut[client];
    if (response.empty() || response.peek().slot != 0 || response.peek().ppn != walk.vpn
        || response.peek().fault || response.peek().level != 0) {
      std::fprintf(stderr, "fill delivered to wrong requester on grant %u\n", i + 1);
      return 1;
    }
    response.pop();

    // Reuse the released MSHR before accepting the next walker request.
    if (i < grants) {
      request(client, i + 1);
      settle_lookup();
    }
  }

  if (tlb->busy()) {
    std::fprintf(stderr, "TLB still busy after all walks completed\n");
    return 1;
  }
  if (max_wait > slots) {
    std::fprintf(stderr, "longest wait was %u grants with %u MSHRs\n", max_wait, slots);
    return 1;
  }
  std::printf("PASSED: %u walks drained, longest wait %u grants with %u MSHRs\n",
              grants + slots, max_wait, slots);
  return 0;
}
