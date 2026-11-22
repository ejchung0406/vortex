# rtl/sim: barrier drain scope knob, VX_CFG_BAR_DRAIN_SCOPE

Status: **complete as a branch; independent review done with one F2 open (not fixed).** The code, tests and docs are committed. The brief's verification matrix ran (simulations at XLEN 64 only). Beacon and xrt did not run.

## Summary

A barrier is held at execute until the whole core's LSU has drained: every LSU
block's scheduler empty, including the reads every other warp and CTA on the
core has outstanding, and the MMU idle (`VX_wctl_unit.sv`, `bar_drain`; SimX
`SfuUnit`, `core_->lsu_drained()`). So one CTA's `__syncthreads()` waits for the
long-latency loads of every other CTA on the core.

This adds `VX_CFG_BAR_DRAIN_SCOPE`, in RTL and SimX together:

- `0` (default): today's core-wide drain, cycle-identical to master in both
  simulators.
- `1`: a barrier that stalls its warp (`vx_barrier`, `vx_barrier_wait`) waits
  for its own warp's instructions to commit, for every LSU block's request
  queue to be empty (requests sent; reads awaiting a response not counted),
  for no request in the global-store paths or at local memory's inputs and
  first engine, and for the MMU to be idle. An arrive (`vx_barrier_arrive`,
  `expect_tx`) keeps the core-wide wait.

No profile sets 1. A new directed test, `bar_vis`, checks that a CTA's shared
and global stores before a barrier are seen by its other warps after it, while
other CTAs keep reads outstanding.

## Motivation

The core-wide drain came from "rtl/sim: BAR opcode drains LSU before
suspending warps" (f9c79bb40, 2026-05-29). It gave `vx_barrier` the CUDA
`__syncthreads` / OpenCL `barrier(CLK_LOCAL_MEM_FENCE)` contract: stores before
the barrier are visible to reads after it. The case it fixed was WGMMA reading
shared-memory operands after a barrier while LMEM stores were still in flight
(rv64, 16 threads, NRC 32, sparse SS, K >= 64: numerical drift in SimX, large
negatives in rtlsim). Scope 1 keeps that ordering. What it drops is the wait
for reads that other warps and CTAs have outstanding. `bar.sync` does not need
that wait.

That wait couples CTAs. When several CTAs on a core stage tiles with loads, a
barrier passes only at instants when no read is in flight anywhere on the
core.

Measured on this branch, with an upstream test that runs four CTAs per core
doing loads and barriers (`sgemm2 -n128 -t8`, 16 threads, 16 warps, issue
width 2, XLEN 64):

| | SimX cycles | rtlsim cycles |
|---|---|---|
| scope 0 (= master) | 873,838 | 892,102 |
| scope 1 | 784,322 (-10.2%) | 790,945 (-11.3%) |

`bar_vis -w4` (two checking and two loading CTAs per core): SimX 33,309 ->
30,354 (-8.9%), rtlsim 30,702 -> 30,578 (-0.4%).

With one CTA per core, scope 1 is about 1% slower: `sgemm2 -n64 -t16` +0.9%
SimX and +1.1% rtlsim, `async_barrier` +0.9% and +1.1%. No other warp's reads
are there to skip, and the barrier now ends at a different point in the
schedule. The full table is under "How verified".

Fork measurements (from a research fork that uses this knob with fork-only
tests and a private H200-like profile; not reproducible from this branch):

- A load-staged decode kernel went from ~~15.8 to 22.2~~ **[corr. 2026-11-21]** 15.08 to 21.06 GB/s per SM
  (21.03 after the later scope-1 fixes, merged on the fork as 18deea5da).
- One kernel's walk went from 1,395,242 to 816,228 cycles (~827 K after those fixes).
- At scope 0, runs switched on their own between two states: CTAs in
  lockstep, or staggered and slower. At scope 1 this stopped.
- On a real H200, 16 CTAs per SM staging with loads took 2,503-2,536 cycles
  per iteration whether they started in step or staggered. A CTA doing
  register work and barriers beside CTAs streaming global loads went from 71
  to 79 cycles per iteration. Real hardware shows no barrier dependence on
  other CTAs' loads.

## Why scope 1 is sufficient (correctness argument)

`bar.sync` / `__syncthreads` must order each warp's shared-memory and global
stores issued before the barrier before the reads the CTA's other warps issue
after it. Release needs every warp of the CTA to arrive. So it is enough that,
at each warp's arrival, that warp's earlier stores can no longer be overtaken
by a later read from another warp. At scope 1, a stalling barrier arrives only
when:

1. The warp's own instructions have all committed (the pending-almost-empty
   bit WSYNC already uses). Nothing younger is in flight because the barrier
   stalls its warp. Its loads and atomics are answered. Its stores have been
   accepted by its LSU block's scheduler, since a store commits on acceptance.
2. Every LSU block's request queue and coalescer are empty (`req_queue_sent`).
   So the accepted stores have left the scheduler. This also covers the TCU
   metadata client that shares block 0's scheduler.
3. No request is in the global-store path (coalescer or lane adapter, up to
   the D-cache port), at local memory's inputs, or in its first engine
   (`lsu_store_landed`). Past these points a store sits in the D-cache bank
   pipeline or in the LMEM banks, which serve a later read to the same
   address after it. These stages drain in a few cycles and wait on no
   CTA's long-latency reads, so this term is core-wide rather than per-warp.
4. The MMU is idle (RTL only, as at scope 0).

An arrive keeps the core-wide wait because its warp runs on: younger
instructions of that warp could otherwise hold the per-warp test.

Terms 2 and 3 are the "store terms". At scope 0 the same window (store left the
scheduler but not yet in the cache or banks) is covered the same way, plus the
slack of waiting for every outstanding read on the core.

What `bar_vis` proves: in each of 16 rounds, the even CTAs store to a
shared-memory word and a global word, each warp as late as a per-warp delay
makes it, then pass `__syncthreads()`. Each thread then checks a word stored by
a thread of another warp. The odd CTAs walk a 4 MB buffer a line apart, so the
core always has reads outstanding. It passes at scope 0 and scope 1 on both
simulators.

It is a regression check, not a detector. On this branch I removed the store
terms, and then the drain altogether, in a scratch copy (a test-only macro in
`VX_wctl_unit.sv` and `sfu_unit.cpp`; not in the branch). `bar_vis` still
passes in SimX and rtlsim at every configuration I tried (results under "How
verified"): a store leaves the LSU sooner than the barrier's release reaches
another warp's read. So the store terms are a structural safeguard backed by
the argument above, not a demonstrated fix. No test here fails without them.

## What changed

RTL:
- `hw/rtl/libs/VX_mem_scheduler.sv`: new output `req_queue_sent` (request
  queue and coalescer empty; index buffer not counted).
- `hw/rtl/core/VX_lsu_scheduler.sv`, `VX_lsu_mem_path.sv`, `VX_lsu_block.sv`,
  `VX_lsu_unit.sv`, `VX_execute.sv`, `VX_execute_slice.sv`,
  `VX_backend_slice.sv`, `VX_compute_slice.sv`, `VX_frontend_xbar.sv`,
  `VX_core.sv`: carry `sched_sent` to `warp_ctl_if.lsu_sched_sent`, and
  `lsu_store_landed` (global-store paths empty AND local memory holds no
  store) to the interface.
- `hw/rtl/mem/VX_lmem_unit.sv`: new output `store_empty` (no store or atomic
  at an input or in the first engine). `hw/rtl/core/VX_mem_unit.sv` passes it
  out as `lmem_store_empty`.
- `hw/rtl/interfaces/VX_warp_ctl_if.sv`: two new signals, `lsu_sched_sent` and
  `lsu_store_landed`, in both modports.
- `hw/rtl/core/VX_wctl_unit.sv`: picks the scope-1 condition by generate on
  the knob. At scope 0 the new signals are unused.
- The other `VX_mem_scheduler` users (`om/VX_om_mem.sv`,
  `raster/VX_raster_earlyz.sv`, `raster/VX_raster_mem.sv`, `tex/VX_tex_mem.sv`)
  and the unit-test tops (`hw/unittest/lmem_unit`, `mem_unit`, `tensor`) leave
  the new outputs unconnected.
- No flops, only AND terms and wiring.

SimX:
- `sim/simx/lsu_unit.{h,cpp}`: `LsuUnit::sent()`.
- `sim/simx/core.{h,cpp}`: `Core::lsu_sent()` and `Core::lsu_stores_landed()`.
  The latter checks the lmem switch inputs, the coalescers' request side or
  the lane adapters, and local memory.
- `sim/simx/mem/mem_coalescer.{h,cpp}`: `req_idle()`.
- `sim/simx/mem/local_mem.{h,cpp}`: `holds_store()`.
- `sim/simx/sfu_unit.{h,cpp}`: `bar_lsu_drained()`, used where
  `lsu_drained()` was.

Config, docs and CI:
- `VX_config.toml`: `VX_CFG_BAR_DRAIN_SCOPE = 0`, with a comment.
- `docs/designs/lsu_pipeline_design.md`: new section 5.5, "What a barrier waits
  for".
- `CHANGELOG.md`: entry under Added.
- `ci/testcases/core.yaml`: `parity-sgemm2-lmem-iw2-bar-warp` and
  `parity-bar-vis-bar-warp` (scope 1).
- `ci/testcases/regression.yaml`: `bar-vis` and `bar-vis-warp-scope` (SimX and
  rtlsim, two LSU blocks, scope 0 and 1).

Test: `tests/regression/bar_vis/` (new), plus `tests/regression/Makefile`.

Profiles: none name the knob, and none are changed. The knob ships at 0.

## How verified

Simulations were verified at XLEN 64 only (XLEN 32 is out of scope for this effort). All runs use XLEN 64 with `VORTEX_VM_MEGAPAGE_HEAP=1`, one core, and the
configuration listed. Results are in `vortex-exp/results/pr_bds_*`.

The base is plain upstream/master (ccf811e11). For `bar_vis` the base copy
also has the test directory copied in, with no hardware change.

**Scope 0 is cycle-identical to upstream/master in both simulators**:
instruction counts and cycles are equal on all 10 tests and both drivers
(20/20).

| test, args | configs | SimX master = scope 0 | rtlsim master = scope 0 |
|---|---|---|---|
| bar_vis -w4 | NW16 NT8 IW2 | 33,309 | 30,702 |
| async_barrier | default | 118,732 | 118,746 |
| bar_slot_phase | NW2 | 64,552 | 64,565 |
| wsync | default | 372,606 | 374,669 |
| sgemm2 -n64 -t16 | NT16 NW16 IW2 | 136,850 | 136,575 |
| sgemm2 -n128 -t8 | NT16 NW16 IW2 | 873,838 | 892,102 |
| sgemm_tcu_wg | NT8 NW8 IW2, fp16/fp32, NRC16, 1 ALU/LSU/FPU block | 9,988 | 9,984 |
| sgemm_tcu_wg_sp -k128 | NT16 NW8 IW2, sparse, NRC32, 1 block each | 69,997 | 69,778 |
| dxa_copy -d1 | DXA | 6,466 | 6,470 |
| fence | default | 6,340 | 6,353 |

**Scope 1** (`-DVX_CFG_BAR_DRAIN_SCOPE=1`). Every test passes on both
drivers, and instruction counts match.

| test | SimX | rtlsim | SimX/rtlsim | vs scope 0 (SimX / rtlsim) |
|---|---|---|---|---|
| bar_vis -w4 | 30,354 | 30,578 | 0.7% | -8.9% / -0.4% |
| async_barrier | 119,776 | 120,023 | 0.2% | +0.9% / +1.1% |
| bar_slot_phase | 64,552 | 64,565 | 0.0% | 0 / 0 |
| wsync | 372,606 | 374,669 | 0.6% | 0 / 0 |
| sgemm2 -n64 -t16 | 138,107 | 138,088 | 0.0% | +0.9% / +1.1% |
| sgemm2 -n128 -t8 | 784,322 | 790,945 | 0.8% | -10.2% / -11.3% |
| sgemm_tcu_wg | 10,000 | 10,000 | 0.0% | +0.1% / +0.2% |
| sgemm_tcu_wg_sp -k128 | 69,765 | 71,124 | 1.9% | -0.3% / +1.9% |
| dxa_copy -d1 | 6,466 | 6,470 | 0.1% | 0 / 0 |
| fence | 6,340 | 6,353 | 0.2% | 0 / 0 |

The fork's own runs had 16 cases cycle-identical at scope 0 and scope 1
within 0.0-1.4%. Those are fork measurements. The tables above are from this
branch.

Observation: `bar_vis -w4` at scope 0 is 33,309 cycles in SimX and 30,702 in
rtlsim, 8.5% apart, on plain master too. So scope 0 for this test is outside
the default parity tolerance, before and after this change. The branch's
scope-0 `bar_vis` cases are regression (pass/fail) cases, not `model_parity`.
The `model_parity` case is at scope 1 (0.7%). I have not root-caused the
scope-0 gap.

**Drain-removal experiment** (`bar_vis`, scratch copy, scope 1 with terms
removed):

| variant | config | args | SimX | rtlsim |
|---|---|---|---|---|
| no store-landed term | NW16 NT8 IW2 | -w4 | PASS 30,311 | PASS 30,411 |
| no store-landed term | NW16 NT8 IW4 | -w4 | PASS 28,096 | not run |
| no store-landed term | NW16 NT8 IW2 | -w8 -s1 -d0 | PASS 23,364 | PASS 23,417 |
| own-warp commits only | NW16 NT8 IW2 | -w4 | PASS 30,224 | PASS 30,375 |
| own-warp commits only | NW16 NT8 IW4 | -w4 | PASS 28,053 | not run |
| own-warp commits only | NW16 NT8 IW2 | -w8 -s1 -d0 | PASS 23,241 | PASS 23,462 |
| no LSU wait at all | NW16 NT8 IW2 | -w4 | PASS 29,984 | PASS 29,903 |
| no LSU wait at all | NW16 NT8 IW4 | -w4 | PASS 27,700 | PASS 27,716 |
| no LSU wait at all | NW16 NT8 IW4 | -w4 -l256 -s8 -d4 | not run | PASS 33,132 |
| no LSU wait at all | NW16 NT16 IW4 | -w4 -s1 -d0 | not run | PASS 10,075 |
| no LSU wait at all | NW16 NT8 IW2 | -w8 -s1 -d0 | PASS 23,352 | PASS 23,777 |

The two SimX cells marked not run were running locally when work was stopped. I found no configuration where `bar_vis` fails.

**Unit tests and lint** (`hw/unittest`, XLEN 32, built with Verilator
`-Wall`):

- Build and run on the branch and on master: `lmem_unit`, `mem_unit`, `core`
  and `tex_core` pass on both. `core`, `lmem_unit` and `mem_unit` also pass
  at scope 1.
- `mem_scheduler` (runtime response-timeout assertion), `om_core` (UNDRIVEN
  lint error in `VX_om_bus_if`) and `raster_core` (C++ narrowing error in
  `main.cpp`) fail identically on plain master. These failures are not from
  this change.
- `VX_tensor_top` is a synthesis DUT, not a unit test. Verilator `--lint-only
  -Wall` with the catalog's tensor configuration: the same four pre-existing warnings (`VX_cache_wrap` width, an unused `VX_tensor_top` parameter, an unused `VX_mem_bus_if` bit) on master, on the branch and on the branch at scope 1; nothing new (`results/pr_bds_lint_tensor_{base,br,brs1}`).
- rtlsim builds with `-Wall`, so every rtlsim run above also elaborated and
  linted the core at that configuration, at scope 0 and scope 1.

**Change class**: `python3 ci/change_impact.py explain --base upstream/master
--head HEAD` gives **R3**. Detectors: `rtl-interface`
(`VX_warp_ctl_if.sv`: +`lsu_sched_sent`, +`lsu_store_landed`, both modports)
and `config-knobs` (`VX_config.toml`: added `pipeline.VX_CFG_BAR_DRAIN_SCOPE`).
Evidence note: `configure`-copied Makefiles changed (`tests/regression/Makefile`
and `bar_vis/Makefile`); every run above used a freshly configured build
tree.

**Not done here, and what the policy still requires**:
- Beacon (`logic_gate`): not run, because CIRCT and foldsim are not installed
  here. Commands:
  `ci/logic_gate.py -b core -b lmem -b tensor -b top_vortex --against-rev $(git merge-base HEAD origin/master)`,
  and the same with `--tool beacon_asic`. At the default scope 0 the new
  signals are unused, so I expect no depth change. The scope-1 path (one more
  AND of `lsu_sched_sent`/`lsu_store_landed` into `bar_drain`, which gates the
  wctl execute ready) is not covered by any DUT configuration. A run of the
  `core` DUT with `-DVX_CFG_BAR_DRAIN_SCOPE=1` would measure it.
- xrt: not run.
- Perf baselines: none should move, since the default is unchanged and
  scope 0 is cycle-identical. No baseline was touched.
- Maintainer approval, recorded in the PR (R3).
- Independent review (an agent that did not write the change; static only, deep SimX mapping not finished; verdict "land after fixes"). Findings open, none fixed on the branch:
  - **F2** (medium, reasoned-risk): `lsu_store_landed` (`VX_core.sv:154-156`) stops at the LSU adapter. It does not check stores in the D-cache flush unit's `REQ_OUT_BUF` (`VX_mem_unit.sv:255-262`, lane 0), the socket dMMU (VM), or the L1 link delay (`VX_socket.sv:221-237`; profiles set 4-34 cycles). The dcache crossbar is round-robin across ports (`VX_cache.sv:521-528`), so a load from another LSU block could overtake a store still waiting at a bank. Fix: add `~mem_unit_busy` (plus link and dMMU idle) to the scope-1 term, or narrow the claim in docs §5.5.
  - **F1** (lockstep): SimX's `lsu_stores_landed()` counts reads at the lmem switch inputs, and `lsu_sent()` counts `TcuReqIn`; the RTL terms do not.
  - **F1** (Cardinal Rule): `Core` reads child queues and `LocalMem` internals directly (open question 4).
  - **F1** (missing-test): `bar_vis` is not a detector (stated above).
  - **F1** (pre-existing): an arrive does not check its warp's pending stores.

## Dependencies

None. The branch is based on upstream/master (ccf811e11) and stands alone.

## Open questions for the maintainer

1. Should scope 1 become the default eventually? On upstream tests it is
   10-11% faster where several CTAs per core load and synchronise, and about
   1% slower with one CTA per core. Real hardware behaves like scope 1.
2. Is a knob wanted at all, or a straight change of the barrier rule with
   scope 0 removed?
3. Scope 1 still has core-wide terms: every block's request queue empty, and
   no store in the short post-scheduler stages. If other CTAs' loads keep a
   block's request queue backed up (pending pool full), a barrier can still
   wait on them. A per-warp count of queued requests would remove that.
   Worth a follow-up?
4. In SimX, `Core::lsu_stores_landed()` reads the state of the lmem switches,
   coalescers, lane adapters and local memory directly. This follows the
   existing `Core::lsu_drained()` / `has_pending_instrs()` pattern that the
   barrier gate already uses, rather than a channel. Acceptable, or should
   these units export a status signal instead?

---

## Notes for the sender

- Branch `pr/bar-drain-scope`, two commits on upstream/master ccf811e11:
  e746eed59 (rtl/sim knob) and 5485ca4ad (tests: bar_vis). The runs used build copies of 0770c8a1e, which has the same tree as 5485ca4ad; only the commit messages changed after.
- The three fork commits were reduced to two. The knob commit is the fork's
  9215d3f88 and caa4fcd4d squashed, so no intermediate state lacks the store
  terms. The test commit is the fork's 3f5adbbd0.
- Changes from the fork:
  - The `VX_config.toml` comment no longer cites the fork's H200 result files.
  - A stale `UNUSED_VAR (a_holds_write)` in `VX_lmem_unit.sv` is removed; the
    signal is now used by `store_empty`.
  - The new docs section was added.
  - The CHANGELOG entry points at the doc.
  - In `tests/regression/Makefile`, `war_order` (fork-only) is not added.
- The fork enables scope 1 only in its private `h200s` profile, which is not
  in this branch.
- Expected conflicts with sibling branches: `CHANGELOG.md` (Added list),
  `ci/testcases/core.yaml` (parity list), `ci/testcases/regression.yaml`,
  `tests/regression/Makefile` (TESTS list), and possibly `sim/simx/core.cpp`,
  `sim/simx/mem/local_mem.cpp` and `hw/rtl/mem/VX_lmem_unit.sv`.
- The scratch drain-removal copy is
  `vortex-exp/src_pr_bar-drain-scope_var` (macros `BAR_DRAIN_TEST_{1,2,3}`).
  It is not for upstream.
