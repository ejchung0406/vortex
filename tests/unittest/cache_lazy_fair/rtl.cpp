#include "stimulus.h"
#include "VVX_cache_top.h"

class CachePorts {
public:
  CachePorts() {
    dut_.clk = 0;
    dut_.core_req_valid[0] = 0;
    dut_.mem_rsp_valid[0] = 0;
    dut_.core_rsp_ready[0] = 1;
    dut_.mem_req_ready[0] = 1;
    dut_.reset = 1;
    tick();
    tick();
    dut_.reset = 0;
  }
  ~CachePorts() { dut_.final(); }

  bool request(bool store, unsigned sector, uint8_t data) {
    dut_.core_req_valid[0] = 1;
    dut_.core_req_rw[0] = store;
    dut_.core_req_addr[0] = sector;
    dut_.core_req_byteen[0] = ~uint64_t{0};
    dut_.core_req_attr[0] = 0;
    dut_.core_req_tag[0] = sector;
    for (unsigned i = 0; i < lazy_fair::sector_bytes / 4; ++i) {
      dut_.core_req_data[0][i] = 0x01010101u * data;
    }
    dut_.eval();
    return dut_.core_req_ready[0];
  }

  bool respond(unsigned tag, uint8_t data) {
    dut_.mem_rsp_valid[0] = 1;
    dut_.mem_rsp_tag[0] = tag;
    for (unsigned i = 0; i < lazy_fair::sector_bytes / 4; ++i) {
      dut_.mem_rsp_data[0][i] = 0x01010101u * data;
    }
    dut_.eval();
    return dut_.mem_rsp_ready[0];
  }

  bool memory_request(unsigned& tag, unsigned& sector) {
    dut_.eval();
    if (!dut_.mem_req_valid[0]) {
      return false;
    }
    if (dut_.mem_req_rw[0]) {
      lazy_fair::fail("unexpected memory write");
    }
    tag = dut_.mem_req_tag[0];
    sector = dut_.mem_req_addr[0];
    return true;
  }

  bool core_response(unsigned& tag, uint8_t& data) {
    if (!dut_.core_rsp_valid[0]) {
      return false;
    }
    tag = dut_.core_rsp_tag[0];
    data = dut_.core_rsp_data[0][0];
    for (unsigned i = 0; i < lazy_fair::sector_bytes / 4; ++i) {
      if (dut_.core_rsp_data[0][i] != 0x01010101u * data) {
        lazy_fair::fail("inconsistent bytes in response sector");
      }
    }
    return true;
  }

  void tick() {
    dut_.eval();
    dut_.clk = 1;
    dut_.eval();
    dut_.clk = 0;
    dut_.core_req_valid[0] = 0;
    dut_.mem_rsp_valid[0] = 0;
    dut_.eval();
  }

private:
  VVX_cache_top dut_;
};

int main(int argc, char** argv) {
  Verilated::commandArgs(argc, argv);
  CachePorts cache;
  return lazy_fair::run(cache);
}
