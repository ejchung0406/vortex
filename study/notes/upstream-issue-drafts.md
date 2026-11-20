# Upstream issue drafts (vortexgpgpu/vortex)

Checked against upstream `master` at ccf811e11 (2026-11-20): none of the files
below changed since our merge base, so all three still apply. Fixes are on the
fork branch `ejchung0406/vortex:feature/dxa-borrowing`.

---

## 1. simx: dcache can return a younger store's data to an older load of the same word

**Summary.** In SimX, a load that misses the L1 dcache and is replayed after
the fill can be overtaken by a younger store from the same thread to the same
word. The load then returns the store's data. The RTL keeps the order
(`creq_hit_order_hazard_st0` in `hw/rtl/cache/VX_cache_bank.sv`), so this is a
SimX/RTL divergence and a functional bug in SimX.

**How it shows up.** Wrong kernel results, timing dependent. We hit it in a
tensor-core GEMM whose loop reloads a stack spill slot and then reuses the
slot: a few outputs off by 1-3%, different ones from run to run with code
layout.

**Mechanism** (`sim/simx/mem/cache.cpp`, traced in one bank):
1. Two loads of a cold line miss and chain in the MSHR.
2. The fill arrives; the first entries are answered by fill-forward.
3. A younger store to the same word is admitted. It hits, so it does not join
   the chain; ordering is left to the MSHR lookup at the pipe exit.
4. The older load is dequeued for replay and enters the pipe behind the store.
5. At the store's exit the MSHR no longer holds the load, so the store merges
   into the line. The replayed load reads the store's data.

Write-back caches have the same hole.

**Reproducer.** `tests/regression/war_order` on the fork: each thread does
load, load, store, load, store, store, load on its own word of a cold line and
checks every value. On a 64-warp x 32-thread configuration, or 16 x 16 with the
default write-back dcache, it fails on `master` (e.g. `Found 125 errors`) and
passes with the fix. It does not fail with the default 4 x 4 configuration:
too few warps to interleave.

**Fix.** Commits dab77b7f2 and 51a4cce00 on the fork: decide at admission, as
the RTL does at S0. A load joins the chain behind an undrained same-word
write; a store behind any undrained same-word entry; a write-through store
queues its line merge at admission and still sends its memory write and
response at the exit. Cycle counts of kernels that never hit the hazard are
unchanged.

**Not verified.** That the RTL passes `war_order` under rtlsim (read from the
RTL, not run).

---

## 2. simx: a fill-forward head is replayed when the bank's core-response queue is full; the RTL holds it

**Summary.** When a forwarded fill finds the bank's 2-entry core-response
queue full, the RTL holds the forward head and waits (`VX_cache_bank.sv`,
`fwd_head`). SimX instead replays that request through the bank pipeline. Under
streaming loads this costs SimX about a third of an SM's load throughput.

**Evidence.** `bwstream -n64 -r1 -p0 -u4 -b32`, one core, 64 warps x 32
threads, 4 dcache banks: rtlsim 20,570 cycles; SimX 25,824 as is, 20,266 when
it holds.

**Fix.** Commit cbc696a98 on the fork adds the hold behind
`VX_CFG_DCACHE_FWD_HOLD` (default 0 = old behaviour) and makes SimX model
`VX_CFG_DCACHE_CRSQ_SIZE`. Happy to make the hold unconditional if preferred.

---

## 3. runtime (simx, rtlsim): a device address can alias a command-processor host region

**Summary.** `host_mem_alloc` returns the raw host pointer as the CP address,
and the CP's dram hooks treat any address that falls inside a registered host
region as host memory. A device address can equal a host heap address. Then a
DMA chunk meant for device memory is copied into the host heap instead.

**How it shows up.** Uploading one ~40 MB buffer on a 1-core configuration:
the chunk at device address 0x2070000 fell inside the command ring's
allocation, the copy ran past it, corrupted the region map and the run
segfaulted in `make_cp_hooks` (dram_read). Without a crash the effect is
silent: 64 KB missing from device memory, or host bytes returned by a download.

**Reproducer.** `gemm_block -m16 -n5120 -k4096 -b1 -s1`, one core (fork test);
any single upload of that size should do.

**Fix.** Commits b974a9e98 (simx) and 51a4cce00 (rtlsim): tag host CP addresses
with bit 62 and check the whole access range against the region. The foldsim
and firesim runtimes have the same lookup and are not changed.
