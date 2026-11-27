#include <algorithm>
#include "stimulus.h"
#include "cache.h"
#include "mem_block_pool.h"

using namespace vortex;

static_assert(VX_CFG_MEM_BLOCK_SIZE == lazy_fair::sector_bytes);

class CachePorts {
public:
  CachePorts() {
    Cache::Config config{};
    config.size = 2048;
    config.L = config.S = config.W = log2ceil(lazy_fair::sector_bytes);
    config.num_ways = 1;
    config.num_inputs = config.mem_ports = 1;
    config.addr_width = 32;
    config.write_back = config.is_llc = config.lazy_fetch = true;
    config.mshr_size = 16;
    config.latency = 2;
    cache_ = Cache::Create("cache", config);
    SimPlatform::instance().reset();
  }

  bool request(bool store, unsigned sector, uint8_t data) {
    auto& in = cache_->core_req_in.front();
    if (in.full()) {
      return false;
    }
    auto block = make_mem_block();
    block->fill(data);
    in.send(MemReq(store ? MemOp::ST : MemOp::LD, sector * lazy_fair::sector_bytes,
                   block, ~uint64_t{0}, sector));
    return true;
  }

  bool respond(unsigned tag, uint8_t data) {
    auto& in = cache_->mem_rsp_in.front();
    if (in.full()) {
      return false;
    }
    auto block = make_mem_block();
    block->fill(data);
    in.send(MemRsp(tag, 0, 0, block));
    return true;
  }

  bool memory_request(unsigned& tag, unsigned& sector) {
    auto& out = cache_->mem_req_out.front();
    if (out.empty()) {
      return false;
    }
    auto& req = out.peek();
    if (req.is_write()) {
      lazy_fair::fail("unexpected memory write");
    }
    tag = req.tag;
    sector = req.addr / lazy_fair::sector_bytes;
    out.pop();
    return true;
  }

  bool core_response(unsigned& tag, uint8_t& data) {
    auto& out = cache_->core_rsp_out.front();
    if (out.empty()) {
      return false;
    }
    auto& rsp = out.peek();
    tag = rsp.tag;
    if (!rsp.data) {
      lazy_fair::fail("missing response data");
    }
    data = rsp.data->front();
    if (!std::all_of(rsp.data->begin(), rsp.data->end(),
                     [data](uint8_t byte) { return byte == data; })) {
      lazy_fair::fail("inconsistent bytes in response sector");
    }
    out.pop();
    return true;
  }

  void tick() { SimPlatform::instance().tick(); }

private:
  Cache::Ptr cache_;
};

int main() {
  CachePorts cache;
  return lazy_fair::run(cache);
}
