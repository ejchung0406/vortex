# Upstream PR candidates

Things found on the fork (`feature/dxa-borrowing`) that could go to upstream
Vortex, one PR per topic, once each is verified. Nothing here has been sent.
Numbers are SimX unless noted; `h200s` is one sixth of an H200 (22 SMs, one
HBM3e stack, 800 GB/s). Last updated 2026-11-21.

Status words: **idea** (not implemented), **experimental** (works in a throwaway
copy, behind build macros), **on fork** (committed on the fork branch),
**verified** (measured against its acceptance test), **ready** (clean code,
reviewed, both simulators).

| # | Topic | Kind | Status | Left before sending |
|---|---|---|---|---|
| 1 | DRAM model: HBM3E type, scheduler window, bank groups, 1 KB interleave | feature | on fork (299605e51), SimX + rtlsim | row timings are placeholders; review |
| 2 | Streaming test with a CUDA-shaped access pattern (`bwstream -b`) | test | on fork (601a1d482) | short README |
| 3 | Decode GEMM test `gemm_block` (load and DXA staging, two roles, rates, per-CTA walk quartiles) | test | on fork (3d21214d2..b56fc65e4), passes | CI case |
| 4 | DXA dispatch prefers an idle worker | fix, SimX/RTL parity | on fork, RTL b0adb2663; rtlsim 66,067 -> 60,307 | review |
| 5 | DXA engine on several memory ports | feature | SimX only; ports alone give nothing (see 24) | superseded by 24 |
| 6 | Core limits that hold the compute roof | config | profile values; rtlsim 118,350 -> 35,006 (SimX 118,418 -> 33,419) | defaults vs profile-only |
| 7 | Smaller findings | notes | see below | |
| 8 | **DXA workers per SM scale with the number of SMs simulated** (1 at 1-15 cores, 2 at 16-23, 16 at 132) | **bug** (config + stale doc) | found; profile works around it; `VX_CFG_DXA_MEM_PORTS` has no reader | report; pick the per-SM count |
| 9 | **SimX dcache returns a younger store's data to an older load** | **bug** (wrong results) | fixed on fork dab77b7f2 + 51a4cce00, test `war_order`, reviewed twice; rtlsim passes it (16x16 write-back: 1,296,287 vs SimX 1,295,606) | h200s-size rtlsim still running; CI case |
| 10 | **SimX dcache replays a forwarded fill on a full response queue; RTL holds** | **bug** (SimX/RTL divergence, timing) | fixed on fork cbc696a98 behind a knob, on in `h200s`; rtlsim 20,570 vs SimX 20,266 / 25,824 | make it the default |
| 11 | **Runtime: device address can alias a CP host region** | **bug** (crash or silent corruption) | fixed on fork b974a9e98 (simx), 51a4cce00 (rtlsim) | foldsim, firesim |
| 12 | **DXA memory tag too narrow for more in-flight slots** | **bug** (RTL does not elaborate) | fixed on fork 348263623, rtlsim builds and passes | review |
| 13 | **rtlsim stopped at time 0 on H100-shaped profiles with WGMMA** | **bug** (RTL/profile mismatch) | fixed on fork 86143baa1; 32 threads / 16 banks and the h200s shape pass on rtlsim | review |
| 14 | wgmma throughput: one shared B buffer puts the four tensor blocks in lockstep | performance | SimX patch `wgmma_tbuf.patch` (340 -> 1,326..2,989); not on fork | set aside: 17 reaches the part's rate without more ports |
| 15 | FIFO L2 makes disjoint SMs slow each other | calibration | pseudo-LRU set in `h200s` (9c27e289a) | propose as a default? |
| 16 | L1 dcache and shared memory both at full size (484 KB vs the part's 256 KB pool) | profile accuracy | found, in every profile from Volta on | decide the split |
| 17 | Tensor-memory engine: B fetched two blocks ahead, A read by bank rows | performance (design change) | on fork, RTL + SimX (93a8adad4, 055e5d12d, merge 303d10c29), on in `h200s`: 1,787 -> 3,684 FLOPs/clock/SM (part 3,786); rtlsim/SimX within 1% | the upstream TMEM branch must land first; logic depth and area not measured |
| 18 | A barrier waited for the whole core's LSU to drain | design property, performance | on fork, RTL + SimX (9215d3f88, caa4fcd4d, merge 7658376d9); scope-1 review fixes and parity cases (VM, link, back-pressured bank, sparse WGMMA) merged 18deea5da; on in `h200s`: load-staged decode 15.08 -> 21.06 GB/s per SM (21.03 after the fixes, x1.40), no two-state rate | logic depth; sliced dcache crossbar and shared-dcache arbiter documented, not covered |
| 19 | New kernels on the tensor-memory engine (`gemm_tmem`, `gemm_tmem_decode`) | tests | patch `gemm_tmem.patch`; 3,334 FLOPs/clock and 82 GB/s per SM | put on the fork after 22 and 24 |
| 20 | DXA and shared-memory port against the real part | calibration | merged into 24 and 27 | |
| 21 | What the real H200 measurements changed (per-SM ingest ~210 GB/s, GPC limit 1.28 TB/s, co-residency starves) | study direction | measured; `tpc-placement-and-borrowing.md` | not an upstream item |
| 22 | **A lone warp executes one instruction per ~11 cycles** (stalled until decode at every instruction) | design (since 2021), performance | measured: H200 one warp up to ~1 instr/cycle, Vortex 11.75 cycles/instr (RTL = SimX on 52 cases); pipelined sequential fetch `VX_CFG_FETCH_INFLIGHT` in RTL + SimX on `feature/warp-rate` (4c0cf2c81): one warp 11.81 -> 3.84, rtlsim = SimX on 49 of 52 rows, the rest within 0.05% | not merged, review pending; logic depth; next limit is the issue path (~3.8 cycles) |
| 23 | **SimX's L2 replays a forwarded fill on a full response queue; RTL holds** | bug (SimX/RTL divergence, timing) | knob on fork (462002754), default off; SimX with hold off is closer to rtlsim (2 cores 20,860 vs 20,864 off / 21,137 on), so not set in `h200s` | explain why hold=1 is further from rtlsim |
| 24 | DXA against the real part's copy curves (16 points) | calibration (design changes) | SimX knobs in `dxa_lmem_calib.patch`: 13 of 16 points within 10%; not on fork | redo after 22; RTL |
| 25 | The megapage heap leaves the first 2 MB on 4 KB pages | runtime behaviour | found; test-side workaround | decide whether to change the mapping |
| 26 | **SimX routed a tile-buffer word by comparing descriptor bases** | **bug** (abort) | fixed on fork 0c1054236 | review |
| 27 | The shared-memory DMA port is exclusive between DXA writes and tile-buffer reads | calibration (design change) | SimX knob (budget 160 B/clock) in `dxa_lmem_calib.patch`; value not checked | check after 22; RTL |
| 28 | **Debug builds fail on both drivers** (`__assert` in `sw/common/util.h` clashes with glibc) | **bug** (build) | found, not fixed | rename the macro |
| 29 | **Every FP operation serialises on fflags** (one FP op per ~21 cycles per warp) | design, performance | `VX_CFG_FFLAGS_ACCUM` in RTL + SimX on `feature/warp-rate` (ea56a3ae3), rtlsim = SimX on every `warp_rate` row; one warp `fma_ind8` (8 warps x 8 threads build) 2,686 -> 1,388 cycles per iteration | not merged, review pending; decide the fflags rule; logic depth |
| 30 | 16-byte loads unavailable at XLEN 64 (`LDST_WIDE` needs FLEN 32) | ISA / config limit | found | decide whether to allow it at FLEN 64 |
| 31 | **SimX adds one register per direction on the DXA's socket memory path** | SimX/RTL divergence, timing | fix in `dxa_mc_parity.patch`, not applied: h200s `-n32` rtlsim 201,583, SimX 182,421 today (-9.5%), 219,161 with the patch (+8.7%) | a second, depth-dependent difference remains; 4-8 cores still +4.8% / -3.4% |
| 32 | DXA perf counters: SimX sums all sockets, rtlsim reports socket 0 | SimX/RTL divergence, counters | found | pick one meaning |
| 33 | **SimX local memory slept with a packet in flight to it** (hang with 2+ DXA workers) | **bug** (hang) | fixed on fork fad1f31d7; reproduces on upstream/master (`sgemm2_dxa`, 2-8 workers) | commit on a PR branch, CI case |
| 34 | LR/SC at large configurations is slow, not livelocked | behaviour, performance | measured (`amo -t lrsc_counter`, h200s) | none; note only |
| 35 | **upstream/master SimX does not build in a TCU configuration without WGMMA** (`decode_descs` guard) | **bug** (build, upstream) | found, not fixed | guard the call or move the definition |
| 36 | **SimX freed a warp's instruction-buffer slot one cycle later than `VX_ibuffer`** (instruction arriving at an empty buffer) | SimX/RTL divergence, timing | fixed on `feature/warp-rate` (4c0cf2c81), not merged; invisible with one fetch in flight, 8% with four | merge with item 22 |
| – | DXA copy on 4 cores: SimX 19,874 vs rtlsim 18,321 cycles (8.5%) | unexplained divergence | noted under 23 | find the cause |
| – | Plain-load calibration: L1 input limit (~139 GB/s per SM), candidate `VX_CFG_DCACHE_READ_MERGE_LINE` | calibration (not adopted) | SimX only, `ld_calib.patch` | keep as a candidate until item 22 lands |
| M1 | cuBLAS 13.4 returns NaN or wrong values for BF16 GEMMs under an MPS active-thread percentage | real-GPU measurement hazard, not a Vortex item | measured (`h200-go-nogo.md` §3) | none: MPS-percentage results invalid, TPC masks used |

Issue texts in English for 9, 10, 11: `upstream-issue-drafts.md`. Upstream `master` checked at ccf811e11 (2026-11-20): the affected files are unchanged there.

---

## 1. DRAM model

**Problem.** With the stock model the memory delivers far less than its nominal
bandwidth once many SMs read at once (33 SMs streaming on a quarter H200:
16-22% of the peak), and a decode GEMM slows down per SM as SMs are added
(+33% at 11 SMs, +113% at 22).

**Causes found, each measured on `h200s`.**

| Cause | Stock | Changed to | Evidence |
|---|---|---|---|
| Channel shape | one 128-bit channel per pair of HBM3 channels, 9375 MT/s | 64-bit channel, two 32-bit pseudo-channels, 32 B burst = BL8, 6250 MT/s (new type `HBM3E`) | JESD238B.01: 64 DQ per channel, 256-bit prefetch per PC, BL8 |
| Scheduler window | Ramulator's `ReqBuffer::max_size = 32` per channel (a constant) | 256 | 22-SM stream: 382 GB/s at 32, 588 at 128, 621 at 256, 636 at 512. At 32, 74% of enqueue attempts were refused. Little's law gives ~100 entries |
| Banks | 16 per pseudo-channel, 4 groups | 32 per pseudo-channel, 8 groups of 4 | JESD238B.01 Table 4/5: 16 x (stack-ID count) banks per PC; a 24 Gb x 8-high stack has 32. At window 32: 382 -> 522 GB/s; 64 banks no further gain |
| Same-group read spacing | 4 model clocks (1.28 ns) | tCCDL = max(4 nCK, 2.5 ns) | JESD238B.01 Table 93. Under 2% |
| Channel interleave | one 64 B line per channel, in order | 1 KB per channel, channel and bank chosen together by a hash of the upper address bits | below |

**Interleave.** A 1 KB unit keeps a contiguous 1 KB in one row of one bank.
Without mixing, all streams land on one channel at a time (11-SM stream
425 -> 200 GB/s); mixing channel and bank separately used half the banks; a
joint mix (`(unit ^ hash(upper bits)) & mask`, a bijection per block) uses all
of them. The 1 KB unit comes from someone's reading of the GPU's driver, not
from a document (Accel-Sim uses 256 B): call it a calibrated value.

**22-SM stream, window 256, 32 banks, GB/s (peak 800; a real H200 reaches 90%).**

| Interleave | warp reads its own stream | CUDA block shape (item 2) |
|---|---|---|
| 64 B (stock mapping) | 660 | 493 |
| 256 B + joint mix | 643 | 706 |
| 1 KB + joint mix | 408 | 708 |

Final profile: stream 714-734 GB/s and DXA copy 728 GB/s at 22 SMs.

**Not settled.** Row timings (tRCD, tRP, tRAS, tRC, tRRD, tFAW) are vendor
values, blank in the standard; the model uses 14/14/33/47/4/15 ns and the
sensitivity is not measured. Ramulator issues one command per clock per
channel while the standard has separate row and column buses; not measured.

**As PRs.** (a) `HBM3E` type and bank groups, (b) scheduler window (a
"Windowed" copy of the controller built by `third_party/Makefile`), (c)
interleave unit and mixing (`DramSim::Config::locate()`, shared by SimX and
rtlsim). Existing profiles keep their behaviour unless they opt in (checked
cycle-identical). The standard's PDF is licensed and must not be committed.

---

## 2. Streaming test with a CUDA-shaped access pattern

**Problem.** `bwstream` gave each warp its own contiguous stream. In a CUDA
kernel the warps of a 1024-thread block read neighbouring 128 B pieces at the
same moment and a warp's next load is 4 KB on. The two orders rank address
mappings oppositely (table in item 1).

**Change.** `-b W`: W one-warp CTAs share a slice as the warps of a W x 32
thread block would; `-b 1` is the old behaviour.

**Lesson.** In the own-stream shape, differencing two run lengths is invalid
(the stream spacing changes with the length): that produced the wrong
"87 / 105 / 109 GB/s" figures. In the block shape whole-run and differenced
values agree.

---

## 3. Decode GEMM test `gemm_block`

**What exists upstream.** `sgemm2_tcu`, `sgemm_tcu_wg`, `sgemm_tcu_wg_dxa` are
general GEMMs; at the decode shape (M = 16) they refuse M or take 30x longer
(20.0 M cycles on one SM against 0.66 M).

**Change.** The CTA's warps are laid along N and the weights staged a K chunk
at a time: `-s 0` by coalesced loads, `-s 1` by the DXA engine with `-d`
chunks outstanding. Later additions: two roles split by SM (`-b`), `-r`
rounds, `-o` one role alone, `-R` weight reuse for the compute role, and
per-role rates over the window in which both roles run (and over the steady
window after the first generation of CTAs).

**Baseline on `h200s`, GB/s per SM.**

| | 1 SM | 11 | 22 | Real H200 |
|---|---|---|---|---|
| loads | 15.9 | 15.0 | 14.2 | FP16 cuBLAS 16-20 |
| DXA depth 1 | 26.3 | 25.6 | 18.6 | |
| DXA depth 4 | 27.9 | 29.0 | 22.7 | FP8 TMA 29-35 (half the SMs), 19-21 (all) |

**Note.** A multiply of more than two tiles spills 24 loads and 24 stores per
K step: `mma_sync` pins 24 of the 32 FP registers.

---

## 4. DXA dispatch prefers an idle worker

**Problem.** A worker reports ready whenever its setup stage is free, whether
or not a transfer runs on it, and the dispatch takes the lowest-numbered ready
worker: a request waits behind worker 0's transfer while others idle.

**Change.** Idle workers first, in SimX and in the RTL (`VX_dxa_dispatch.sv`,
`VX_dxa_core.sv`, `VX_dxa_worker.sv`).

**Verified.** h200s, one SM, 8 workers, `bwstream_dxa -n8`: rtlsim 66,067 ->
60,307 cycles (-8.7%); SimX 61,767 -> 57,629 (-6.7%).

---

## 5. DXA engine on several memory ports

The engine has one of the socket's four L2 ports. One SM, GB/s: 3 workers 65;
4 workers with 1 / 2 / 4 ports 84 / 90 / 94.5; 8 workers with 1 port 101; 16
workers 111. A real H200 SM delivers about 101. Chosen: **8 workers, 1 port,
256 lines in flight**; more ports are not pursued.

---

## 6. Core limits that hold the compute roof

Register-only multiply-add kernel, 64 warps, issue width 4:

| Limit | Derived value on the H100 shapes | Set to | IPC |
|---|---|---|---|
| (stock) | | | 0.86 |
| FPU operations in flight per block, `VX_CFG_FPU_QUEUE_SIZE` | 2 | 16 | 2.18 |
| Operations between issue and a unit, `VX_CFG_DISPATCH_QUEUE_SIZE` | 4 | 8 | 2.96 |
| Register-file banks, `VX_CFG_NUM_GPR_BANKS` | 4 | 8 | 3.30 |

**rtlsim.** Stock 118,350 cycles (SimX 118,418); all three raised 35,006
(SimX 33,419): 3.4x in the RTL too.

For upstream: should the derivation of `FPU_QUEUE_SIZE` account for the FMA
latency, and should `h100`/`h200` carry the values.

---

## 7. Smaller findings (notes in `vortex-design-inefficiencies.md`)

- FP operations of one warp are serialized by the FCSR rule
  (`sim/simx/scoreboard.cpp`); no effect at 64 warps. Low priority. (Measured
  and in the RTL as well: now item 29.)
- The `memory: lat=` perf counter wraps on large runs.
- TLB reach: a decode GEMM over 128 MiB of weights needs `DTLB_SIZE` and
  `L2_TLB_MEGA_SIZE` of 256 with 2 MB pages, and `VORTEX_VM_MEGAPAGE_HEAP=1`
  (without it the run measures page walks).
- The debug build of SimX does not compile (`__assert` clash). (Both drivers:
  now item 28.)

---

## 8. DXA workers per SM scale with the number of SMs simulated (bug)

```
[dxa]
# Cluster-level DXA engine count, decoupled from cores/socket size.
VX_CFG_NUM_DXA_CORES = "expr: max(1, up($VX_CFG_NUM_CORES / 8))"
```

The formula dates from when the engines were a cluster-level pool (2026-02).
Commit `7d6950de5` (2026-07-11) moved the DXA core into every socket and did
not touch the formula, so the value is now the number of workers inside each
SM's DXA core, still derived from the total core count: 1 worker at 1-15
SMs, 2 at 16-23, 16 at 132 (**[corr. 2026-11-21]** earlier text said "1 at
1-8 SMs, 2 at 22"). An SM's TMA capacity depends on how many SMs the run has,
which breaks any SM-count scaling experiment; the comment and
`docs/designs/dxa_async_copy_multicast.md` describe a structure that no longer
exists. `VX_CFG_DXA_MAX_INFLIGHT` defaults to 8 beside it (8 -> 6.7 GB/s,
256 -> 90 GB/s).

**Proposed.** A per-socket count independent of `NUM_CORES`, and a corrected
comment.

**History of the misreading.** This fork's notes first called the DXA "a
cluster-level shared pool, Virgo-style" from the comment and formula alone.
Wrong: it has been per SM since July. Check the instantiation, not the comment.

---

## 9. SimX dcache: a younger store overtakes an older load of the same word (bug, wrong results)
- **Symptom**: `gemm_block` with 16 tiles (M=256), DXA staging: 1-30 of 262,144 outputs off by 1-3%, different ones as the code layout changes. Errors under the host check's 1% tolerance pass silently.
- **Cause** (`sim/simx/mem/cache.cpp`): a store that hits skips the MSHR chain at admission and is ordered only by an MSHR lookup at the pipe exit. An older same-word load that has left the chain for replay is invisible there, so the store merges first and the replayed load returns its data. Write-back caches have the same hole.
- **RTL**: orders at S0 (`creq_hit_order_hazard_st0`, `VX_cache_bank.sv:716-738`; `VX_cache_mshr.sv:524-538`). rtlsim passes `war_order` on the 16x16 write-back configuration (1,296,287 cycles; fixed SimX 1,295,606, 0.05% apart); the h200s-size rtlsim runs are still running.
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
- Set in `h200s` as a calibrated value (the part's policy is not published).

## 16. L1 dcache + shared memory (profile accuracy)
- Separate structures, each at full size: 256 KB + 228 KB = 484 KB per SM against the part's 256 KB configurable pool; same pattern in v100, a100, rtx5080 profiles. L1 at 32 KB: load-mode GEMM +1%, DXA-mode +9-18%.

## 17. Tensor memory engine: deeper B prefetch, A by bank rows (performance, design change)
- **Base**: `upstream/feature/tmem-dataflow` (three commits, 2026-11-15/16, not on `master`): a per-core tensor memory in the style of Blackwell's `tcgen05`. Accumulators live in tensor memory, so a tile can be N = 512 wide where FP registers stop at 64; one `tc.mma` uses all four blocks. wmma, the DXA and `gemm_block` run cycle-identical in a TMEM build; wgmma is excluded from it.
- **As is** (h200s, FEDP2K, one 128 B/cycle tile-buffer port, test `tmem_roof`): 976 / 1,296 / 1,587 / 1,789 FLOPs per clock per SM at N = 64 / 128 / 256 / 512, against a 4,096 roof and the part's 3,786. The port carries 34 B/clock, so it is not the limit.
- **Cause**: a B block serves 2 compute cycles; the engine fetches one block ahead and a fetch takes 4 cycles, so the walk computes 2 cycles in every 4. A is read one row per request (64 reads per op where 16 bank rows carry the same bytes). Both are in the branch's RTL too (`VX_tcu_bbuf.sv` `pf_en`, `VX_tcu_abuf.sv`), read not run; whether the RTL turnaround is 3 or 4 cycles is not settled.
- **Change** (SimX only so far, both default to the branch's behaviour and keep its cycle counts): `VX_CFG_TCU_TMEM_B_AHEAD` (2: fetch two blocks ahead; +128 B of buffer, 256 -> 384 B) and `VX_CFG_TCU_TMEM_A_BANKROW`.

| N | as is | A by bank rows | B two ahead | both |
|---|---|---|---|---|
| 128 | 1,296 | 1,699 | 1,868 | 1,926 (issuer-bound; 2,849 unrolled) |
| 256 | 1,587 | 1,857 | 2,567 | 3,355 |
| 512 | 1,789 | 1,948 | 3,156 | 3,689 (3,684 without counters) |

- Depth 3 equals depth 2. Port traffic at the best point 62-65 B/clock: one port is enough. Remaining 7% at N = 512 is the per-op A fetch (18 of 276 cycles).
- **Not solved**: the branch's GEMM test stays at 1,292 -> 1,299 (staging by the DXA sets the pace, the engine sits empty 60% of the time). A lone issuing warp gives one `tc.mma` per ~128 cycles (11 cycles per ALU instruction), which binds below N = 256 unless the loop is unrolled.
- **Kind of change**: hardware design (engine control and one more buffer slot), not a kernel change and not a wider port. No size value of the profile changes.
- **Before sending**: the branch has to land upstream first; RTL counterpart of both knobs; rtlsim parity.

## 18. A barrier waits for the whole core's LSU to drain (design property, performance)
- **What**: `VX_wctl_unit.sv:185` holds a barrier at execute until every LSU block of the core is empty and the MMU idle (`bar_drain = is_bar && !((&lsu_sched_empty) && mmu_empty)`); SimX does the same (`sfu_unit.cpp`, `core_->lsu_drained()`). So a CTA's barrier waits for the outstanding loads of every other CTA on the SM, and the single SFU block is blocked behind it (the CSR read after each barrier as well). CUDA's `bar.sync` only orders the CTA's own shared-memory writes.
- **Effect**: with several CTAs per SM staging by loads, barriers release only at core-wide drain moments. If all CTAs start together they lock into step (262K cycles per walk); once staggered, some CTA nearly always has a miss outstanding and every barrier waits longer (about 300K). Runs switch between the two on their own: this was the unexplained "0.86" co-run loss. Per-warp work is the same in both states; all extra time is at barriers and in the SFU queue (+1,753 warp-cycles per chunk).
- **Which is honest**: random or staggered CTA starts always end slow. The decode baseline measured with all 16 CTAs launched together (15.9 GB/s per SM) is the lockstep figure; staggered it is 14.8.
- **Experiment** (`VX_BAR_DRAIN=warp` in SimX: a barrier waits for its own warp's requests only; results still pass): load-mode memory role 6.5-7.8 -> 11.6 B/cycle (x1.49 lockstep, x1.72 staggered), no state switching, decode baseline 22.5 GB/s per SM, co-run/solo 0.99. A real H200 does 16-20 GB/s per SM with FP16 cuBLAS, so this would overshoot the load-mode calibration.
- **DXA mode is not affected** (one warp issues copies ahead; no per-warp load phase).
- **Status**: instrumentation and the experiment switch in `notes/bwco_instrumentation.patch`; nothing on the fork. A fix is a hardware change in RTL and SimX together; its timing cost is not measured.

## Status update 2026-11-21 (items 13, 17, 18 and new 19-21)
- **13 (rtlsim stopped on H100-shaped profiles with WGMMA)**: fixed on the fork (86143baa1): the B tile buffer follows the block geometry, not the LMEM bank count. At 32 threads / 16 banks rtlsim now passes WGMMA fp16 SS 92,712 (SimX 92,470), sparse 66,916 (66,492), FEDP2K RS 199,345 (201,187).
- **17 (tensor-memory fetch)**: both knobs are in the RTL as well (93a8adad4, 055e5d12d) and merged into the work branch (303d10c29). rtlsim vs SimX per op: 8 threads N=128 off 301.6/300.6, B ahead 2 175.6/174.6, both 151.6/150.6; 32 threads N=256 334.1/333.0, 208.1/207.0, 160.1/159.0. RTL B turnaround measured 4.0 cycles per block. Correction: B storage goes from 256 B to 640 B at 32 threads with depth 2 (the two original slots are kept and three full-row slots added), not +128 B; reusing the original slots would give 384 B. A found on the way: SimX did one `tc.cp` row per cycle, the RTL one per 3 cycles (fixed, 9fda5ab5c/888369783). Reviewed independently: no blocker; open: logic depth and area never measured (no tool here), perf baselines stale, sparse knob-on parity 3.8%.
- **18 (barrier drain)**: implemented as `VX_CFG_BAR_DRAIN_SCOPE` in RTL and SimX (9215d3f88, caa4fcd4d), merged (7658376d9). Scope 1: a stalling barrier waits for its own warp's requests plus stores in flight past the LSU. 16 cases cycle-identical at scope 0 in both simulators; scope 1 passes all, SimX/rtlsim within 0.0-1.4%. h200s: load-staged decode ~~15.8 -> 22.2~~ **[corr. 2026-11-21]** 15.08 -> 21.06 GB/s per SM (21.03 after the scope-1 fixes, ratio 1.40), the M=64 role's walk 1,395,242 -> 816,228 (~827 K after the fixes), no two-state behaviour. History: the core-wide drain came from f9c79bb40 (2026-05-29) to give CUDA `__syncthreads` semantics; a real H200 shows no dependence of a barrier on other CTAs' loads (E4). No test fails even with the drain removed, so the store terms are a structural safeguard, not a demonstrated fix.
- `h200s` now sets barrier scope 1, FEDP2K, tensor memory (1,024 columns), B ahead 2, A bank-row (dc9fdeef4).

## 19. New kernels on the tensor-memory engine (tests, not on the fork yet; `notes/gemm_tmem.patch`)
- `gemm_tmem` (compute-bound, M large, weights reused): 3,608 FLOPs per clock per SM over the multiply phase, 3,334 over the kernel (part: 3,786); holds at 1/2/4 SMs; DRAM traffic 0.6% of an SM's fair share with reuse. Needs weights and activations packed by K step (2,557 with plain layouts).
- `gemm_tmem_decode` (memory-bound, M = 4/16/64, each weight byte read once): 82 GB/s per SM (gemm_block: 26-28; real decode on few SMs: 88-104). Bound by the shared-memory DMA port: one client per cycle, and a DXA worker writes one 64 B line per beat. Scaling 82 / 153 / 266 / 595 / 729 GB/s at 1 / 2 / 4 / 11 / 22 SMs (memory saturates between 10 and 22 SMs).

## 20. DXA and shared-memory port against the real part (calibration, in progress)
- Real H200, one SM: TMA 16 KB x 1/2/4/8 = 30/57/104/142 GB/s; 32 KB x 1/2/3/6 = 52/99/142/208; ceiling 210-219. Shared memory: reads 253, TMA writes 215, staged 157-170. Vortex: DXA copy tops out at 101-111 (one 64 B/clock gmem port), and the LMEM DMA port is exclusive between DXA writes and tile-buffer reads. Experiments running (ports, 128 B write beats, port arbitration, per-transfer cost).

## 21. What the real H200 measurements changed (study direction, not an upstream item)
- One SM alone reaches ~210 GB/s; the "100 GB/s per SM" was 4 x 16 KB in flight. Software borrowing through thread-block clusters works but adds nothing. A GPC returns at most ~1.28 TB/s (about 6 SMs); spreading memory-bound SMs over the 8 GPCs gives 2.4x over packing; MPS appears to pack. Co-residency starves the memory job (0.6-22%). See `tpc-placement-and-borrowing.md`, `noc-and-design-space.md`.

## Added 2026-11-21 afternoon (items 22-27)

### 22. A lone warp executes one instruction per ~11 cycles (design, old; performance)
- `hw/rtl/core/VX_warp_sched.sv` stalls a warp at every schedule until its instruction is decoded ("stall the warp until decode stage"), for every instruction, not only control flow. In the history since at least 2021 (b1eef0fb7 "warp scheduler optimization"; the comment since Vortex 2.0, c1e168fdb). SimX mirrors it.
- With 64 ready warps the core still reaches IPC ~3.3 of 4, so throughput tests do not show it. Anything paced by one warp does: a lone warp issuing `tc.mma` gives one per ~128 cycles; a producer warp issues one DXA transfer per 354-436 cycles (35-37 instructions per iteration), which capped one SM's copy at ~125 GB/s where a real H200 does 210; warps reading shared memory with loads reach 48-74 GB/s where the part does 251-253.
- Hopper/Blackwell-style kernels (one warp issuing TMA or tensor ops for the SM) depend on exactly this.
- **Why it stayed hidden**: in the kernels Vortex was built around, all 64 warps do the same work (each loads, each multiplies), so one warp's idle cycles are filled by the others and the SM issues 3.3 of 4 instructions per clock. In a Hopper-style kernel one producer warp issues the TMA requests (and in a Blackwell-style one a single thread issues the tensor ops) while the other warps wait for what it brings; nobody can fill that warp's idle cycles, so the SM runs at that one warp's pace.
- **Evidence**: with the DXA busy 96% of the time, 1.09 of its 8 workers were running on average; every stage from the DXA's memory port to the L2 banks, MSHRs and DRAM was idle; requests arrived one per ~436 cycles whatever the ring depth.
- **Direction of the fix**: GPUs have no branch predictor (ready warps fill the gap, divergence makes prediction a 32-bit mask problem, and squashing speculative state is expensive), so the fix is not speculation: classify the instruction as it is fetched and stall the warp only for the ones that can change its PC or thread mask (branches, jumps, tmc/split/join/predicate, wspawn, barrier, traps); for all others keep fetching at PC+4. Nothing is ever fetched wrongly, so no squash path is needed. To check: the classification fits the fetch timing; the stages after fetch accept several instructions of one warp; the C extension (PC+2) is excluded or handled; and the resulting per-warp rate does not exceed the real part's.
- In progress: per-warp instruction rate measured on a real H200, a stage-by-stage budget in Vortex, and a SimX prototype (keep fetching past instructions that are not control flow or mask changes; knob, default off). Unrolling the issuing loop is a kernel-side workaround (75 -> 100 B/clock on one case), not a fix.

### 23. SimX's L2 replays a forwarded fill on a full response queue; the RTL holds (as item 10, for the L2)
- Same `VX_cache_bank`; SimX wired the hold only for the dcache. Knob `VX_CFG_L2_FWD_HOLD` (default 0), and `VX_CFG_L2_CRSQ_SIZE` now reaches the model. h200s: 11-SM stream 133,189 -> 130,691 cycles; decode GEMM 653,498 -> 635,151; with the DXA calibration set a deep copy point went 73.6 -> 85.3 B/clock. rtlsim parity not shown: the small multi-core runs never filled the queue.
- Seen on the way: `bwstream_dxa` on 4 cores (16x16, L2) takes 19,874 cycles in SimX and 18,321 on rtlsim (8.5% apart; 2 cores 3.2%), with either setting. Not explained.

### 24. DXA against the real part's copy curves (calibration; SimX knobs in `notes/dxa_lmem_calib.patch`)
- Real H200, one SM, GB/s: 16 KB x 1/2/4/8 = 30/57/104/142; 8 KB x 16 = 71; 32 KB x 1/2/3/6 = 52/99/142/208; 64 KB x 3 = 201; L2-resident 16 KB x 1/2/4 = 61/122/142, 32 KB x 1/2/3 = 95/183/211.
- Vortex today: 25/47/70/123; 97 (above the part); 32/61/87/125; 102; 37/64/80; 47/69/84.
- With the experimental set (2 gmem ports, 2 lines per worker per cycle, 512 lines in flight, shared-memory DMA budget 160 B/clock, 228-cycle issue interval per warp, capture buffer, L2 hold, 120-cycle start latency, unrolled issuing loop): 29/58/110/144; 72; 52/104/147/184; 162; 56/114/133; 92/184/188. 13 of 16 points within 10%, none more than 5% above.
- What each knob stands for: the worker's 64 B-per-cycle request/capture/write datapath is the RTL design (`VX_dxa_smem_wr.sv` holds one pending line), and its single capture slot head-of-line blocks the L2 response path; Vortex has no per-transfer issue cost (hence the 8 KB overshoot); the 120-cycle start latency compensates a missing SM-to-L2 network (warm 16 KB transfer: 571 cycles on the part, ~358 in Vortex) and should become link latency instead.
- Not yet RTL; to be redone after item 22.

### 25. The megapage heap leaves the first 2 MB on 4 KB pages
- `VORTEX_VM_MEGAPAGE_HEAP` maps from the heap start (0x10000) to the first 2 MB boundary with 4 KB pages (`vm.cpp` picks the leaf by alignment). A small buffer placed there walks every ~9 KB: 1,800 walks in a 4 MB copy (66.6 vs 75.2 B/clock). Workaround in the test: allocate past the first 2 MB.

### 26. SimX routed a tile-buffer word by comparing descriptor bases (bug, fixed 0c1054236)
- When an A row and B start at the same shared-memory address, B reads went to the A buffer and the run aborted with "TCU buffer miss". The RTL keeps separate A and B buffers.

### 27. The shared-memory DMA port is exclusive between DXA writes and tile-buffer reads
- One client per cycle (`local_mem.cpp` dma_fire; `VX_lmem_unit.sv`). A real SM does 215 GB/s of TMA writes, 253 of reads and 157-170 staged, i.e. the two overlap partly. In Vortex the decode kernel was bound by this port at 82 GB/s per SM; with a 160 B/clock budget it is engine-bound at 114. The budget value could not be checked against load-instruction consumers because of item 22.

## Added 2026-11-21 night (items 28-35; updates to 8, 18, 22, 23; what reproduces upstream)

### Update to 8 (DXA worker count)
- **[corr. 2026-11-21]** Workers per socket from `max(1, up(NUM_CORES / 8))`, where `/` is integer division and `up(x)` is "x, or 1 if 0" (`ci/gen_config.py`): 1 at 1-15 cores, 2 at 16-23, 3 at 24-31, 16 at 132. The list said "1 at 1-8" (source: `pr/notes/issue-dxa-worker-count.md`).
- `VX_CFG_DXA_MEM_PORTS` (`VX_config.toml:428` on upstream/master) is derived there and read nowhere (`git grep`; no reader under `hw/`, `sim/`, `sw/`, `ci/` or `profiles/`); the socket passes `GMEM_OUT_PORTS = 1` to its DXA core.

### Update to 18 (barrier scope 1): unconfirmed until `feature/bar-drain-fix` is verified and merged
- **Review findings**: the scope-1 store check stopped at the LSU adapter, so it did not cover stores still in the dcache flush unit's output buffer (`VX_mem_unit.sv:255-262`), in the socket dMMU under VM, or in the L1 link delay (`VX_socket.sv:221-237`); a load from another LSU block could overtake such a store (F2, medium, argued from code; `pr/notes/INDEX.md`). Also: no parity run had VM on; the description (config comment, profile, interface) said less than the code does; SimX counted reads and TCU requests in its scope-1 terms where the RTL did not (F1).
- **Fixes on `feature/bar-drain-fix`** (`git log feature/dxa-borrowing..feature/bar-drain-fix`; commit messages are "WIP", read from the diffs):
  - 9c99c5b34 (RTL + SimX): a scope-1 barrier waits for the whole path from the LSU to the dcache, i.e. a flush lane's request buffer (`VX_mem_unit` busy), this core's lanes of the socket dMMU and the link to the dcache (`dcache_path_drained` in `VX_core.sv`); the separate wait for every MMU of the socket (`mmu_empty`) is dropped from the scope-1 term.
  - f09e7c0c6 (SimX): tensor-unit metadata reads not yet taken into an LSU queue no longer hold a scope-1 barrier, as in the RTL.
  - 155190ece (docs): `VX_config.toml`, `VX_warp_ctl_if.sv`, `docs/designs/lsu_pipeline_design.md` (new table of where a request can still be held, and the sliced-crossbar caveat) and the `h200s` comment now say what the code does: the scope-1 terms are core-wide and count loads as well as stores.
  - New CI cases: `parity-vm-bar-vis-bar-warp` (VM on, 586fd3c41), `model_parity-wgmma-sparse-fullk-bar-warp` (7ab22fc33), `bar-vis-warp-scope-bank-stress` and `-link` (one-bank dcache with 4 MSHRs, with and without an 8-cycle L1 link; 4cb08bce4).
  - On the same branch, not barrier work: 4c5d609be (item 23's `L2_CRSQ_SIZE` assert), 1261d40da + 99a5a2168 (`sgemm_tcu_wg` with aliased A and B descriptors, the test for item 26), f6ab22bb4 (`gemm_block`'s idle role polls from one thread; `-o` needs the A extension), and WIP tests (`warp_rate`, `ldstream`, `smprobe`, `gemm_tmem*`, `bwstream` argument checks).
- **Consequence**: the `h200s` scope-1 figures (load-staged decode ~~15.8 -> 22.2~~ **[corr. 2026-11-21]** 15.08 -> 21.06 GB/s per SM, the M=64 walk 1,395,242 -> 816,228) are unconfirmed until that branch is verified, merged and `h200s` rerun. (Done: merged 18deea5da; see "Update to 18" in the late-night section below.) The upstream branch `pr/bar-drain-scope` (`pr/notes/bar-drain-scope.md`) was cut from the pre-review commits (its scope-1 term still includes the MMU) and its independent review did not run.

### Update to 22 (per-warp instruction rate): measured, prototyped
- **Real H200** (`gpu/warprate.cu`, `results_gpu/h200_warprate_summary.txt`): one warp issues up to ~1 instruction per cycle (independent FMA 1.07 cycles per instruction, independent integer 1.21); the SM reaches 4 per cycle (independent FMA, 8 or more warps: 4.00).
- **Vortex** (`tests/regression/warp_rate`, SimX `h200s`, 1 core, `results/wr_warp_rate/warp_rate_tables.txt`): independent integer 11.75 cycles per instruction (`int_ind4`; `int_ind8` 11.81), dependent integer 14.60, FMA 21.3-21.8 (item 29), a branch every instruction 23.99.
- **RTL and SimX agree cycle for cycle** on 52 small cases (13 kinds x 1/2/4/8 warps, 8 warps x 8 threads, issue width 4); at issue width 2 within ~0.2%.
- **Prototype `VX_CFG_FETCH_INFLIGHT`** (`notes/warp_rate.patch`, SimX only, default 1 = cycle- and register-hash-identical to base): a warp keeps up to N sequential fetches in flight, only when it is alone in its fetch slice (`fetch_alone`); a fetch past an instruction that changes the PC or mask is squashed (`fetch_squash`). Refused with the C extension (`#error`). At N = 4: `int_ind8` 11.81 -> 3.84 cycles per instruction, `int_ind4` 4.25, `br_every16` 5.49, `lds_own` 6.41; dependent chains unchanged (14.49); FMA only with item 29's knob (3.84). N = 8 with an 8-entry buffer: 3.75.
- **The "classify at fetch, no squash" direction** written above is bounded by the ~8-cycle icache round trip: the next fetch cannot start before the instruction is back to be classified. The prototype therefore fetches ahead and squashes.
- **RTL not written**; deferred until a research direction is chosen.

### Update to 23 (L2 forwarded-fill hold): parity result
- The RTL holds structurally (same `VX_cache_bank`), but SimX with hold=0 is numerically closer to rtlsim: 2 cores rtlsim 20,860 vs SimX 20,864 (hold 0) / 21,137 (hold 1); 4 cores 10,575 vs 10,978 / 11,179. So the knob stays default off and is not set in `h200s`. Why hold=1 moves away from rtlsim is not explained.
- Passing `VX_CFG_L2_CRSQ_SIZE` only armed an assert ("a deeper core-response queue is modeled under read merging only"; the L2 has no read merging). Fixed on `feature/bar-drain-fix` (4c5d609be: the queue takes `crsq_size` without read merging).

### 28. Debug builds fail on both drivers (bug, build)
- `sw/common/util.h:36` defines a macro `__assert`, which clashes with the `__assert` of glibc's `assert.h`; the debug build of SimX and of rtlsim does not compile. (Item 7 had it for SimX only.)
- Status: found, not fixed.

### 29. Every FP operation serialises on fflags (design, performance)
- `VX_decode.sv:628/636` marks every FP operation as writing the FCSR pseudo-register (`wr_xregs[XREG_0] = 1`, "most FP operations may set exception flags"), and `VX_scoreboard.sv:115/149/182` tracks it as a written register (in the busy mask, reserved at issue, checked against the in-use set). So each FP operation waits for the previous one to commit: one FP operation per ~21 cycles per warp, independent FMAs no faster than dependent ones (`fma_ind8` 21.57, `fma_dep` 21.27). The real H200 does 1.07 cycles per FMA for one warp. With 64 warps the core still reaches IPC 2.8-2.9, so throughput tests hide it (item 7's note).
- Prototype `VX_CFG_FFLAGS_ACCUM` (`notes/warp_rate.patch`, SimX scoreboard only, default 0): FP operations accumulate their flags instead of reserving the register; only instructions that read or write the FCSR wait. Measured together with `FETCH_INFLIGHT=4`: `fma_ind8` 21.47 -> 3.84 cycles per instruction, core IPC at 64 warps 2.76 -> 3.01. Not measured alone; RTL not written.

### 30. 16-byte loads unavailable at XLEN 64 (ISA / config limit)
- `sim/simx/decode.cpp:39`: `VX_CFG_EXT_LDST_WIDE_ENABLE` requires F with FLEN 32; the XLEN 64 builds have D, so FLEN is 64 and wide loads are off. The widest load is 8 B per thread, 256 B per warp instruction, against the part's 16 B per thread (512 B).
- Status: found; effect on load-staged kernels not measured.

### 31. SimX adds one register per direction on the DXA's socket memory path (SimX/RTL divergence, timing)
- `sim/simx/socket.cpp:235-251` binds a one-entry slice in each direction at the socket's memory port, behind which the socket arbiter registers both directions again; the RTL arbiter (`hw/rtl/VX_socket.sv:974-1005`, `mem_arb`, `REQ_OUT_BUF`/`RSP_OUT_BUF`) has one register each way. The L1 caches already discount it (`Cache::Config::mem_rsp_slice`); the DXA did not.
- Fix in `notes/dxa_mc_parity.patch` (`dxa_core.cpp`: a GMEM request enters the arbiter in its issue cycle, the response gets one cycle back on the way to the SMEM write). After it 1-2 cores are within 0.2% of rtlsim; 4-8 cores are still +4.8% / -3.4%.
- **Not applied yet**: one `h200s` point moves +20%, and its rtlsim reference run (`par_h2_dxa32_rtl`) is still running.

### 32. DXA perf counters: SimX sums all sockets, rtlsim reports socket 0 (SimX/RTL divergence, counters)
- `sim/simx/csr_unit.cpp:261-264` reads the cluster's DXA stats, which `cluster.cpp` sums over all sockets; rtlsim reports socket 0's only. On more than one socket the two drivers' DXA counters differ.
- Status: found.

### 33. SimX local memory slept with a packet in flight to it (bug, hang; fixed on fork fad1f31d7)
- `LocalMem` went to sleep when its engines were drained and no DMA input held a *delivered* packet (`empty()`). A packet reserved toward an input in the same cycle by a module that ticks first woke the unit before delivery; the unit ticked, found the input empty and slept again, and nothing woke it when the packet arrived. One DXA worker hid this (its next row re-armed the unit). With several workers the DMA channel filled in one cycle, every worker held its row, and the device stalled with all warps at their transfer barrier (h200q shape with `VX_CFG_NUM_DXA_CORES >= 2`, down to one core).
- Fix (`sim/simx/mem/local_mem.cpp`): the sleep test counts queued and in-flight packets on the DMA and request inputs (`size()`, as the cache does).
- Effect: runs without the DXA unchanged to the cycle (`sgemm_tcu`, `bwstream` on h200q); a DXA run that had recovered from lost wake-ups 0.14% shorter (2,532,751 -> 2,529,311, `bwstream_dxa`, four h200q cores, one worker).
- **Reproduces on upstream/master** (`pr/notes/INDEX.md`, results `results/pr_simx-lmem-wake_*`): SimX `sgemm2_dxa -n64 -t8 -m1` at 16 warps x 16 threads stalls (the run stops at the 100,000-cycle no-progress limit with every warp parked at one PC) with 2, 4 or 8 DXA workers on 1 core, with 4 workers on 4 cores, and at 16 cores with the default worker formula (2 workers); it passes with 1 worker. With the fix all pass: 309,094 / 311,340 / 307,346 / 97,692 / 42,206 cycles. rtlsim on master passes at 4 workers (311,222). Issue text (index, item 11): "simx: LocalMem can sleep past a packet reserved toward it in the same cycle, and the device stalls (its sleep test looks at delivered packets only)."
- Not explained: with the fix the 1-worker run changes slightly (308,369 -> 308,445); some 8x8 shapes also stall on master, fix side not run.
- Status: on fork; on the upstream side the fix is uncommitted in worktree `pr/simx-lmem-wake` (no commit, CI case or note).

### 34. LR/SC at large configurations is slow, not livelocked (behaviour)
- `tests/regression/amo -n1 -t lrsc_counter`, SimX `h200s` (`results/lrsc_*`); every run passes. Cycles:

| warps | 1 core | 2 | 4 | 8 | 22 |
|---|---|---|---|---|---|
| 4 | 247,089 | 494,689 | 929,864 | 1,805,495 | 5,170,288 |
| 16 | 3,259,085 | 6,519,617 | 13,094,833 | | |
| 64 | | 72,972,015 | | | |

- Linear in cores, about quadratic in warps (4 -> 16 warps: x13). Not an upstream item; recorded so the long runs are not mistaken for a hang.

### 35. upstream/master SimX does not build in a TCU configuration without WGMMA (bug, build)
- With `VX_CFG_EXT_TCU_ENABLE` and without `VX_CFG_TCU_WGMMA_ENABLE`: `sim/simx/tcu/tcu_unit.cpp:1239: 'class vortex::TcuUnit::Impl' has no member named 'decode_descs'`. eaea0c79a (#488) calls `decode_descs()` from the unguarded `Impl::wgmma()` but defines it inside the `#ifdef VX_CFG_TCU_WGMMA_ENABLE` block (lines 760-1129). Upstream's own non-WGMMA SimX TCU cases (e.g. `sgemm_tcu-1`) fail with it. Source: `pr/notes/tests-gemm-block.md` (Dependencies), seen in `results/pr_gb_fork_s0`; reported by three agents.
- Status: found, not fixed on any branch.

### What reproduces on plain upstream/master (from the upstream-PR effort, `pr/notes/INDEX.md`, wound down 2026-11-21 19:20)
The effort stopped on a change of priority: nothing was pushed, no PR or issue opened; 15 local branches `pr/*` off upstream/master ccf811e11, XLEN 64 only.
- **1 (DRAM)**: on the fork the SimX and rtlsim address maps agree at the defaults (both plain) and under `h200s` (both hashed, interleave 1024); they diverge only when `MEM_BLOCK_SIZE` is set below 64 (SimX takes the hashed map, rtlsim the plain one; review finding F2 on `pr/dram-interleave`). `SCHED_WINDOW=0` is accepted and hangs; the defaults use 32 and `h200s` 256, so no study result is affected. Static reading, not run.
- **2 (`bwstream -b`)**: `bwstream` and `bwstream_dxa` do not exist upstream; the PR (`pr/tests-bwstream`) adds both tests whole.
- **4 (DXA dispatch)**: on upstream's own tests the effect is small: rtlsim `sgemm2_dxa -n32` 203,317 -> 202,352 (-0.5%) at 4 and 8 workers; `dxa_copy` and `dxa_copy_mcast` unchanged. The -8.7% above was `bwstream_dxa -n8` on the fork's profile.
- **9 (dcache ordering)**: `war_order` fails on master at 35 of 60 sweep points (16x16 write-back 8/12, 16x16 behind an L2 4/12, 64x32 write-back 11/12, 64x32 behind an L2 12/12), none at the default 4x4; 0 with the fix.
- **11 (host address alias)**: new `tests/runtime/test_host_alias` segfaults 5/5 on simx (non-PIE, ASLR off); passes built PIE.
- **12 (DXA tag width)**: rtlsim does not elaborate at large worker x in-flight settings.
- **13 (B-buffer geometry)**: rtlsim stops with WGMMA at 16 threads / 8 banks and at 32 / 16.
- **26 (tile-buffer routing)**: reproduces on master with the test on `pr/simx-tbuf-route`.
- **33 (LocalMem wake)**: reproduces (above).

## Added 2026-11-21 late night (item 36; updates to 18, 22, 29, 31; plain-load calibration; measurement note M1)

### Update to 18 (barrier scope 1): the fixes are merged (18deea5da)
Sources: `git log 325ac78fb..18deea5da` (commit messages quoted below by hash).
- **What scope 1 waits for after the fix** (48c3dc8c9, f23ec2438, 6c8c7cc90): only the pending-instruction term is per warp. The "sent" and "landed" terms are core-wide and see loads as well as stores: requests still queued in any LSU block, and requests on their way to the data cache. The way to the cache now includes the request buffer of a flush lane's `VX_dcr_flush` arbiter, this core's lanes of the socket dMMU (VM) and the request side of the L1 link (`L1_LINK_LATENCY != 0`). Only reads waiting at the caches and memory no longer hold the barrier. The socket-wide `mmu_empty` (every core's loads and instruction fetches) is dropped from the scope-1 term. Local memory counts stores only; the link term counts requests only (6c8c7cc90: before it, the RTL also waited for other warps' load responses crossing the link back). SimX: a tensor-unit metadata read not yet taken into the LSU queue no longer holds the barrier, as in the RTL (d1d86fdcb). No flops added; scope 0 does not read these terms and is unchanged (demo 6,410/6,423, `sgemm2` 136,850/136,575, `bar_vis` 33,309/30,702 SimX/rtlsim, identical before and after).
- **What the core-wide terms still cost** (SimX, per-barrier wait recording, f23ec2438): decode on 1 core with 16 CTAs, mean 29.4 cycles per barrier, p99 595 (5% of it waiting for the warp's own instructions); the M=64 co-run's compute-bound core, mean 171, p99 2,981. That is about 30 cycles against about 3,000 between a warp's barriers in the decode (171 against about 13,000 on the compute core), about 1% of a walk. Store-only terms (store counts on every buffer of the path) are not done.
- **Parity cases added (SimX / rtlsim, cycles)**:

| Case | What it covers | SimX | rtlsim |
|---|---|---|---|
| `parity-vm-bar-vis-bar-warp` (6e135afd8) | VM on, two LSU blocks, megapage heap, odd CTAs streaming through the dcache MMU | 32,155 | 31,944 (+0.7%) |
| `parity-bar-vis-bar-warp-link` (02960b890) | `L1_LINK_LATENCY=8` | 30,860 | 30,876 |
| `bar_vis` at scope 1, one-bank dcache with 4 MSHRs, `-r64 -s1 -d0` (ccc0bcdc0) | back-pressured bank | 44,613 | 43,713 |
| the same with an 8-cycle link (ccc0bcdc0; after 6c8c7cc90) | bank + link | 45,934; 46,247 | 45,237; 45,231 |
| `model_parity-wgmma-sparse-fullk-bar-warp` (e30d82137) | sparse WGMMA (metadata reads share the LSU queue) | 595,714 | 599,084 (-0.6%) |

  The one-bank + link case was the one the old RTL condition missed: rtlsim 38,228 -> 45,237 with 48c3dc8c9 (from +20% against SimX to +1.5%).
- **A stale read could not be provoked on rtlsim before the fix** (48c3dc8c9): a directed attempt (a 1-bank dcache with 4 MSHRs back-pressuring the stores, also with link 8 + VM and with a dcache shared by two cores) opened the window, 131-1,643 cycles with a store at the flush lane's head while the old term reported it landed, but no barrier passed inside it in 1,024-2,048 barriers, and every run read the right values: the same back-pressure keeps other requests queued in the LSU, which the core-wide "sent" term waits for. The back-pressure cases are therefore regression checks, not detectors (ccc0bcdc0).
- **Remaining limitations, documented, not covered** (`docs/designs/lsu_pipeline_design.md` 5.4, 3840b80ba): a request already accepted by a sliced dcache request crossbar (fan-in > 1.5 x `MAX_FANOUT`) can still be overtaken there, and a shared dcache's core arbiter (one buffered `VX_mem_bus_arb` per lane in `VX_cache_cluster`) holds requests of different ports apart after accepting them. Both can reorder ports after the dcache's acceptance; the release-to-load path covers them only while the bank keeps up. The same holds at scope 0.
- **[corr. 2026-11-21] Decode figure**: scope 0 -> 1 on `h200s` is **15.08 -> 21.06 GB/s per SM (21.03 after the fixes; ratio 1.40)**, not "15.8 -> 22.2"; the M=64 role's walk 1,395,242 -> 816,228 becomes **~827 K** after the fixes (`open-threads.md`, 22:40). 1-core decode (`gemm_block -m16 -n8192 -k4096 -s0`): 6,310,272 -> 6,268,293 cycles with 48c3dc8c9.
- **Scope 2 was not built.** It was proposed in the load-calibration copy (`notes/ld_calib.patch`: a barrier waits only for its own warp's stores) because there `smprobe` with load-streaming neighbours did not finish at scope 1 (`results/ld_p1_k0n1`, `ld_p1_k1n1`: the log ends with the driver reporting that nothing executes; two scope-2 cells there, `ld_q2_k0n2`, `ld_q2_k1n2`, did not finish either). The starvation came from changes in that copy and did not reproduce on the work branch: `smprobe` finishes every cell on SimX and rtlsim. The table below is built from the raw runs (`results/barfix_sm_*`, `barfix_smr_*`, `barfix_smm_*`, copy `src_barfix_v5`); neither `open-threads.md` nor the merge's commit messages carry one. Probe iteration time, median cycles; all runs PASSED.

  SimX, `h200s`, 1 core, 16 CTAs, 200 iterations:

| probe (`-k`) | neighbours (`-n`) | scope 0 | scope 1 |
|---|---|---|---|
| shared memory + barrier (0) | idle (0) | 290 | 292 |
| | streaming loads (1) | 2,971 | 1,892 |
| | loads and barriers (2) | 7,962 | 1,580 |
| barrier only (1) | idle | 250 | 250 |
| | streaming loads | 1,397 | 1,125 |
| | loads and barriers | 7,232 | 1,487 |
| shared memory only (2) | idle | 260 | 260 |
| | streaming loads | 1,329 | 1,346 |
| | loads and barriers | 717 | 846 |

  rtlsim / SimX, 50 iterations, issue width 2: 16 warps x 16 threads, 2 CTAs (`-c2`): scope 0 probe 0 idle 264 / 259, probe 0 loads 390 / 400, probe 1 loads 333 / 254; scope 1 262 / 256, 293 / 287, 250 / 246. 32 warps x 16 threads, 4 CTAs (`-c4`): scope 0 probe 0 loads 509 / 503, probe 1 loads 460 / 278; scope 1 364 / 351, 327 / 338.

### Update to 22 (per-warp instruction rate): pipelined sequential fetch in RTL and SimX (`feature/warp-rate`, not merged, review pending)
Sources: `git log feature/dxa-borrowing..feature/warp-rate` (2f156bfc1, d434c1fb6, 1306fef6d, ea56a3ae3, 4c0cf2c81, 299df630b).
- **Design** (4c0cf2c81): with `VX_CFG_FETCH_INFLIGHT = N > 1` (a power of two, C extension off) a warp that is the only runnable warp of its frontend slice is picked again while it has fewer than N fetches between pick and decode, a free instruction-buffer slot, no squash to drain and, under VM, a next fetch in the page of its last one. Fetch is sequential (PC+4), several fetches in flight; there is no prediction. Decode checks each response as it arrives: a stalling instruction (control flow or a thread-mask change) is taken, squashes what was fetched behind it and stalls the warp, so fetch stops at a branch; a response of an older stream, or one that overtook an older fetch of its warp, is dropped (the latter restarts the stream at the overtaken fetch). RTL: per-warp in-flight count, run position, run, stream and drain bits in `VX_warp_sched`; `VX_fetch` tags icache requests {warp, position} with N tag entries per warp; checks in `VX_decode`. At N = 1 every added term is a generate branch tied to a constant (today's request tag, tag store, buffers and pick set). SimX mirrors it stage by stage and replaces the prototype's epoch.
- **Verified**: rtlsim vs SimX, 8 warps x 8 threads, issue width 4, N = 4, `FFLAGS_ACCUM=1`, `warp_rate` 13 classes x 1/2/4/8 warps: **49 of 52 rows equal to the cycle, the other 3 within 0.05%**; one warp `int_ind8` 1,379 -> 402 cycles per 128 instructions, `fma_ind8` 2,682 -> 939, `br_every1` 2,425 -> 2,397. SimX at the defaults equals its parent on all 52 rows. Test `fetch_squash` (299df630b) checks that nothing fetched on a wrong path executes (taken branches, jal/jalr, not-taken branches, back-to-back branches, a fetch overtaking an older one, FP flag access, a barrier); `model_parity` cases run `warp_rate` with both knobs on. Logic depth not measured.
- **Variants compared** at `h200s`, one warp, cycles per instruction (SimX; `results/wrr_dev/h200s_def_w1.txt`, `x_a1_w1.txt`, `x_b4_w1.txt`, `h200s_k4f_w1.txt`; H200 from `results_gpu/h200_warprate_summary.txt`). A = one fetch in flight, released at predecode; B = fetch a block of 4 words; C = N = 4 fetches in flight (chosen):

| class | today | A | B | C | H200 |
|---|---|---|---|---|---|
| `int_ind8` | 11.81 | 10.82 | 3.86 | 3.84 | 1.21 |
| `fma_ind8` | 21.57 | 10.82 | 3.58 | 3.84 | 1.07 |
| `br_every4` | 14.87 | 14.12 | 9.07 | 8.85 | 3.88 |
| `br_every16` | 12.65 | 11.72 | 5.55 | 5.49 | 2.13 |
| `br_every1` | 23.99 | 23.98 | 23.91 | 23.71 | 10.78 |
| `lds_own` | 11.64 | 10.65 | 6.35 | 6.41 | 3.27 |
| `sts_own` | 11.88 | 10.90 | 6.58 | 6.58 | 3.98 |

  A saves about one cycle; B and C are close, C was chosen. With C the **next per-warp limit is the issue path, ~3.8 cycles per instruction** against the part's 1.2.
- **Real-H200 evidence for "stop at a branch, no prediction"** (`results_gpu/h200_warprate_nottaken.txt`, source `gpu/warprate_nt_gen.py`): a not-taken conditional branch costs about what a taken one does (taken forward branch 11.4-12.1 cycles over the branch-free loop; a not-taken loop back-edge within 1-6 cycles of a taken one); with the same code, alternating and LFSR-random outcomes cost the linear mix of always and never taken (within 0.5%): no sign of a branch predictor or of any history effect.
- **Status**: not merged into `feature/dxa-borrowing`; review pending. Item 36 is a SimX fix found on the way.

### Update to 29 (fflags): `VX_CFG_FFLAGS_ACCUM` in RTL and SimX (`feature/warp-rate`, not merged, review pending)
- ea56a3ae3 (SimX prototype 1306fef6d): the FP exception flags get a scoreboard slot of their own (`XREG_FFLAGS`, a third xreg bit). An FP operation is counted while outstanding and does not reserve the slot, so a warp's FP operations no longer issue one at a time; a CSR access of fflags or fcsr waits for the count to reach zero and reserves the slot if it writes; an FP operation waits for that reservation. TCU metadata loads keep `XREG_0`/`XREG_1`, so with the knob they no longer wait for FP operations. At 0 the logic is today's; added state at 1: an 8-bit counter per warp and one xreg bit through the issue and commit payloads.
- rtlsim = SimX in every row (8 warps x 8 threads, issue width 4, `warp_rate`, cycles per iteration, one warp / 8 warps): defaults `fma_ind8` 2,686.38 / 2,681.62; with the knob `fma_ind4` 1,382.38 / 1,833.25, `fma_ind8` 1,387.88 / 1,838.62, `fma_dep` 2,611.00 / 2,606.50 (a dependent chain is unchanged). With pipelined fetch at `h200s`, `fma_ind8` 3.84 cycles per instruction (table above). Logic depth not measured. Not merged; review pending.

### Update to 31 (DXA socket-path register): the rtlsim reference
- `h200s`, `bwstream_dxa -e4096 -n32 -g16 -d4` (`results/par_h2_dxa32_rtl`, `par_chk_dxa_head`, `par_chk_dxa_fix`): **rtlsim 201,583 cycles; SimX today 182,421 (-9.5%); with the patch 219,161 (+8.7%)**. At `-n8` (`par_h2_dxa8_head`, `_fix`): rtlsim 60,307, SimX today 53,786, with the patch 58,201.
- Conclusion: the patch fixes a real one-cycle difference, but a second, depth-dependent difference remains, so the patch alone moves SimX from one side of rtlsim to the other. Not applied.

### 36. SimX freed a warp's instruction-buffer slot one cycle later than RTL `VX_ibuffer` (SimX/RTL divergence, timing)
- When an instruction arrives with its warp's buffer and staging register empty, `VX_ibuffer` pops it into the staging register in the cycle it arrives, so its buffer slot is free in that cycle; SimX freed the slot a cycle later.
- Invisible with one fetch in flight per warp (the slot count never reached the limit that way, so defaults do not move); with four fetches in flight it cost 8%.
- Fixed in SimX on `feature/warp-rate` in 4c0cf2c81, together with item 22's fetch; not merged.

## Plain-load calibration (not adopted)
Sources: `notes/ld_calib.patch` (load-calibration copy `src_ld`, SimX only, with path counters behind `PATH_STATS`), runs `results/ld_*` and `results/wrr_ld*`.
- **The L1 input limit.** A warp's 64 B coalesced line is two 32 B dcache requests to one bank: the dcache's read merge takes reads of the same word only. One SM streaming plain loads plateaus at **~139 GB/s per SM** (`ldstream` lean, one SM: L2-resident `ld_fa_ll_a32` / `_a64` 140.7 / 138.8; DRAM `wrr_ldl64_def` 139.0, `_k2f` 139.7, `_k4f` 138.4). The real H200's per-SM ingress is 215-230 GB/s (item 21, `h200-l2-structure.md`).
- **Candidate `VX_CFG_DCACHE_READ_MERGE_LINE`** (`ld_calib.patch`: the merge takes any reads of the same line, each port keeping its own tag and word, so the two 32 B words are one bank access; the RTL cache has this as `READ_MERGE_LINE`, which the icache uses). Reported at **183-185 GB/s** per SM; the run behind that figure is not among `results/ld_*`. In the runs that are there (`ld_msf_*` against `ld_mst_*`; the `msf` runs also set the patch's barrier scope 2, so they are not the merge alone): `ldstream` lean, 1 SM 130.2 -> 153.5 GB/s, 4 SMs 122.1 -> 144.0, 11 and 22 SMs unchanged (memory-bound); `gemm_block -s0` on 1 SM 768,809 -> 662,501 cycles; `gemm_tmem_decode` with load staging on 1 SM 1,815,684 -> 1,645,449.
- **Status**: kept as a candidate, not in `h200s`, until the per-warp rate work (item 22) lands.

## Measurement notes (real GPU; not Vortex items)

### M1. cuBLAS 13.4 returns NaN or wrong values for BF16 GEMMs under an MPS active-thread percentage
- Source: `h200-go-nogo.md` §0 and §3 (raw `results_gpu/h200_gng_c_diag.txt`, `h200_gng_c_gemm.txt`, `h200_gng_c_qwen_mask.txt`). PyTorch 2.15.0+cu132 with cuBLAS 13.4.1, driver 615.71.09.
- Qwen3.5-9B alone in an MPS client: wrong output (`!!!!`, the argmax of all-NaN logits) at 80/70/40/20% (104/92/52/26 SMs read back), correct at none/100/60/10% (132/132/78/12). First failing module `layers.0.mlp.gate_proj`, a plain BF16 `nn.Linear` with finite inputs. Standalone `F.linear` on Qwen's shapes: bad on 18 of 30 shapes at 80%, 7 at 60%, 25 at 40%, 0 at 100%. Not the script, uninitialised memory, Triton/FLA or the co-runner.
- With the SM allotment set by a TPC mask instead, the output is correct at every SM count (26-132).
- **Consequence**: every result taken under an MPS active-thread percentage with this software is invalid for correctness-sensitive work; the Qwen numbers under MPS 80/70/40/20% and the co-runs at LLM 20/40% (`corun-workloads.md`) came from runs computing NaN. TPC masks are used instead (they slow the host-bound decode 2.5x and hang CUPTI, so kernel times under a mask still need another method).

## How these were found (for the talk)
- Rule that produced most of them: match a real H200, never provision above it, and when a number looks wrong test several hypotheses instead of tuning.
- Wrong turns worth showing: bank count as the stream limit (artifact); "DXA is cluster-shared" (stale comment and design doc; it has been per SM since 7d6950de5); buffer overwrite as the M=256 cause (it was the dcache); the first dcache fix (opened a new hole, caught by review).
- Tools that paid off: rtlsim parity runs from frozen copies; the LSU shadow checker (`notes/m16_memchk_debug.patch`); a directed test per bug; independent review before pushing.
- Real-GPU measurements (`corun-workloads.md`): Z-Image-Turbo's DiT scales with SMs; **[corr. 2026-11-21]** it does not use "1.7% of memory": that counted weights only; measured with ncu it moves 178-209 GB per forward = 1.16-1.36 TB/s, 25-29% of the roof (`h200-go-nogo.md` §3). Qwen3.5-9B decode's matmul kernels use 65-68% of the memory bandwidth; **[corr. 2026-11-21]** the "72% of the speed on 20% of the SMs" point was taken under MPS 20% and is invalid (cuBLAS computed NaN, M1).
