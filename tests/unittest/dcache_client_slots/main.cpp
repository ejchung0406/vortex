#include <array>
#include <iostream>
#include "lsu_mem_adapter.h"
#include "mem_coalescer.h"

using namespace vortex;

namespace {
template <typename Client>
int check_slots(const std::shared_ptr<Client>& client) {
  SimChannel<LsuRsp> sink(client.get(), 1);
  client->RspOut.bind(&sink);
  auto& platform = SimPlatform::instance();
  platform.reset();

  constexpr unsigned num_reads = 4;
  std::array<MemRsp, DCACHE_CHANNELS> replies;
  for (unsigned tag = 0; tag < num_reads; ++tag) {
    LsuReq read(LSU_CHANNELS);
    read.tag = tag;
    for (unsigned port = 0; port < DCACHE_CHANNELS; ++port) {
      const auto lane = port * (LSU_CHANNELS / DCACHE_CHANNELS);
      read.mask.set(lane);
      read.addrs.at(lane) = (tag * DCACHE_CHANNELS + port) * DCACHE_WORD_SIZE;
    }
    client->ReqIn.send(read);
    for (auto& out : client->ReqOut) {
      while (out.empty() && platform.cycles() < 100) {
        platform.tick();
      }
      if (out.empty()) {
        std::cerr << "missing request for LSU tag " << tag << '\n';
        return 1;
      }
    }
    bool ok = true;
    for (unsigned port = 0; port < DCACHE_CHANNELS; ++port) {
      auto& req = client->ReqOut.at(port);
      replies.at(port) = MemRsp(req.peek().tag, req.peek().hart_id, req.peek().uuid);
      req.pop();
      const bool accepted = client->RspIn.at(port).try_send(replies.at(port));
      // Tag 0 fills the stalled sink. The next two occupy the wrapper skid.
      if (accepted != (tag < num_reads - 1)) {
        std::cerr << client->name() << " port " << port << " returned ready=" << accepted
                  << " for LSU tag " << tag << '\n';
        ok = false;
      }
    }
    if (!ok) {
      return 1;
    }
    if (tag == 0) {
      while (sink.empty() && platform.cycles() < 100) {
        platform.tick();
      }
      if (sink.empty()) {
        std::cerr << "first response did not reach the sink\n";
        return 1;
      }
    }
  }

  for (unsigned tag = 0; tag < num_reads; ++tag) {
    while (sink.empty() && platform.cycles() < 100) {
      platform.tick();
    }
    if (sink.empty() || sink.peek().tag != tag || sink.peek().mask.count() != DCACHE_CHANNELS) {
      std::cerr << "failed to drain LSU response " << tag << '\n';
      return 1;
    }
    sink.pop();
    if (tag == 1) {
      for (unsigned port = 0; port < DCACHE_CHANNELS; ++port) {
        if (!client->RspIn.at(port).try_send(replies.at(port))) {
          std::cerr << "port " << port << " did not regain capacity\n";
          return 1;
        }
      }
    }
  }
  std::cout << client->name() << " accepted two responses per stalled port and drained all four reads\n";
  return 0;
}
} // namespace

int main() {
  static_assert(VX_CFG_DCACHE_ENABLED && VX_CFG_NUM_DCACHES == VX_CFG_SOCKET_SIZE);
  static_assert(VX_CFG_L1_LINK_LATENCY == 0 && VX_CFG_DCACHE_READ_MERGE && DCACHE_CHANNELS == 2);
  if constexpr (DCACHE_WORD_SIZE > LSU_WORD_SIZE) {
    return check_slots(MemCoalescer::Create("coalescer", LSU_CHANNELS, DCACHE_CHANNELS,
                                            DCACHE_WORD_SIZE, LSU_QUEUE_OUT_SIZE, 2));
  } else {
    return check_slots(LsuMemAdapter::Create("adapter", DCACHE_CHANNELS, 1, 1));
  }
}
