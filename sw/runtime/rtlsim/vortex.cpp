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

// ============================================================================
// rtlsim backend — a pure transport HAL (see callbacks.h). It exposes only:
//   * device lifecycle      — init() / ~vx_device()
//   * CP register channel   — cp_reg_read / cp_reg_write
//   * CP-visible host memory — host_mem_alloc / host_mem_free
//
// Device-memory allocation and caps decoding live in the common core; the
// Command Processor is the sole memory engine. rtlsim has unified memory, so
// host memory is a plain process allocation: the software CommandProcessor's
// dram hooks dereference it directly (the sim runs in-process).
// ============================================================================

#include <VX_types.h>
#include <common.h>

#include <mem.h>
#include <util.h>
#include <processor.h>
#include <cmd_processor.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <cassert>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <chrono>

using namespace vortex;

class vx_device {
public:
  vx_device()
    : ram_(0, RAM_PAGE_SIZE)
    , cp_(make_cp_hooks())
  {
    ram_.enable_acl(false);   // the common-core allocator owns the address map
    processor_.attach_ram(&ram_);
  }

  ~vx_device() {
    if (future_.valid()) {
      future_.wait();
    }
    for (auto& kv : host_regions_)
      free(reinterpret_cast<void*>(kv.first));
    host_regions_.clear();
  }

  int init() {
    // Virtual memory lives wholly in the common core (vx::Device owns the
    // VMManager; the CP model translates) — nothing backend-side here.
    return 0;
  }

  // ----- CP register channel -----
  // rtlsim has no hardware CP; the regfile surface is provided by a
  // functional CommandProcessor C++ model. A bounded tick burst around
  // each MMIO transaction keeps the CP responsive without a dedicated
  // simulation thread.
  int cp_reg_write(uint32_t off, uint32_t value) {
    cp_.mmio_write(off, value);
    for (int i = 0; i < 256 && cp_.busy(); ++i) cp_.tick();
    return 0;
  }
  int cp_reg_read(uint32_t off, uint32_t* value) {
    for (int i = 0; i < 256 && cp_.busy(); ++i) cp_.tick();
    *value = cp_.mmio_read(off);
    return 0;
  }

  // ----- CP-visible host memory (command ring + DMA staging) -----
  // Unified memory: a plain process allocation. cp_addr is the pointer
  // tagged with kHostTag, a bit no device address has, so the CP model's
  // dram hooks tell a host region from device memory by the address alone:
  // an untagged pointer value can equal a device address.
  static constexpr uint64_t kHostTag = uint64_t(1) << 62;
  static_assert(VX_CFG_MEM_ADDR_WIDTH < 62, "device addresses must leave the host tag bit clear");

  int host_mem_alloc(uint64_t size, void** host_ptr, uint64_t* cp_addr) {
    uint64_t asize = aligned_size(size, CACHE_BLOCK_SIZE);
    void* ptr = aligned_alloc(CACHE_BLOCK_SIZE, asize);
    if (ptr == nullptr)
      return -1;
    assert((reinterpret_cast<uint64_t>(ptr) & kHostTag) == 0);
    std::lock_guard<std::mutex> g(host_mu_);
    host_regions_[reinterpret_cast<uint64_t>(ptr)] = asize;
    *host_ptr = ptr;
    *cp_addr  = reinterpret_cast<uint64_t>(ptr) | kHostTag;
    return 0;
  }

  int host_mem_free(uint64_t cp_addr) {
    if ((cp_addr & kHostTag) == 0)
      return -1;
    uint64_t ptr = cp_addr & ~kHostTag;
    {
      std::lock_guard<std::mutex> g(host_mu_);
      auto it = host_regions_.find(ptr);
      if (it == host_regions_.end())
        return -1;
      host_regions_.erase(it);
    }
    free(reinterpret_cast<void*>(ptr));
    return 0;
  }

  // Host memory is the process's own memory: always coherent.
  int host_mem_pull(uint64_t /*cp_addr*/) {
    return 0;
  }

  int host_mem_push(uint64_t /*cp_addr*/) {
    return 0;
  }


private:
  // If `addr` is a tagged host address, return its host pointer, checking
  // that `bytes` from it lie in one registered region; otherwise nullptr →
  // device memory.
  void* host_region_ptr(uint64_t addr, std::size_t bytes) {
    if ((addr & kHostTag) == 0)
      return nullptr;
    uint64_t ptr = addr & ~kHostTag;
    std::lock_guard<std::mutex> g(host_mu_);
    auto it = host_regions_.upper_bound(ptr);
    if (it == host_regions_.begin()) {
      std::cerr << "*** CP access to an unregistered host address 0x" << std::hex << ptr << std::dec << std::endl;
      std::abort();
    }
    --it;
    if (ptr < it->first || ptr + bytes > it->first + it->second) {
      std::cerr << "*** CP access [0x" << std::hex << ptr << ", +0x" << bytes
                << ") outside host region [0x" << it->first << ", +0x" << it->second << ")" << std::dec << std::endl;
      std::abort();
    }
    return reinterpret_cast<void*>(ptr);
  }

  vortex::CommandProcessor::Hooks make_cp_hooks() {
    vortex::CommandProcessor::Hooks h;
    h.dram_read = [this](uint64_t addr, void* dst, std::size_t bytes) {
      if (void* hp = host_region_ptr(addr, bytes)) {
        std::memcpy(dst, hp, bytes);
        return;
      }
      if (processor_.cp_llc_client()) {
        if (future_.valid()) {
          future_.wait();
        }
        processor_.cp_mem_read(addr, dst, bytes);
        return;
      }
      ram_.read(static_cast<uint8_t*>(dst), addr, bytes);
    };
    h.dram_write = [this](uint64_t addr, const void* src, std::size_t bytes) {
      if (void* hp = host_region_ptr(addr, bytes)) {
        std::memcpy(hp, src, bytes);
        return;
      }
      if (processor_.cp_llc_client()) {
        if (future_.valid()) {
          future_.wait();
        }
        processor_.cp_mem_write(addr, src, bytes, false);
        return;
      }
      ram_.write(static_cast<const uint8_t*>(src), addr, bytes);
    };
    if (processor_.cp_llc_client()) {
      // The CP DMA is a client of the last-level cache: transfers stay
      // coherent with it, and those of at least half its size write around it.
      h.dma_write = [this](uint64_t addr, const void* src, std::size_t bytes, bool no_alloc) {
        if (void* hp = host_region_ptr(addr, bytes)) {
          std::memcpy(hp, src, bytes);
          return;
        }
        if (future_.valid()) {
          future_.wait();
        }
        processor_.cp_mem_write(addr, src, bytes, no_alloc);
      };
      h.dma_llc_client = true;
      h.dma_no_alloc_min = processor_.cp_llc_size() / 2;
      if (const char* v = std::getenv("VORTEX_CP_NO_ALLOC_MIN")) {
        h.dma_no_alloc_min = std::strtoull(v, nullptr, 0);
      }
    }
    h.vortex_dcr_write = [this](uint32_t addr, uint32_t value) {
      // Wait for any background processor_.run() to finish so dcr_write
      // does not race the Verilator state.
      if (future_.valid()) {
        future_.wait();
      }
      processor_.dcr_write(addr, value);
    };
    h.vortex_dcr_read = [this](uint32_t addr, uint32_t tag) -> uint32_t {
      // Wait for any background processor_.run() to finish so dcr_read
      // does not race the Verilator state.
      if (future_.valid()) future_.wait();
      uint32_t v = 0;
      processor_.dcr_read(addr, tag, &v);
      return v;
    };
    h.vortex_start = [this]() {
      future_ = std::async(std::launch::async, [&] { processor_.run(); });
    };
    h.vortex_busy = [this]() -> bool {
      if (!future_.valid()) return false;
      return future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    };
    h.vortex_drain = [this]() {
      if (future_.valid()) future_.wait();
      processor_.drain();
    };
#ifdef VX_CFG_VM_ENABLE
    // The device MMU control surface answers the fault-report DCRs.
    h.mmu_fault_report = true;
#endif
    return h;
  }

  RAM                 ram_;
  Processor           processor_;
  std::future<void>   future_;
  vortex::CommandProcessor cp_;
  std::mutex          host_mu_;
  std::map<uint64_t, uint64_t> host_regions_;   // base -> size
};

#include <callbacks.inc>
