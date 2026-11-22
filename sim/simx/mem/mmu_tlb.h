// Copyright © 2019-2025
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include <VX_config.h>

#ifdef VX_CFG_VM_ENABLE

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace vortex {

// Per-core TLB. Small fully-associative CAM of {vpn → ppn} translations
// with MRU-style eviction. Tracks MMU perf counters (VX_DCR_MPM_CLASS_MEM).
class Tlb {
public:
  explicit Tlb(uint32_t size = VX_CFG_TLB_SIZE);

  struct Result {
    bool     hit = false;
    uint64_t ppn = 0;     // 4KB-granule translation (superpage entries
                          // splice the vpn low bits back in)
    uint8_t  flags = 0;
    uint8_t  level = 0;
  };

  // Increments `reads_` on every call and `hits_` on a successful lookup.
  Result lookup(uint64_t vpn);

  // Install a new translation at the given page level (0 = base page,
  // 1 = mega, 2 = giga); one superpage entry covers its whole range.
  // Evicts a non-MRU entry when the TLB is full; updates `evictions_`
  // if the chosen slot was previously valid.
  void fill(uint64_t vpn, uint64_t ppn, uint8_t flags, uint8_t level);

  // Invalidate every entry (sfence.vma equivalent).
  void flush();

  uint64_t reads()     const { return reads_; }
  uint64_t hits()      const { return hits_; }
  uint64_t misses()    const { return misses_; }
  uint64_t evictions() const { return evictions_; }

private:
  struct Entry {
    bool     valid = false;
    bool     mru   = false;
    uint64_t vpn   = 0;
    uint64_t ppn   = 0;   // aligned down to the entry's level
    uint8_t  flags = 0;
    uint8_t  level = 0;
  };

  // Linear flat array; small enough (typ. 32 entries) for a per-cycle
  // linear scan to model CAM lookup behavior.
  std::vector<Entry> entries_;
  // The valid entries indexed by level, then by the vpn bits above the
  // level's page offset (the bits a lookup compares), each key listing its
  // entries in ascending index order: a lookup probes one key per level.
  struct level_index_t {
    uint32_t count = 0;  // valid entries at this level
    std::unordered_map<uint64_t, std::vector<uint32_t>> keys;
  };
  std::vector<level_index_t> index_;
  void index_insert(uint32_t id);
  void index_erase(uint32_t id);

  uint64_t reads_     = 0;
  uint64_t hits_      = 0;
  uint64_t misses_    = 0;
  uint64_t evictions_ = 0;
};

} // namespace vortex

#endif // VX_CFG_VM_ENABLE
