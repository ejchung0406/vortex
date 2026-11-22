# Upstream PR branches: index (final, wound down 2026-11-21 19:20)

The effort was stopped early on the user's change of priority. Nothing was
pushed and no PR or issue was opened. All branches are local, off
`upstream/master` ccf811e11, in worktrees under
`/storage/project/r-hkim358-0/echung67/vortex-exp/pr/<slug>`. Every result is
XLEN 64 only (no 32-bit toolchain here). Beacon (`logic_gate`) and xrt ran
for no branch. "Note" is `pr/notes/<slug>.md`.

The lead wrote this index from the notes, the branch heads and the result
directories; most implementers' final messages did not reach the lead, so
where a note is a draft the state below is what the lead could see, not what
the agent may have known.

## Branches

| slug (item) | commits | state | class | reproduces on upstream/master | verified (run) | not run / missing | review |
|---|---|---|---|---|---|---|---|
| runtime-cp-host-tag (11) | bcb703c71 | complete | R2 | yes: new `tests/runtime/test_host_alias` segfaults 5/5 on simx (non-PIE, ASLR off); passes when built PIE | simx before/after, defaults; firesim syntax only | foldsim (not installed), xrt | no independent review seen |
| dxa-mem-tag-width (12) | 5d48b6c1a | complete | R2 (gpu-pkg detector: see note) | yes: rtlsim does not elaborate at large worker x in-flight settings | rtlsim before/after, defaults cycle-identical | xrt, Beacon | none spawned |
| simx-dcache-order (9) | 534e91a8e | complete | R2 | yes: `war_order` fails at 16x16 write-back and 64x32; not at default 4x4 | SimX before/after, rtlsim parity at 16x16, defaults | perf gate | open questions in note (LR/SC reservation, per-word demotion); no separate review seen |
| simx-tbuf-route (26) | 94d9d2bec | complete | R2 | yes (the fix was written on the tmem merge, but the bug is on master too) | SimX reproducer before/after, defaults identical | rtlsim of the reproducer: see note | review still open |
| tcu-bbuf-geometry (13) | 6e20ef775 | complete | R2 | yes: rtlsim stops with WGMMA at 16 threads/8 banks and 32/16 shapes | rtlsim + SimX at the failing shapes, defaults identical on both drivers | Beacon, some larger shapes | none seen |
| tests-bwstream (2) | 6161fba5f, c6b1d85c7 | complete | R2 (Makefiles), rest R1 | n/a (new tests) | SimX + rtlsim default and 64w x 32t (16,942 vs 16,377) | `make -C tests/regression` whole build: see note | findings open: stronger address checking (note, end) |
| bar-drain-scope (18) | e746eed59, 5485ca4ad | complete as a branch, **one F2 open** | R3 (knob, RTL interface) | n/a (design change, default unchanged) | scope 0 cycle-identical to master on both drivers; scope 1 passes on both | Beacon, xrt; `bar_vis` never fails even with the drain removed | reviewed: F2 open (below), four F1 |
| tests-gemm-block (3) | 076f64d3b | partial | R2 | n/a | rtlsim at registered shapes | all SimX legs (upstream SimX build break, below), whole-regression build | none |
| dram-hbm3e (1a) | 8230e48a6 + 1 uncommitted file (`ci/testcases/core.yaml`) | partial: note is a draft | R3 (knob enum) | n/a | unit test, some SimX/rtlsim | review fixes below not confirmed | reviewed: land after minor fixes |
| dram-sched-window (1b) | 857d99c26 + 1 uncommitted file | partial: note has unfilled sections | R3 expected | n/a | not confirmed | `SCHED_WINDOW=0` hang fix not confirmed | reviewed: land, follow-ups |
| dram-interleave (1c) | 60b564e7d on top of dram-hbm3e + 1 uncommitted file; no note | partial, **F2 open** | R3 expected | n/a | reviewer verified locate() is a bijection | F2 fix (default must be plain) not confirmed committed | reviewed: do not land until F2 fixed |
| simx-dcache-fwd-hold (10) | a1cced425, 58a10526a ("draft") | partial: note is a 3-line draft | not classified | not established on this branch | ~330 result dirs `results/pr_fh_*` exist, not tabulated | everything in the note | none |
| simx-l2-fwd-hold (23) | none (branch not created) | not started | - | - | - | all | - |
| dxa-dispatch-idle (4) | 08a453708 ("wip") + 1 uncommitted file; no note | partial | not classified | n/a | rtlsim master vs branch, upstream tests (below) | SimX after-numbers, parity case, note | none |
| simx-lmem-wake (unlisted fork fix fad1f31d7) | none; the 10-line fix is uncommitted in the worktree; no note at the time of writing | partial | not classified | **yes**: SimX `sgemm2_dxa -n64 -t8 -m1`, 16 warps x 16 threads, stalls on master (run stops at the 100,000-cycle no-progress limit with every warp parked at one PC) with 2, 4 or 8 DXA workers on 1 core, with 4 workers on 4 cores, and at 16 cores with the DEFAULT worker formula (2 workers); passes with 1 worker | with the fix all of these pass (309,094 / 311,340 / 307,346 / 97,692 / 42,206 cycles); rtlsim on master passes at 4 workers (311,222); results `results/pr_simx-lmem-wake_*` | commit, CI case, note; 1-worker cycles differ slightly with the fix (308,369 -> 308,445), not explained; some 8x8 shapes also stall on master, fix side not run | none |

Backup branch `pr/tests-bwstream-prev-msgs-backup` is an older copy of
tests-bwstream (the agent made it before rewording commit messages).

### Numbers the lead read directly from result directories

dxa-dispatch-idle, rtlsim, upstream/master vs branch (`results/pr_ddi_*`):
`dxa_copy` and `dxa_copy_mcast` unchanged at 4 and 8 workers (6,470 / 6,542 /
3,563 both sides); `sgemm2_dxa -n32` 203,317 -> 202,352 (-0.5%) at 4 and at 8
workers; default worker count unchanged (202,997). So on upstream tests the
gain is small; the fork's -8.7% was `bwstream_dxa -n8` on its own profile.

### Open review findings

- bar-drain-scope F2 (medium, argued from code): at scope 1 the store check
  stops at the LSU adapter; it does not cover stores still in the dcache flush
  unit's output buffer (`VX_mem_unit.sv:255-262`), the socket dMMU under VM, or
  the L1 link delay (`VX_socket.sv:221-237`). A load from another LSU block
  could overtake such a store. Fix: add mem-unit/link/dMMU idle to the scope-1
  condition. **The fork's `h200s` profile runs scope 1, so this applies to the
  fork too.** F1s: SimX counts reads and TCU requests in its scope-1 terms and
  the RTL does not; SimX `Core` reads child state directly; `bar_vis` is not a
  detector.
- dram-interleave F2 (static proof): SimX sets `block_size =
  VX_CFG_MEM_BLOCK_SIZE` while the interleave defaults to
  `PLATFORM_MEMORY_DATA_SIZE` (64); with `MEM_BLOCK_SIZE` 8 or 16 (CI cases
  sgemm-11/13, mstress-3/5) SimX takes the hashed map and rtlsim the plain one.
  Fix: default 0 = plain. F1s: sizes not validated at configuration; rtlsim
  AXI path ignores the knobs silently; stale doc text.
- dram-sched-window: `SCHED_WINDOW=0` is accepted and hangs; generated
  controller lacks a Makefile prerequisite and a guard.
- dram-hbm3e: rtlsim honours bank groups where foldsim refuses; a unit-test
  comparison has no margin; one dead timing value. JESD238 values not checked
  by the reviewer.

### Same problems on the fork (`feature/dxa-borrowing`), read from its code

- Interleave: the fork has the same code (`sim/simx/processor.cpp:57`,
  `mixed()` is `interleave > block_size`). At fork defaults
  (`MEM_BLOCK_SIZE` = `DATA_SIZE` = `INTERLEAVE_SIZE` = 64) both simulators
  use the plain map: no problem. Under `h200s` no profile key changes
  `MEM_BLOCK_SIZE` or `DATA_SIZE` (both 64) and the interleave is 1024, so
  both simulators use the hashed map and agree. The divergence exists on the
  fork only when `MEM_BLOCK_SIZE` is set below 64. Static reading; not run.
- `SCHED_WINDOW=0` would hang on the fork as well; defaults use 32 and
  `h200s` 256, so no study result is affected.

## If any of this is sent later: order and hand work

1. runtime-cp-host-tag, dxa-mem-tag-width, simx-dcache-order,
   tcu-bbuf-geometry, simx-tbuf-route (independent, complete).
2. tests-bwstream; tests-gemm-block after the upstream SimX build break is
   fixed and its SimX legs are run.
3. dxa-dispatch-idle and simx-dcache-fwd-hold were meant to be stacked on
   tests-bwstream (their evidence tests live there), and fwd-hold also on
   simx-dcache-order (same file, `sim/simx/mem/cache.cpp`). The restack was
   not done: all branches still sit on upstream/master.
4. bar-drain-scope after its F2; DRAM as hbm3e, sched-window, interleave
   (sched-window conflicts with hbm3e in 7 files, mechanical).
By hand for every branch: XLEN 32, Beacon, xrt, maintainer approval; for the
partial ones, finish as the table says.

## Issues only (paste-ready, 3-5 lines each)

1. **simx: dcache can return a younger store's data to an older load of the
   same word.** A store that hits skips the MSHR chain and is ordered only at
   the pipe exit, where an older load already dequeued for replay is
   invisible; the store merges first. The RTL orders at S0
   (`creq_hit_order_hazard_st0`). Reproducer `war_order` (branch
   `pr/simx-dcache-order`): fails on master at 16x16 write-back and 64x32,
   not at 4x4.
2. **simx: a forwarded fill is replayed when the bank's core-response queue
   is full; the RTL holds it** (`VX_cache_bank.sv`, `fwd_head`). Fork
   measurement, 1 core, 64 warps x 32 threads, `bwstream -b32`: rtlsim 20,570
   cycles, SimX 25,824 replaying, 20,266 holding. The same applies to the L2
   banks. Not re-established on upstream/master in this effort.
3. **runtime (simx, rtlsim, firesim, foldsim): a device address can alias a
   command-processor host region.** The CP address of a host region is the raw
   pointer and the dram hooks route by "start inside a region"; a non-PIE
   host heap sits among device addresses. A 64 MB `vx_copy_to_dev` on one core
   segfaults in `make_cp_hooks` on master (5/5 with ASLR off). Fix on
   `pr/runtime-cp-host-tag`.
4. **rtl: the socket memory tag is not sized for the DXA.** The tag is the
   maximum over the L1, texture and ray caches; with more DXA workers x
   in-flight slots (`VX_CFG_NUM_DXA_CORES`, `VX_CFG_DXA_MAX_INFLIGHT`) the DXA
   tag does not fit and rtlsim fails to elaborate. Table of failing pairs in
   `dxa-mem-tag-width.md`.
5. **config: `VX_CFG_NUM_DXA_CORES` sizes each socket's DXA engine from the
   total core count.** Full text in `issue-dxa-worker-count.md`. Workers per
   socket are 1 for 1-15 cores, 2 for 16-23, 16 at 132; the comment and
   `docs/designs/dxa_async_copy_multicast.md` still describe a cluster-level
   pool; `VX_CFG_DXA_MEM_PORTS` is derived and read nowhere.
6. **tcu: rtlsim stops at time 0 with WGMMA when LMEM banks differ from the B
   block's words** (`VX_tcu_bbuf.sv` assertion), which is the shape of the
   `h100`/`h200` profiles (32 threads, 16 banks). SimX has no such check.
   Reproduced on master at 16 threads / 8 banks and 32 / 16.
7. **simx: a tile-buffer word is routed by comparing descriptor bases.** When
   an A row and B start at the same shared-memory address, B's reads go to
   the A buffer and the run aborts with "TCU buffer miss"; the RTL keeps
   separate buffers. Reproduced on master with the test on
   `pr/simx-tbuf-route`.
8. **simx does not build on master in a TCU configuration without WGMMA**
   (a `decode_descs` guard; reported independently by three agents; exact
   macro set and error in `tests-gemm-block.md`, Dependencies). Not fixed on
   any branch.
9. **dxa: the dispatch takes the first ready worker, not the first idle one**
   as `docs/designs/dxa_async_copy_multicast.md` says; a request can wait
   behind worker 0's transfer while others idle. rtlsim, `sgemm2_dxa -n32`, 4
   workers: 203,317 -> 202,352 cycles with idle-first.
10. **A barrier waits for the whole core's LSU to drain** (`VX_wctl_unit.sv`,
    `bar_drain`; from f9c79bb40), so one CTA's barrier waits for every other
    CTA's outstanding loads. A narrower scope is on `pr/bar-drain-scope`
    behind a knob, with an open correctness finding.
11. **simx: LocalMem can sleep past a packet reserved toward it in the same
    cycle, and the device stalls** (its sleep test looks at delivered packets
    only). On master, SimX `sgemm2_dxa -n64 -t8 -m1` at 16 warps x 16 threads
    stalls with `VX_CFG_NUM_DXA_CORES` 2, 4 or 8, and at 16 cores with the
    default formula; rtlsim passes. Fix: fork commit fad1f31d7 (count queued
    and in-flight packets), uncommitted in worktree `pr/simx-lmem-wake`.

## Disk created by this effort (nothing deleted)

Under `/storage/project/r-hkim358-0/echung67/vortex-exp/`:
- `src_pr_*`: 31 build copies, about 348 MB each, about 10.8 GB.
- `pr/<slug>`: 14 worktrees, about 259 MB each, about 3.6 GB; `pr/notes`
  111 KB; `pr/scripts` 204 KB (57 files).
- `results/pr_*`: about 720 result directories (small logs).
- node-local, will vanish with the node: `/tmp/echung67/pr_*` (8 build trees).
Git: 15 local branches `pr/*` in the fork repository. Remove a worktree with
`git worktree remove <path>` before deleting its branch.
