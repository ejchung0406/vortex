# Upstream PR candidates

Things found on the fork (`feature/dxa-borrowing`) that could go to upstream
Vortex, one PR per topic, once each is verified. Nothing here has been sent.
Numbers are SimX unless noted; `h200s` is one sixth of an H200 (22 SMs, one
HBM3e stack, 800 GB/s). Last updated 2026-11-20.

Status words: **idea** (not implemented), **experimental** (works in a throwaway
copy, behind build macros), **on fork** (committed on the fork branch),
**verified** (measured against its acceptance test), **ready** (clean code,
reviewed, both simulators).

| # | Topic | Kind | Status | Left before sending |
|---|---|---|---|---|
| 1 | DRAM model: HBM3E type, scheduler window, bank groups, 1 KB interleave | feature | on fork (299605e51), SimX + rtlsim | row timings are placeholders; review |
| 2 | Streaming test with a CUDA-shaped access pattern (`bwstream -b`) | test | on fork (601a1d482) | short README |
| 3 | Decode GEMM test `gemm_block` (load and DXA staging, two roles, rates) | test | on fork (3d21214d2..69e11596b), passes | CI case |
| 4 | DXA dispatch prefers an idle worker | fix, SimX/RTL parity | on fork, RTL b0adb2663; rtlsim 66,067 -> 60,307 | review |
| 5 | DXA engine on several memory ports | feature | SimX only | dropped for now: 8 workers x 1 port chosen |
| 6 | Core limits that hold the compute roof | config | profile values; rtlsim 118,350 -> 35,006 (SimX 118,418 -> 33,419) | defaults vs profile-only |
| 7 | Smaller findings | notes | see below | |
| 8 | **DXA workers per SM scale with the number of SMs simulated** | **bug** (config + stale doc) | found; profile works around it | report; pick the per-SM count |
| 9 | **SimX dcache returns a younger store's data to an older load** | **bug** (wrong results) | fixed on fork dab77b7f2 + 51a4cce00, test `war_order`, reviewed twice | run `war_order` on rtlsim; CI case |
| 10 | **SimX replays a forwarded fill on a full response queue; RTL holds** | **bug** (SimX/RTL divergence, timing) | fixed on fork cbc696a98 behind a knob; rtlsim 20,570 vs SimX 20,266 / 25,824 | make it the default |
| 11 | **Runtime: device address can alias a CP host region** | **bug** (crash or silent corruption) | fixed on fork b974a9e98 (simx), 51a4cce00 (rtlsim) | foldsim, firesim |
| 12 | **DXA memory tag too narrow for more in-flight slots** | **bug** (RTL does not elaborate) | fixed on fork 348263623, rtlsim builds and passes | review |
| 13 | **rtlsim stops at time 0 on H100-shaped profiles with WGMMA** (`VX_tcu_bbuf.sv` wants LMEM banks 32, profiles set 16) | **bug** (RTL/profile mismatch) | found; rtlsim runs disable WGMMA | fix comes with 14 |
| 14 | wgmma throughput: one shared B buffer puts the four tensor blocks in lockstep | performance | SimX patch `wgmma_tbuf.patch` (340 -> 1,326..2,989 FLOPs/cycle); not on fork | port count decision; RTL |
| 15 | FIFO L2 makes disjoint SMs slow each other | calibration | measured; pseudo-LRU removes it | decision |
| 16 | L1 dcache and shared memory both at full size (484 KB vs the part's 256 KB pool) | profile accuracy | found, in every profile from Volta on | decide the split |

Issue texts in English for 9, 10, 11: `upstream-issue-drafts.md`. Upstream `master` checked at ccf811e11 (2026-11-20): the affected files are unchanged there.

---

## 1. DRAM model

**Problem.** With the stock model the memory delivers far less than its nominal
bandwidth once many SMs read at once, and a GEMM slows down per SM as SMs are
added. On the quarter-H200 shape 33 SMs streaming reached 16-22% of the peak;
it only reached the peak after the channel count was raised eightfold, which is
not the part. A decode GEMM took +33% kernel time at 11 SMs and +113% at 22.

**Causes found, each measured on `h200s`.**

| Cause | Stock | Changed to | Evidence |
|---|---|---|---|
| Channel shape | one 128-bit channel per pair of HBM3 channels, 9375 MT/s | 64-bit channel, two 32-bit pseudo-channels, 32 B burst = BL8, 6250 MT/s (new type `HBM3E`) | JESD238B.01: 64 DQ per channel, 256-bit prefetch per PC, BL8 |
| Scheduler window | Ramulator's `ReqBuffer::max_size = 32` per channel, with an unbounded FIFO in front | 256 | 22-SM stream: 382 GB/s at 32, 588 at 128, 621 at 256, 636 at 512. At 32, 74% of enqueue attempts were refused and a row activation served 77 B. Little's law at 45 GB/s per channel and ~72 ns residency gives ~100 entries. Accel-Sim uses 64 (QV100/A100) and 128 (H100/H200) |
| Banks | 16 per pseudo-channel, 4 groups | 32 per pseudo-channel, 8 groups of 4 | JESD238B.01 Table 4/5: 16 x (stack-ID count) banks per PC; a 24 Gb x 8-high stack (12 Gb per channel) has 32. 22-SM stream at window 32: 382 -> 522 GB/s; 64 banks no further gain |
| Same-group read spacing | 4 model clocks (1.28 ns) | tCCDL = max(4 nCK, 2.5 ns) = 2.56 ns | JESD238B.01 Table 93. Effect measured: under 2% |
| Channel interleave | one 64 B line per channel, in order; 128 B per bank visit | 1 KB per channel, channel and bank chosen together by a hash of the address bits above them | see below |

**Interleave.** A 1 KB unit puts a contiguous 1 KB in one row of one bank.
Without mixing all streams land on one channel at a time (11-SM stream 425 ->
200 GB/s). Mixing channel and bank separately used half the banks. A joint mix
(`(chunk ^ hash(chunk >> bits)) & mask`, a bijection per block of
channels x banks KB) uses all banks; figure `mapping_distribution.png`. The
1 KB unit itself comes from someone's reading of the GPU's driver, not from a
document; Accel-Sim uses 256 B (`dramid@8`) with a hashed channel index. Call
it a calibrated value in a PR.

**Results with window 256, 32 banks, tCCDL per spec (`h200s`).**

Streaming, 22 SMs, GB/s (peak 800; a real H200 reaches 90% with every SM):

| Interleave | warp reads its own contiguous stream | CUDA-shaped (item 2) |
|---|---|---|
| 64 B (stock mapping) | 660 | 493 |
| 256 B + joint mix | 643 | 706 |
| 1 KB + joint mix | 408 | 708 |

Decode GEMM kernel time against 1 SM (`gemm_decode -m16 -k4096`, 1024 columns per SM):

| Interleave | 11 SMs | 22 SMs |
|---|---|---|
| stock mapping, stock window and banks | +33% | +113% |
| stock mapping, window 256, 32 banks | +24..26% | pending |
| 256 B + joint mix | +5.8% | pending |
| 1 KB + joint mix | +2.4% | pending (16 banks, window 512: +24%) |

**Not settled.**
- 22-SM GEMM with the final values.
- Row timings (tRCD, tRP, tRAS, tRC, tRRD, tFAW) are vendor data-sheet values,
  blank in the standard. The model uses 14/14/33/47/4/15 ns. Accel-Sim's H200
  uses about 6/6/14.5/16/1.9 ns; macsim_turbo 42 ns per row switch and tFAW 8.
  Sensitivity not measured yet.
- Ramulator issues one command per clock on a channel; the standard has
  separate row and column command buses. With the model clock at twice CK the
  slot count matches, so the effect should be small. Not measured (the in-house
  bank model has the split but also a different scheduler: 300-428 GB/s on the
  same runs, so it does not isolate this).
- An SM streams at most ~45 GB/s (a real one 107): its outstanding-miss limits
  (128 lines) bound it. Separate from the DRAM model but shows in the same plots.

**What a PR needs.**
- A controller with a configurable window in `third_party/ramulator_ext` (the
  submodule's `ReqBuffer` size is a constant), plus `VX_CFG_PLATFORM_MEMORY_*`
  knobs for the window, banks per PC and the interleave unit.
- The interleave inside `sim/common/dram_sim` so SimX and rtlsim share it; today
  the experiment patches `sim/simx/mem/memory.cpp` only.
- Existing profiles keep their behaviour unless they opt in; `h100`/`h200` then
  need their own before/after numbers.
- Unit tests in `tests/unittest/dram_sim` for the mapping (bijection, balance)
  and the window.
- Likely three PRs: (a) `HBM3E` channel type and bank count, (b) scheduler
  window, (c) interleave unit and mixing.

**Where things are.** Type `HBM3E` and profile `h200s`: uncommitted in the
working tree. Window, banks, tCCDL, interleave: `vortex-exp/src_rq` behind
`EXP_GRAN`, `EXP_VISIT`, `EXP_JOINT`, `EXP_LBG`, `EXP_CCDL` and the `EXP_RQ`
environment variable. Results: `vortex-exp/results/{s1,gr,bk,rq,fin,blk}_*`.
The standard's PDF is licensed to the user and must not be committed.

---

## 2. Streaming test with a CUDA-shaped access pattern

**Problem.** `bwstream` gave each warp its own contiguous stream. A CUDA kernel
with a 1024-thread block has neighbouring warps read neighbouring 128 B pieces
at the same moment, and a warp's successive loads 4 KB apart. The two orders
rank address mappings oppositely (table in item 1), so a mapping judged with the
first order is judged wrongly.

**Change.** `-b W`: W consecutive one-warp CTAs share a slice as the warps of a
W x 32 thread block would. `-b 1` is the old behaviour. Files:
`tests/regression/bwstream/{common.h,kernel.cpp,main.cpp}`.

**Verified.** Passes on `h200s` at 2, 11, 22 SMs with `-b 32 -u 4` and `-u 8`.
With the 1 KB mapping a row activation serves 1023 B at 2 SMs, 589 B at 11,
177 B at 22.

**Note.** `bwstream`, `bwstream_dxa`, `corun`, `gemm_decode` are all the fork's
own tests; a PR would introduce the test itself, not only the option.

---

## 3. Decode GEMM test: cooperative load and TMA staging

**What exists upstream.** `sgemm2_tcu` (CTA of several warps copies each tile
into shared memory), `sgemm_tcu_wg` (wgmma), `sgemm_tcu_wg_dxa` (wgmma + DXA)
and others. They are general GEMMs: the CTA's warps are laid along M and one
tensor-core tile is staged per K step. At the decode shape they do not fit:
`sgemm2_tcu -m16` and `sgemm_tcu_wg_dxa -m16` refuse M ("must be a multiple of
cta_M" = 32 / 64), and `sgemm_tcu_wg -m16 -n1024 -k4096` passes but takes 20.0 M
cycles on one SM against 0.66 M for the tests below.

**Change.** `tests/regression/gemm_block` (uncommitted): the CTA's warps are
laid along N, the weights are staged a K chunk at a time, `-s 0` with coalesced
loads by the CTA's warps, `-s 1` with the DXA engine and `-d` chunks
outstanding; `-w` warps per CTA, `-c` K elements per chunk. Same reference
check and in-kernel timing as `gemm_decode` (the fork's one-warp-per-CTA test).

**Verified.** PASSED in both modes on `h200s` at 1 SM (`-m16 -n1024 -k4096`)
and at 2 SMs (small size). 1 SM, target memory configuration, FP16:

| Kernel | Kernel cycles | GB/s per SM | Real H200 |
|---|---|---|---|
| `gemm_decode` | 657,166 | 25.3 | |
| `gemm_block -s0`, 4 KB / 8 KB chunks | 1,037,833 / 972,423 | 16.0 / 17.1 | FP16 cuBLAS 16-20 |
| `gemm_block -s1`, 4 KB x depth 2 | 665,059 | 25.0 | FP8 TMA+wgmma 29-35 |

The load mode executes 2.7x the instructions of `gemm_decode` (the copy).
These DXA numbers used 16 engines per SM; see item 8.

**Open.** 11 and 22 SMs (running). Whether to offer it upstream as a decode
variant of the existing tests rather than a new test, and whether it should use
wgmma as `sgemm_tcu_wg_dxa` does.

---

## 4. DXA dispatch prefers an idle worker

**Problem.** A DXA worker reports ready when its setup stage is idle whether or
not a transfer runs on it (`hw/rtl/dxa/VX_dxa_setup.sv:84`), and the dispatch
takes the lowest-numbered ready worker, so a request waits behind a running
transfer while other workers idle. 16 workers, 64 transfers outstanding: 5.3
workers running on average, delivery 60-67 GB/s per SM.

**Change.** SimX commit `db8fde8f5`: idle workers take requests first. Delivery
90 GB/s per SM (H200 measured ~101).

**Before a PR.** The same rule in `VX_dxa_dispatch` (needs each worker's
transfer-active out of `VX_dxa_worker`), and an engine-bound rtlsim point that
moves from the old SimX value to the new one. Baseline rtlsim runs
(`par2_rtlsim_w4`, `_w16`) are still running; SimX gives 49,105 and 41,140
cycles on the old profile. Until then SimX and RTL differ for more than one
worker.

---

## 5. DXA engine on several memory ports

**Problem.** `dxa_gmem_bus_if[1]`: the engine has one of the socket's four ports
to the L2. Four workers deliver 62 GB/s per SM with one port and 90 with four.

**Change.** SimX commit `89fd86725`, `VX_CFG_DXA_GMEM_PORTS` (default 1).

**Before a PR.** RTL counterpart; area.

---

## 6. Core limits that hold the compute roof

Register-only multiply-add kernel, 64 warps, issue width 4 (`corun -b0 -i4096 -g128`):

| Limit | Derived value on the H100 shapes | Set to | IPC |
|---|---|---|---|
| (stock) | | | 0.86 |
| FPU operations in flight per block, `VX_CFG_FPU_QUEUE_SIZE` | 2 | 16 | 2.18 |
| Operations between issue and a unit, `VX_CFG_DISPATCH_QUEUE_SIZE` | 4 | 8 | 2.96 |
| Register-file banks, `VX_CFG_NUM_GPR_BANKS` | 4 | 8 | 3.30 |

The fork sets these in `h200q`/`h200s` only (commits `e113ec0df`, `a7b9d8a9d`).
For upstream the question is whether the derivation of `FPU_QUEUE_SIZE`
(`2 * SIMD_WIDTH / NUM_FPU_LANES`) should account for the FMA latency, and
whether `h100`/`h200` should carry the values. RTL cost of the three not
measured. Ruled out by measurement: operand collectors, instruction buffer,
icache banks, integer lanes.

---

## 7. Smaller findings (notes in `vortex-design-inefficiencies.md`)

- FP operations of one warp are serialized by the FCSR rule
  (`sim/simx/scoreboard.cpp:63`): writers of the sticky flags wait for each
  other. One warp 93,864 -> 83,623 cycles without the write-write part; no
  effect at 64 warps. Low priority.
- The `memory: lat=` perf counter wraps on large runs.
- TLB reach: a decode GEMM over 128 MiB of weights needs `DTLB_SIZE` and
  `L2_TLB_MEGA_SIZE` of 256 with 2 MB pages (34% hit rate with 64), and the
  runs need `VORTEX_VM_MEGAPAGE_HEAP=1`.
- The debug build of SimX does not compile (`__assert` clash in
  `sim/common/dram_sim.cpp`).

---

## 8. DXA engines per SM scale with the number of SMs simulated (bug report)

**What.** `VX_config.toml`:

```
[dxa]
# Cluster-level DXA engine count, decoupled from cores/socket size.
VX_CFG_NUM_DXA_CORES = "expr: max(1, up($VX_CFG_NUM_CORES / 8))"
```

The formula entered with the DXA (commit `70ff16385`, 2026-02-21) when the
engines were a cluster-level pool: one engine per eight cores. Commit
`7d6950de5` (2026-07-11, "relocate TEX/RTU/DXA into the socket") moved the DXA
core into every socket (`hw/rtl/VX_socket.sv`, `sim/simx/socket.cpp:211`) and
did not touch `VX_config.toml`. `VX_CFG_NUM_DXA_CORES` is now the number of
workers inside each socket's DXA core, still derived from the total core count.

**Effect.** No profile sets the value (`h100` only enables the extension), so
with one core per socket an SM has 1 worker in a 1- to 8-SM run, 2 in a 22-SM
run (seen in a run's resolved configuration) and 16 at the H100's 132 SMs. An
SM's TMA capacity therefore depends on how many SMs the run has, which breaks
any SM-count scaling experiment, and the comment describes a structure that no
longer exists. `VX_CFG_DXA_MAX_INFLIGHT` defaults to 8 alongside it.

**Proposed.** A per-socket count independent of `NUM_CORES`, and a corrected
comment. For the value: a real H200 SM delivers about 29 GB/s with one 16 KB
transfer outstanding, 56 with two, 101 with four, and no more with eight (4 KB
tiles: 27 at four, 29 at eight), which is what four engines that each hold one
transfer for a memory round trip would give (16 KB / (256 + 660 cycles) =
35 GB/s each). Being measured on `h200s`: 3, 4 and 8 workers with 1, 2 and 4
memory ports (`eng_*`).

**History of the misreading.** Two days earlier this fork's notes called the
DXA "a cluster-level shared pool, Virgo-style" from this comment and formula
without checking where the core is instantiated. That was wrong; the baseline
"one TMA block per SM, not shared" is what the code has had since July.


---

# Added 2026-11-20 (items 9-16)

## 9. SimX dcache: a younger store overtakes an older load of the same word (bug, wrong results)
- **Symptom**: `gemm_block` with 16 tiles (M=256), DXA staging: 1-30 of 262,144 outputs off by 1-3%, different ones as the code layout changes. Errors under the host check's 1% tolerance pass silently.
- **Cause** (`sim/simx/mem/cache.cpp`): a store that hits skips the MSHR chain at admission and is ordered only by an MSHR lookup at the pipe exit. An older same-word load that has left the chain for replay is invisible there, so the store merges first and the replayed load returns its data. Write-back caches have the same hole.
- **RTL**: orders at S0 (`creq_hit_order_hazard_st0`, `VX_cache_bank.sv:716-738`; `VX_cache_mshr.sv:524-538`). Read, not run.
- **How it was found**: a shadow checker in the LSU that replays every stack store in program order and compares each load: 8 bad lanes in 49.7 M checks before, 0 after. Cycle-level trace of one event in the agent report (bank 1, line 0x1fefc2b40, cycles 4147016-4147241).
- **Fix**: decide at admission like the RTL. First attempt (dab77b7f2) deferred at the exit and opened a new hole (a younger load could miss the deferred store's bytes) — caught by independent review and confirmed by a directed test; reworked in 51a4cce00.
- **Test** `tests/regression/war_order`: 30-point sweep, failures before / first fix / final: h200s 7 / 3 / 0; 16x16 write-back config 3 / 3 / 0. Does not fail on the default 4x4 config.
- **Effect on results**: one-tile kernels unchanged to the cycle (1,079,840 / 677,085); the runs of this study did not hit the hazard (cycle counts identical before and after).
- **Review follow-ups**: LR/SC reservation not invalidated by a write-through merge replay (cache.cpp ~1995); loads demote on any-word writes where the RTL is per word; `war_order` not in `ci/testcases`.

## 10. SimX dcache: forwarded fill replayed on a full response queue (bug, timing)
- RTL holds the fill-forward head when the bank's 2-entry core-response queue is full (`VX_cache_bank.sv:1356-1360`); SimX replayed it through the bank.
- rtlsim parity, `bwstream -n64 -b32`, h200s, 1 SM: rtlsim 20,570; SimX 25,824 (replay), 20,266 (hold).
- Effect, plain-load streaming, block shape: 51 -> 77 GB/s (1 SM), 202 -> 301 (4), 539 -> 730 (11 SMs, 91% of the memory peak; a real H200 fills 90% with half its SMs). GEMM and DXA copy unchanged.
- Found while chasing why 4 dcache banks seemed to limit an SM; two earlier explanations (bank count, 128 B bank visit) were wrong, and the "87 / 105 / 109 GB/s" figures were a measurement artifact (differencing two run lengths).
- Fork: `VX_CFG_DCACHE_FWD_HOLD` (default 0), on in `h200s`. SimX now also models `VX_CFG_DCACHE_CRSQ_SIZE`.

## 11. Runtime: device address aliases a command-processor host region (bug, crash or silent corruption)
- `sw/runtime/simx/vortex.cpp`: the CP address of a host region is the raw pointer, and the dram hooks route by "address inside a registered region", checking only the start. A device address can equal a heap pointer.
- Seen: one 42 MB upload on 1 core, chunk at 0x2070000 inside the command ring's allocation -> copied over the host heap -> segfault in `make_cp_hooks`. Without a crash: 64 KB missing from device memory, or host bytes in a download.
- Fix: tag host addresses with bit 62, check the whole range. simx and rtlsim fixed; foldsim and firesim have the same lookup.

## 12. DXA memory tag width (bug, RTL elaboration)
- The socket arbiter's tag was sized for the L1, texture and ray caches; the DXA had to fit. With `VX_CFG_DXA_MAX_INFLIGHT` 256 and 8 workers it does not, and the RTL fails to elaborate. Fix in `VX_gpu_pkg.sv` (`DXA_MEM_TAG_WIDTH` in the maximum) and a lint guard in `VX_dxa_gmem_req.sv`.

## 13. rtlsim stops at time 0 on H100-shaped profiles (bug, RTL/profile mismatch)
- `VX_tcu_bbuf.sv:100` asserts LMEM banks == B block words (32 at 32 threads); h100/h200 profiles set `VX_CFG_LMEM_NUM_BANKS=16` (8 B banks, the part's 32 x 4 B). SimX has no such check and keeps the whole physical row. Any rtlsim run with WGMMA on these profiles fails.

## 14. wgmma throughput (performance)
- Target: 989.5 TFLOPS / 132 SMs / 1.98 GHz = 3,786 FLOPs per clock per SM. wmma roof 2,048 (measured 1,923); wgmma with FEDP2K 340.
- Time per warpgroup multiply at 16 CTAs: 132 cycles, of which compute 16, A fetch 11, B fetch 19, CTA lockstep 41, block empty 45.
- Causes: one shared CTA-keyed B buffer (`VX_tcu_tbuf.sv`, `VX_tcu_lockstep.sv`), no prefetch, one 128 B/cycle port.
- Patch: per-block two-slot B buffer with prefetch (`VX_CFG_TCU_BBUF_ROWS`) and `VX_CFG_TCU_TBUF_PORTS`. FLOPs/cycle, N=16 / N=64 tile: 1 port 948 / 1,326; 2 ports 1,513 / 2,384; 4 ports 1,988 / 2,989.
- Operand bytes per clock for 4,096 FLOPs: 320 (64x16), 128 (64x64), 80 (Hopper m64n256k16); Vortex stops at N=64 (accumulators in 32 FP registers).
- `sgemm_tcu_wg` is not sped up (bound by staging).

## 15. FIFO L2 and co-running SMs (calibration)
- All caches default to FIFO replacement. A streaming SM pushes another SM's hot lines out of the shared L2 once per L2 fill.
- Co-run / solo, compute role: FIFO 0.83-0.98, pseudo-LRU 1.00-1.07 (real H200: 1.00). Solo workloads move at most 1.6% (stream at 11 SMs +2.7%).

## 16. L1 dcache + shared memory (profile accuracy)
- Separate structures, each at full size: 256 KB + 228 KB = 484 KB per SM against the part's 256 KB configurable pool; same pattern in v100, a100, rtx5080 profiles. L1 at 32 KB: load-mode GEMM +1%, DXA-mode +9-18%.

## How these were found (for the talk)
- Rule that produced most of them: match a real H200, never provision above it, and when a number looks wrong test several hypotheses instead of tuning.
- Wrong turns worth showing: bank count as the stream limit (artifact); "DXA is cluster-shared" (stale comment and design doc; it has been per SM since 7d6950de5); buffer overwrite as the M=256 cause (it was the dcache); the first dcache fix (opened a new hole, caught by review).
- Tools that paid off: rtlsim parity runs from frozen copies; the LSU shadow checker (`notes/m16_memchk_debug.patch`); a directed test per bug; independent review before pushing.
