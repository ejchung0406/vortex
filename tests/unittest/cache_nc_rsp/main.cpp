#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include "cache.h"
#include "mem_block_pool.h"

using namespace vortex;

namespace {
constexpr unsigned count = 32;
constexpr unsigned warm_tag = 2 * count;
constexpr uint64_t hot_line = 0;

[[noreturn]] void fail(const char* message) {
  std::cerr << "FAILED: " << message << '\n';
  std::exit(1);
}

MemRsp reply(const MemReq& req, uint8_t value) {
  auto data = make_mem_block();
  data->fill(value);
  return MemRsp(req.tag, req.hart_id, req.uuid, data);
}
} // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    fail("expected read-merge and cached-port arguments");
  }
  const unsigned merge = std::stoul(argv[1]);
  const unsigned cached_port = std::stoul(argv[2]);
  Cache::Config config{};
  config.size = 2048;
  config.L = config.S = config.W = log2ceil(VX_CFG_MEM_BLOCK_SIZE);
  config.B = 1;
  config.num_ways = 1;
  config.num_inputs = config.mem_ports = 2;
  config.addr_width = 32;
  config.is_llc = config.core_out_buf = config.nc_enable = true;
  config.mshr_size = 16;
  config.latency = 2;
  config.read_merge = merge;
  auto cache = Cache::Create("cache", config);
  auto& platform = SimPlatform::instance();
  platform.reset();

  // Capture genuine bypass tags, then release their replies into an already hot cache.
  cache->core_req_in.at(cached_port).send(MemReq(MemOp::LD, hot_line, nullptr, 0, warm_tag));
  unsigned io_issued = 0;
  std::vector<MemRsp> io_responses;
  bool warmed = false;
  for (unsigned cycle = 0; cycle < 1000 && !(warmed && io_responses.size() == count); ++cycle) {
    auto& in = cache->core_req_in.at(0);
    if (io_issued < count && !in.full()) {
      MemReq req(MemOp::LD, (1 + io_issued) * VX_CFG_MEM_BLOCK_SIZE, nullptr, 0, io_issued);
      req.flags.io = true;
      in.send(req);
      ++io_issued;
    }
    platform.tick();
    for (unsigned lane = 0; lane < config.mem_ports; ++lane) {
      auto& out = cache->mem_req_out.at(lane);
      if (!out.empty()) {
        const auto& req = out.peek();
        if (req.flags.io) {
          if (lane != 0 || req.addr != (1 + io_responses.size()) * VX_CFG_MEM_BLOCK_SIZE) {
            fail("incorrect bypass request");
          }
          io_responses.push_back(reply(req, io_responses.size()));
        } else {
          if (req.addr != hot_line) {
            fail("unexpected cache miss");
          }
          cache->mem_rsp_in.at(lane).send(reply(req, warm_tag));
        }
        out.pop();
      }
    }
    auto& response = cache->core_rsp_out.at(cached_port);
    if (!response.empty()) {
      if (response.peek().tag != warm_tag || !response.peek().data || response.peek().data->front() != warm_tag) {
        fail("incorrect warm fill");
      }
      response.pop();
      warmed = true;
    }
  }
  if (!warmed || io_responses.size() != count) {
    fail("could not prepare both response streams");
  }

  constexpr unsigned hot_hits = 8;
  unsigned sent = 0, returned = 0;
  uint64_t first_issue = platform.cycles(), last_return = 0;
  for (unsigned cycle = 0; cycle < 1000 && returned < hot_hits; ++cycle) {
    auto& in = cache->core_req_in.at(cached_port);
    if (sent < hot_hits && !in.full()) {
      in.send(MemReq(MemOp::LD, hot_line, nullptr, 0, warm_tag + 1 + sent++));
    }
    platform.tick();
    auto& out = cache->core_rsp_out.at(cached_port);
    if (!out.empty()) {
      auto& rsp = out.peek();
      if (rsp.tag != warm_tag + 1 + returned || !rsp.data || rsp.data->front() != warm_tag) {
        fail("incorrect uncontended cached response");
      }
      uint64_t now = platform.cycles();
      if (returned == 0) {
        std::cout << "cache hit latency=" << now - first_issue << " cycles\n";
      } else if (now != last_return + 1) {
        std::cerr << "cycles between uncontended hits=" << now - last_return << '\n';
        fail("uncontended cached hits did not return one per cycle");
      }
      last_return = now;
      ++returned;
      out.pop();
    }
  }
  if (returned != hot_hits) {
    fail("uncontended cached hits did not drain");
  }

  unsigned hits_issued = 0, io_sent = 0;
  std::array<unsigned, 2> received{};
  uint64_t last_delivery = ~uint64_t{0};
  bool last_io = false;
  bool monitor = true, blocked = false, saw_full = false;
  unsigned cached_while_full = 0;
  unsigned alternating = 0;
  for (auto& port : cache->core_rsp_out) {
    port.tx_callback([&](const MemRsp& rsp, uint64_t cycle) {
      bool io = rsp.tag < count;
      // All ready outputs see the same class from the shared switch grant.
      if (monitor) {
        if (last_delivery == cycle) {
          fail("switch delivered cached and bypass responses in the same cycle");
        }
        if (received[0] != 0 && received[1] != 0) {
          if (io == last_io) {
            fail("contending cached and bypass responses did not alternate");
          }
          ++alternating;
        }
        last_delivery = cycle;
        last_io = io;
      }
    });
  }

  unsigned held_cycles = 0;
  for (unsigned cycle = 0; cycle < 1000; ++cycle) {
    auto& in = cache->core_req_in.at(cached_port);
    if (hits_issued < count && !in.full()) {
      in.send(MemReq(MemOp::LD, hot_line, nullptr, 0, count + hits_issued));
      ++hits_issued;
    }
    auto& mem = cache->mem_rsp_in.at(0);
    if (io_sent < count && !mem.full()) {
      mem.send(io_responses.at(io_sent++));
    }
    platform.tick();
    if (alternating >= 8 && monitor) {
      monitor = false;
      blocked = true;
    }
    if (blocked && cache->core_rsp_out.at(0).full()) {
      saw_full = true;
    }
    if (blocked && ++held_cycles == 16) {
      // The core port holds two responses. Draining more confirms the switch
      // grants new cached work even while the bypass port stays full.
      if (!saw_full || (cached_port != 0 && cached_while_full <= 2)) {
        fail("a blocked bypass port stopped cached responses on a ready port");
      }
      blocked = false;
    }
    for (unsigned port = 0; port < config.num_inputs; ++port) {
      auto& out = cache->core_rsp_out.at(port);
      if (out.empty() || (blocked && port == 0)) {
        continue;
      }
      const auto& rsp = out.peek();
      bool io = rsp.tag < count;
      unsigned& next = received[io ? 0 : 1];
      if (port != (io ? 0 : cached_port) || next >= count || rsp.tag != (io ? next : count + next)
       || !rsp.data || rsp.data->front() != (io ? next : warm_tag)) {
        fail("incorrect response port, order, tag, or data");
      }
      if (blocked && port != 0 && cache->core_rsp_out.at(0).full()) {
        ++cached_while_full;
      }
      ++next;
      out.pop();
    }
    if (received[0] == count && received[1] == count) {
      if (held_cycles != 16) {
        fail("did not exercise round-robin under contention and output backpressure");
      }
      std::cout << "PASSED: merge=" << merge << " cached_port=" << cached_port
                << " alternating=" << alternating << " cached while NC full=" << cached_while_full
                << " cached and NC responses=" << count << '/' << count << '\n';
      return 0;
    }
  }
  fail("response streams did not drain");
}
