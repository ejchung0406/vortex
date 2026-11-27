#include "vl_simulator.h"
#include "VVX_tlb_walk_top.h"
#include <bitset>
#include <cstdio>

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  vl_simulator<VVX_tlb_walk_top> sim;
  sim->req_valid = 0;
  sim->fill_valid = 0;
  sim->walk_ready = 0;
  auto tick = sim.reset(0);

  constexpr unsigned requests = 32;
  constexpr unsigned slots = 4;
  constexpr unsigned walk_period = 16;
  std::bitset<requests> walked, replied;
  unsigned sent = 0, grants = 0, last_initial_grant = 0;
  bool held = false;
  unsigned held_slot = 0, held_vpn = 0;

  for (unsigned cycle = 0; cycle < (requests + 2) * walk_period; ++cycle) {
    sim->req_valid = sent < requests;
    sim->req_id = sent;
    // Between walker grants an early slot can fill, drain and accept another miss.
    sim->walk_ready = cycle != 0 && cycle % walk_period == 0;
    sim->eval();
    if (held && (!sim->walk_valid || sim->walk_slot != held_slot || sim->walk_vpn != held_vpn)) {
      std::fprintf(stderr, "walk payload changed under backpressure at cycle %u\n", cycle);
      return 1;
    }
    held = sim->walk_valid && !sim->walk_ready;
    held_slot = sim->walk_slot;
    held_vpn = sim->walk_vpn;
    const bool issue = sim->walk_valid && sim->walk_ready;
    if (issue) {
      if (held_vpn >= sent || walked.test(held_vpn)) {
        std::fprintf(stderr, "unexpected walk for VPN %u after %u accepts\n", held_vpn, sent);
        return 1;
      }
      walked.set(held_vpn);
      ++grants;
      if (held_vpn < slots) {
        last_initial_grant = grants;
      }
      if (grants == 4 * slots) {
        for (unsigned vpn = 0; vpn < slots; ++vpn) {
          if (!walked.test(vpn)) {
            std::fprintf(stderr, "resident VPN %u still waiting after %u walker grants\n", vpn, grants);
            return 1;
          }
        }
      }
    }
    if (sim->fill_valid && !sim->fill_ready) {
      std::fprintf(stderr, "issued slot %u refused its returning fill\n", sim->fill_slot);
      return 1;
    }
    if (sim->rsp_valid) {
      const unsigned id = sim->rsp_id;
      if (id >= requests || !walked.test(id) || replied.test(id) || !sim->rsp_ok) {
        std::fprintf(stderr, "unexpected translation response for request %u\n", id);
        return 1;
      }
      replied.set(id);
    }
    if (sim->req_valid && sim->req_ready) {
      ++sent;
    }
    if (replied.all() && sim->empty) {
      if (last_initial_grant > slots) {
        std::fprintf(stderr, "last of %u initial walks got grant %u\n", slots, last_initial_grant);
        return 1;
      }
      std::printf("PASS: all %u cold 4 KiB misses issued and retired; first %u served in %u grants\n",
                  requests, slots, last_initial_grant);
      return 0;
    }
    tick = sim.step(tick, 2);
    sim->fill_valid = issue;
    sim->fill_slot = held_slot;
    sim->fill_ppn = held_vpn + 1;
  }
  std::fprintf(stderr, "%u requests, %u walks, %zu responses at timeout\n", sent, grants, replied.count());
  return 1;
}
