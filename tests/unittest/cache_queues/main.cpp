#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include "cache.h"
#include "mem/noc.h"
#include "regslice.h"
#include "mem_block_pool.h"

using namespace vortex;

namespace {
constexpr unsigned stores = 96;
constexpr unsigned shallow = 16, deep = 32;
constexpr unsigned sector = VX_CFG_MEM_BLOCK_SIZE;

struct Stream {
  Cache::Ptr cache;
  unsigned sent = 0;
  unsigned received = 0;
};

Cache::Ptr make_cache(const char* name, unsigned mshr_size) {
  Cache::Config cfg{};
  cfg.size = 2048;
  cfg.L = cfg.S = cfg.W = log2ceil(sector);
  cfg.num_ways = cfg.num_inputs = cfg.mem_ports = 1;
  cfg.addr_width = 32;
  cfg.write_back = cfg.write_around = true;
  cfg.mshr_size = mshr_size;
  cfg.latency = 2;
  return Cache::Create(name, cfg);
}

void issue(Stream& stream) {
  auto& in = stream.cache->core_req_in.front();
  if (stream.sent < stores && !in.full()) {
    auto block = make_mem_block();
    block->fill(stream.sent);
    MemReq req(MemOp::ST, stream.sent * sector, block, ~uint64_t{0});
    req.flags.noalloc = true;
    in.send(req);
    ++stream.sent;
  }
}

void drain(Stream& stream) {
  auto& out = stream.cache->mem_req_out.front();
  if (!out.empty()) {
    const auto& req = out.peek();
    if (stream.received >= stores || !req.is_write() || req.addr != stream.received * sector
     || !req.data || !std::all_of(req.data->begin(), req.data->end(),
                                  [&](uint8_t b) { return b == stream.received; })) {
      std::cerr << "lost, duplicated or reordered write-around store\n";
      std::exit(1);
    }
    out.pop();
    ++stream.received;
  }
}

bool request_fifo() {
  std::array<Stream, 2> streams{{{make_cache("small", shallow)}, {make_cache("large", deep)}}};
  auto& platform = SimPlatform::instance();
  platform.reset();

  // Occupy the external receiver before offering stores so ready starts low.
  for (auto& stream : streams) {
    auto& out = stream.cache->mem_req_out.front();
    if (out.capacity() != 2) {
      std::cerr << "expected the timed receiver's two slots\n";
      return false;
    }
    for (unsigned i = 0; i < out.capacity(); ++i) {
      out.send(MemReq{}, 0);
    }
  }
  // No-allocate stores exercise the write-back bank's request FIFO without
  // consuming miss slots. Both streams face identical downstream backpressure.
  for (unsigned cycle = 0; cycle < 2 * stores; ++cycle) {
    for (auto& stream : streams) {
      issue(stream);
    }
    platform.tick();
  }
  std::array<unsigned, 2> accepted{};
  for (unsigned i = 0; i < streams.size(); ++i) {
    auto& stream = streams[i];
    accepted[i] = stream.sent - stream.cache->core_req_in.front().size();
    auto& out = stream.cache->mem_req_out.front();
    for (unsigned j = 0; j < out.capacity(); ++j) {
      if (out.empty() || out.peek().is_write()) {
        return false;
      }
      out.pop();
    }
  }
  std::cout << "core request handshakes with memory ready low: "
            << accepted[0] << " / " << accepted[1] << '\n';
  if (accepted[0] != shallow + 2 || accepted[1] != deep + 2) {
    return false;
  }
  for (unsigned cycle = 0; cycle < 8 * stores; ++cycle) {
    for (auto& stream : streams) {
      if (cycle % 3 == 0) {
        drain(stream);
      }
      issue(stream);
    }
    platform.tick();
  }
  return streams[0].received == stores && streams[1].received == stores;
}

bool parallel_fills(unsigned mem_ports, unsigned response_slots) {
  Cache::Config cfg{};
  cfg.size = 2048;
  cfg.L = cfg.S = cfg.W = log2ceil(sector);
  cfg.B = 2;
  cfg.num_ways = 1;
  cfg.num_inputs = 2;
  cfg.mem_ports = mem_ports;
  cfg.mrsq_size = response_slots;
  cfg.addr_width = 32;
  cfg.mshr_size = 16;
  cfg.latency = 2;
  auto cache = Cache::Create("parallel", cfg);
  auto& platform = SimPlatform::instance();
  platform.reset();

  // Banks zero and one have independent paths from two ports. From a single
  // port, the same two replies can enter the network on consecutive clocks.
  std::array<MemRsp, 2> fills;
  std::array<bool, 2> requested{};
  std::array<unsigned, 2> completed{};
  unsigned sent = 0;
  for (unsigned port = 0; port < fills.size(); ++port) {
    cache->core_req_in[port].send(MemReq(MemOp::LD, port * sector, nullptr, 0, port));
  }
  for (unsigned cycle = 1; cycle <= 512; ++cycle) {
    platform.tick();
    for (unsigned port = 0; port < mem_ports; ++port) {
      auto& memory = cache->mem_req_out[port];
      if (!memory.empty()) {
        const auto& req = memory.peek();
        unsigned bank = req.addr / sector;
        if (bank >= fills.size() || req.addr != bank * sector || bank % mem_ports != port
         || requested[bank] || req.is_write()) {
          std::cerr << "unexpected miss on memory port " << port << ": " << req << '\n';
          return false;
        }
        auto block = make_mem_block();
        block->fill(bank + 1);
        fills[bank] = MemRsp(req.tag, req.hart_id, req.uuid, block);
        requested[bank] = true;
        memory.pop();
      }
    }
    for (unsigned port = 0; port < fills.size(); ++port) {
      auto& core = cache->core_rsp_out[port];
      if (!core.empty()) {
        const auto& rsp = core.peek();
        if (completed[port] || rsp.tag != port || !rsp.data || rsp.data->front() != port + 1) {
          std::cerr << "incorrect response on core port " << port << ": " << rsp << '\n';
          return false;
        }
        completed[port] = cycle;
        core.pop();
      }
    }
    if (requested[0] && requested[1]) {
      for (unsigned port = 0; port < mem_ports && sent < fills.size(); ++port) {
        auto& in = cache->mem_rsp_in[port];
        if (sent % mem_ports == port && !in.full()) {
          in.send(fills[sent++], 0);
        }
      }
    }
    if (completed[0] && completed[1]) {
      std::cout << mem_ports << " memory ports, " << response_slots << " response slots, return cycles: "
                << completed[0] << " / " << completed[1] << '\n';
      return completed[1] == completed[0] + (mem_ports == 1);
    }
  }
  std::cerr << "parallel fills timed out: memory requests " << requested[0] << '/' << requested[1]
            << ", core response cycles " << completed[0] << '/' << completed[1] << '\n';
  return false;
}
bool stalled_forward() {
  constexpr unsigned chain = 24;
  auto cache = make_cache("forward", 2 * chain + 16);
  auto& platform = SimPlatform::instance();
  platform.reset();
  auto& input = cache->core_req_in.front();
  auto& output = cache->core_rsp_out.front();
  auto& memory = cache->mem_req_out.front();
  auto& returns = cache->mem_rsp_in.front();
  MemRsp first_fill;
  unsigned sent = 0, received = 0, misses = 0;
  bool independent_miss = false;
  for (unsigned cycle = 0; cycle < 20 * chain; ++cycle) {
    if (sent < chain && !input.full()) {
      input.send(MemReq(MemOp::LD, 0, nullptr, 0, sent++));
    }
    if (cycle == 4 * chain) {
      if (sent != chain || misses != 1) {
        return false;
      }
      returns.send(first_fill, 0);
    }
    // With the whole chain still ahead of the consumer, its held head
    // leaves the bank pipeline free to issue a miss to a different line.
    if (cycle == 8 * chain) {
      input.send(MemReq(MemOp::LD, sector, nullptr, 0, chain));
    }
    if (!memory.empty()) {
      const auto& req = memory.peek();
      auto block = make_mem_block();
      block->fill(req.addr ? 2 : 1);
      MemRsp fill(req.tag, req.hart_id, req.uuid, block);
      if (!misses && req.addr == 0) {
        first_fill = fill;
      } else if (misses == 1 && req.addr == sector) {
        independent_miss = cycle < 12 * chain;
        returns.send(fill, 0);
      } else {
        return false;
      }
      ++misses;
      memory.pop();
    }
    if (cycle >= 12 * chain && !output.empty()) {
      const auto& rsp = output.peek();
      if (rsp.tag != received || !rsp.data || rsp.data->front() != (received == chain ? 2 : 1)) {
        return false;
      }
      ++received;
      output.pop();
    }
    platform.tick();
  }
  std::cout << "independent miss issued with forward response stalled: " << independent_miss
            << ", responses: " << received << '\n';
  return independent_miss && received == chain + 1;
}

bool socket_return_stream() {
  Cache::Config cfg{};
  cfg.size = 2048;
  cfg.L = cfg.S = cfg.W = log2ceil(sector);
  cfg.B = 1;
  cfg.num_ways = cfg.num_inputs = 1;
  cfg.mem_ports = 2;
  cfg.mem_rsp_slice = true;
  cfg.mshr_size = 8;
  cfg.addr_width = 32;
  cfg.latency = 2;
  auto cache = Cache::Create("socket-cache", cfg);
  SimChannel<MemRsp> peer(cache.get(), 2);
  if (!cache->bind_response_peer(1, peer)) {
    return false;
  }

  NocSwitch::Ptr sw;
  {
    SimPlatform::DomainScope uncore(1);
    auto boundary = RegSlice<MemRsp>::Create("socket-return", 1);
    boundary->Out.bind(&cache->mem_rsp_in[1]);
    boundary->In.set_registered_ready(true);
    sw = NocSwitch::Create("switch", 2, 1, log2ceil(sector));
    sw->RspOut[1].bind(&boundary->In);
  }

  auto& platform = SimPlatform::instance();
  platform.reset();
  unsigned departed = 0, sent = 0, received = 0, previous = 0;
  sw->RspIn[0].pop_callback([&departed]() { ++departed; });
  constexpr unsigned length = 32;
  for (unsigned cycle = 0; cycle < 128; ++cycle) {
    if (sent < length && sw->RspIn[0].try_send(MemRsp((sent << 2) | 3), 0)) {
      ++sent;
    }
    platform.tick();
    if (cycle == 31 && departed != 2 + peer.capacity()) {
      std::cerr << "stopped socket accepted " << departed << " switch replies\n";
      return false;
    }
    if (cycle >= 32 && !peer.empty()) {
      if (peer.peek().tag != received || (received && cycle != previous + 1)) {
        std::cerr << "socket reply " << received << " arrived on clock " << cycle << '\n';
        return false;
      }
      ++received;
      previous = cycle;
      peer.pop();
    }
  }
  std::cout << "socket replies in consecutive clocks: " << received << '\n';
  return received == length;
}

} // namespace

int main() {
  const bool requests_ok = request_fifo();
  const bool parallel_ok = parallel_fills(2, 0);
  const bool consecutive_ok = parallel_fills(1, 1);
  const bool forward_ok = stalled_forward();
  const bool socket_ok = socket_return_stream();
  return requests_ok && parallel_ok && consecutive_ok && forward_ok && socket_ok ? 0 : 1;
}
