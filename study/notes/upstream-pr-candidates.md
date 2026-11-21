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
| 9 | **SimX dcache returns a younger store's data to an older load** | **bug** (wrong results) | fixed on fork dab77b7f2 + 51a4cce00, test `war_order`, reviewed twice; rtlsim passes `war_order` (write-back 16x16: 1,296,287 cycles, SimX 1,295,606) | h200s-size rtlsim run (running); CI case |
| 10 | **SimX replays a forwarded fill on a full response queue; RTL holds** | **bug** (SimX/RTL divergence, timing) | fixed on fork cbc696a98 behind a knob; rtlsim 20,570 vs SimX 20,266 / 25,824 | make it the default |
| 11 | **Runtime: device address can alias a CP host region** | **bug** (crash or silent corruption) | fixed on fork b974a9e98 (simx), 51a4cce00 (rtlsim) | foldsim, firesim |
| 12 | **DXA memory tag too narrow for more in-flight slots** | **bug** (RTL does not elaborate) | fixed on fork 348263623, rtlsim builds and passes | review |
| 13 | **rtlsim stops at time 0 on H100-shaped profiles with WGMMA** (`VX_tcu_bbuf.sv` wants LMEM banks 32, profiles set 16) | **bug** (RTL/profile mismatch) | found; rtlsim runs disable WGMMA | fix comes with 14 |
| 14 | wgmma throughput: one shared B buffer puts the four tensor blocks in lockstep | performance | SimX patch `wgmma_tbuf.patch` (340 -> 1,326..2,989 FLOPs/cycle); not on fork | port count decision; RTL |
| 15 | FIFO L2 makes disjoint SMs slow each other | calibration | pseudo-LRU set in `h200s` (9c27e289a) | propose as a default? |
| 16 | L1 dcache and shared memory both at full size (484 KB vs the part's 256 KB pool) | profile accuracy | found, in every profile from Volta on | decide the split |
| 17 | Tensor memory engine: B prefetch two blocks ahead and A read by bank rows (on `upstream/feature/tmem-dataflow`) | performance (design change) | SimX patch `tmem_roof.patch`: 1,789 -> 3,684 FLOPs/clock/SM with one 128 B port (part: 3,786); not on the fork yet | merge the branch, RTL counterpart, rtlsim parity |
| 18 | A barrier waits for the whole core's LSU to drain (cause of the two-state load-mode rate) | design property, performance | found and traced; per-warp drain experiment in SimX (x1.5-1.7 for load-mode staging) | decide whether to change it (RTL + SimX) |

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
  (`sim/simx/scoreboard.cpp`); no effect at 64 warps. Low priority.
- The `memory: lat=` perf counter wraps on large runs.
- TLB reach: a decode GEMM over 128 MiB of weights needs `DTLB_SIZE` and
  `L2_TLB_MEGA_SIZE` of 256 with 2 MB pages, and `VORTEX_VM_MEGAPAGE_HEAP=1`
  (without it the run measures page walks).
- The debug build of SimX does not compile (`__assert` clash).

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
SM's DXA core, still derived from the total core count: 1 worker at 1-8 SMs,
2 at 22, 16 at 132. An SM's TMA capacity depends on how many SMs the run has,
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
- **18 (barrier drain)**: implemented as `VX_CFG_BAR_DRAIN_SCOPE` in RTL and SimX (9215d3f88, caa4fcd4d), merged (7658376d9). Scope 1: a stalling barrier waits for its own warp's requests plus stores in flight past the LSU. 16 cases cycle-identical at scope 0 in both simulators; scope 1 passes all, SimX/rtlsim within 0.0-1.4%. h200s: load-staged decode 15.8 -> 22.2 GB/s per SM, the M=64 role's walk 1,395,242 -> 816,228, no two-state behaviour. History: the core-wide drain came from f9c79bb40 (2026-05-29) to give CUDA `__syncthreads` semantics; a real H200 shows no dependence of a barrier on other CTAs' loads (E4). No test fails even with the drain removed, so the store terms are a structural safeguard, not a demonstrated fix.
- `h200s` now sets barrier scope 1, FEDP2K, tensor memory (1,024 columns), B ahead 2, A bank-row (dc9fdeef4).

## 19. New kernels on the tensor-memory engine (tests, not on the fork yet; `notes/gemm_tmem.patch`)
- `gemm_tmem` (compute-bound, M large, weights reused): 3,608 FLOPs per clock per SM over the multiply phase, 3,334 over the kernel (part: 3,786); holds at 1/2/4 SMs; DRAM traffic 0.6% of an SM's fair share with reuse. Needs weights and activations packed by K step (2,557 with plain layouts).
- `gemm_tmem_decode` (memory-bound, M = 4/16/64, each weight byte read once): 82 GB/s per SM (gemm_block: 26-28; real decode on few SMs: 88-104). Bound by the shared-memory DMA port: one client per cycle, and a DXA worker writes one 64 B line per beat. Scaling 82 / 153 / 266 / 595 / 729 GB/s at 1 / 2 / 4 / 11 / 22 SMs (memory saturates between 10 and 22 SMs).

## 20. DXA and shared-memory port against the real part (calibration, in progress)
- Real H200, one SM: TMA 16 KB x 1/2/4/8 = 30/57/104/142 GB/s; 32 KB x 1/2/3/6 = 52/99/142/208; ceiling 210-219. Shared memory: reads 253, TMA writes 215, staged 157-170. Vortex: DXA copy tops out at 101-111 (one 64 B/clock gmem port), and the LMEM DMA port is exclusive between DXA writes and tile-buffer reads. Experiments running (ports, 128 B write beats, port arbitration, per-transfer cost).

## 21. What the real H200 measurements changed (study direction, not an upstream item)
- One SM alone reaches ~210 GB/s; the "100 GB/s per SM" was 4 x 16 KB in flight. Software borrowing through thread-block clusters works but adds nothing. A GPC returns at most ~1.28 TB/s (about 6 SMs); spreading memory-bound SMs over the 8 GPCs gives 2.4x over packing; MPS appears to pack. Co-residency starves the memory job (0.6-22%). See `tpc-placement-and-borrowing.md`, `noc-and-design-space.md`.

## How these were found (for the talk)
- Rule that produced most of them: match a real H200, never provision above it, and when a number looks wrong test several hypotheses instead of tuning.
- Wrong turns worth showing: bank count as the stream limit (artifact); "DXA is cluster-shared" (stale comment and design doc; it has been per SM since 7d6950de5); buffer overwrite as the M=256 cause (it was the dcache); the first dcache fix (opened a new hole, caught by review).
- Tools that paid off: rtlsim parity runs from frozen copies; the LSU shadow checker (`notes/m16_memchk_debug.patch`); a directed test per bug; independent review before pushing.
