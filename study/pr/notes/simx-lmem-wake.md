# simx: keep the local memory awake for a packet still in flight to it

Branch `pr/simx-lmem-wake`, one commit `53f5a0b34` on `upstream/master` (ccf811e11).
Files: `sim/simx/mem/local_mem.cpp` (+10/-1), `ci/testcases/dxa.yaml` (+16),
`CHANGELOG.md` (+7).

Status (wind-down, 2026-11-21): **the hang reproduces on upstream/master (run)**;
the fix is committed and the worktree is clean. Every run cited below finished
before the wind-down; no job of this branch is queued or running. Verified at
XLEN 64 only (XLEN 32 out of scope); independent review not done.

## Summary

SimX's `LocalMem` could go to sleep while a DXA row write was on its way to
it. Nothing woke it when the row arrived, so the row stayed unserved until
some other packet reached the unit. On upstream/master this hangs SimX: with
two or more DXA workers every warp stalls at its transfer barrier, and with
the default single worker a kernel whose last rows were left behind never ends.
The fix makes the sleep test count in-flight packets (`size()`), as the
tick-gating contract and the cache do. The RTL has no sleep and is unchanged.
A new CI case, `sgemm2_dxa-workers`, hangs on upstream/master and passes with
the fix.

## Motivation

The tick-gating contract (`sim/common/simobject.h`, `SimObjectBase::tick_sleep`)
lets an object drop out of the tick scan when "all watched input channels
[are] empty, nothing in flight toward them". `SimChannel::reserve()` (run by
`send`) wakes the endpoint's object; `receive_packet()` (the delivery, one or
more cycles later) does not. So the contract holds only if a sleeping object
counts reserved-but-undelivered packets, which `size()` does and `empty()`
does not.

`LocalMem::Impl::tick()` slept on `idle && !dma_pending()`, and `dma_pending()`
tests `empty()` on the DMA inputs. The DXA core is created in
`Socket::Impl`'s constructor (`sim/simx/socket.cpp:211`) before the cores
(`socket.cpp:329`), so it ticks first. In a cycle where the unit is otherwise
idle:

1. the DXA core sends a row to a core's DMA input: `reserve()` wakes the unit
   (it is awake anyway);
2. the unit ticks, sees the input empty (not delivered yet) and sleeps;
3. the row is delivered after the ticks; the unit stays asleep.

The row is served when the next packet is reserved toward the unit. That
packet may never come:

- **Two or more workers.** A DMA input holds two packets
  (`SimChannel` default capacity). Two workers can fill it in step 1. Both
  rows are delivered, the input is full, every worker gates its next beat on
  room in all of the core's DMA inputs (`dxa_core.cpp:718-721`), so nothing is
  reserved toward the unit again. The warps wait at their transfer barrier and
  the scheduler's no-progress timeout fires. `VX_CFG_NUM_DXA_CORES` is the
  worker count per socket and is 2 by default from 16 cores
  (`max(1, up(NUM_CORES / 8))`, `VX_config.toml:427`), so a default 16-core
  build hangs too.
- **One worker.** The worker's next row re-arms the unit, so a transfer in
  the middle of a kernel only loses time. The barrier is released by the
  channel's `tx_callback` at delivery (`socket.cpp:417-423`), not when the unit
  writes the row, so warps can finish while rows wait. At the end of a kernel
  the waiting rows keep the platform from being idle (`SimPlatform::idle()`
  counts unconsumed packets), `Processor::run` waits for idle
  (`processor.cpp:344`), and the run never ends. No warp is active, so the
  scheduler timeout does not fire either.

Other inputs. `ReqIn` is fed by the lmem switches, created after the unit
(`core.cpp:209-227`), and the TCU tile buffer's DMA client is created after it
as well (`core.cpp:353-366`); their reservations come after the unit's tick
and wake it. The fix counts `ReqIn` too, so the test does not depend on
creation order.

Other SimX units that sleep on `empty()`: `CpLlcPort` (`cp_llc_port.h:131`)
sleeps only with no read and no fence outstanding, so no response can be in
flight; `FlushTap` (`mem/cache.cpp:3098`) is fed with a zero-delay response
from its non-cacheable arbiter (2 inputs, 1 output: `rsp_delay` 0,
`types.h:1862-1870`), delivered before it ticks, and the comment at its
creation (`cache.cpp:3166`) records that order; `CacheRspMcast`
(`cache.cpp:2994`) is created before the banks that feed it, so their
reservations wake it after its tick. None of these was changed; no hang was
seen or argued for them.

The fork this came from saw the stall on an H200-quarter shape (64 warps x 32
threads, `VX_CFG_NUM_DXA_CORES >= 2`, down to one core) and measured a 0.14%
shorter single-worker run (2,532,751 -> 2,529,311 cycles). Those are fork
measurements on a fork-only test; everything below was run on this branch and
on plain upstream/master.

## What changed

- `sim/simx/mem/local_mem.cpp`: the sleep test requires `size() == 0` on every
  DMA input and every request input (plus the existing engine, response and
  DMA-slot state). `dma_pending()` is unchanged and still decides the DMA slot.
- `ci/testcases/dxa.yaml`: `sgemm2_dxa-workers`, simx and rtlsim, XLEN 64,
  `sgemm2_dxa -n32 -t4 -m1`, `-DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_NUM_DXA_CORES=2`,
  8 warps x 8 threads, `touches` includes `sim/simx/mem/local_mem`.
- `CHANGELOG.md`: a Fixed entry.

## How verified

Build copies: `src_pr_simx-lmem-wake_base` (upstream/master ccf811e11) and
`src_pr_simx-lmem-wake` (this commit; tracked files checked identical to
`git archive HEAD`), both with third_party from `src_all` and the Ramulator
extension rebuilt by upstream's `third_party/Makefile`. Runs through
`run_point.sbatch` (XLEN 64, `--perf=6`, `VORTEX_VM_MEGAPAGE_HEAP=1`), results
under `vortex-exp/results/pr_simx-lmem-wake_{base,fix}_<tag>`. "16x16" is
`-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=16`; all DXA runs add
`-DVX_CFG_EXT_DXA_ENABLE`. A hang on SimX prints
`*** cluster0-socket0-core0-scheduler timeout: cycles=100000, every active warp stalled:`
with every warp at the PC after its `vx_bar` (0x1800003ec in sgemm2_dxa).

Reproducer and fix (SimX):

| tag | app, args | cores | workers | shape | upstream/master | this branch |
|---|---|---|---|---|---|---|
| sg2dxa_w2_c1_16x16 | sgemm2_dxa -n64 -t8 -m1 | 1 | 2 | 16x16 | scheduler timeout | 309,094 PASSED |
| sg2dxa_w4_c1_16x16 | same | 1 | 4 | 16x16 | scheduler timeout | 311,340 PASSED |
| sg2dxa_w8_c1_16x16 | same | 1 | 8 | 16x16 | scheduler timeout | 307,346 PASSED |
| sg2dxa_w4_c4_16x16 | same | 4 | 4 | 16x16 | scheduler timeout | 97,692 PASSED |
| sg2dxa_dflt_c16_16x16 | same | 16 | default (2) | 16x16 | scheduler timeout | 42,206 PASSED |
| x_w2_8x8_n32t4 (CI case) | sgemm2_dxa -n32 -t4 -m1 | 1 | 2 | 8x8 | scheduler timeout | 66,892 PASSED |
| tcu_wg_dxa_w4_m128 | sgemm_tcu_wg_dxa -m 128 -n 64 -k 32, CI config of sgemm_tcu_wg_dxa-1 | 1 | 4 | 8x32 | scheduler timeout | 10,281 PASSED |
| dxa_copy_16x16 | dxa_copy -d2 -s0 128 -s1 128 -t0 8 -t1 8 | 1 | default (1) | 16x16 | never ends (job cancelled after 37 min) | 95,527 PASSED |
| sg2dxa_w1_c1_16x16 | sgemm2_dxa -n64 -t8 -m1 | 1 | 1 | 16x16 | 308,369 PASSED | 308,445 PASSED |

Other multi-worker shapes that hang on upstream/master (base side only):
`x_w2_8x8_n64t4`, `x_w4_8x8_n32t4`, `x_w4_16x16_sg2m2` (`-m2`). Multi-worker
shapes that pass on upstream/master: `x_w2_16x16_n32t8`, `x_w4_8x8_n64t8`,
`x_w4_4x4_n32t4`, `x_w4_16x16_mcast`, `dxa_copy_w4_16x16` (95,151 on both
sides), `tcu_wg_dxa_w4` (10,578 / 10,580).

Mechanism check, diagnostic build (`src_pr_simx-lmem-wake_diag`, upstream/master
plus prints, not a branch; results `pr_simx-lmem-wake_diag_*`): counts sleeps
with a DMA packet in flight and stops a run whose cores are done while the
platform stays busy for 200,000 cycles.
- `diag_sg2dxa_w2_8x8`: two such sleeps, at cycle 1,051 (1 packet) and 1,056
  (2 packets: the input full), then the scheduler timeout.
- `diag_dxa_copy_16x16` (one worker): 1,024 such sleeps, the last at cycle
  95,456; then "cores done but platform not idle for 200000 cycles" at 295,528.
- `diag_sg2dxa_w1_16x16` (one worker): 1,524 such sleeps; the run recovers and
  passes at 308,369 cycles, as on upstream/master.

rtlsim (no sleep in the RTL; local_mem.cpp is not compiled into rtlsim):

| config | upstream/master | this branch | SimX with the fix |
|---|---|---|---|
| sgemm2_dxa -n64 -t8 -m1, 1 core, 4 workers, 16x16 | 311,222 PASSED | 311,222 PASSED | 311,340 (+0.04%) |
| CI case sgemm2_dxa -n32 -t4 -m1, 2 workers, 8x8 | 66,016 PASSED | 66,016 PASSED | 66,892 (+1.3%) |
| sgemm_tcu_wg_dxa -m 128 -n 64 -k 32, 4 workers | 10,955 PASSED | not run | 10,281 (-6.2%) |

Retired instructions match SimX in all three (115,136 / 36,608 / 2,436).

Defaults and single-worker runs, upstream/master vs this branch (SimX,
cycles):

| test (CI case it mirrors) | upstream/master | this branch |
|---|---|---|
| dxa_copy -d1 (model_parity-copy) | 6,466 | 6,466 |
| dxa_copy -d2 (dxa_copy-2) | 53,990 | 53,990 |
| dxa_copy -d3, 2 cores (dxa_copy-3) | 16,674 | 16,674 |
| dxa_copy_mcast (dxa_copy_mcast-1) | 3,559 | 3,559 |
| sgemm2_dxa default (sgemm2_dxa-1) | 36,270 | 36,270 |
| sgemm2_dxa default, 2 cores (sgemm2_dxa-3) | 18,551 | 18,551 |
| sgemm2_dxa -n128 -t4 -m1 (model_parity-sgemm2-dxa) | 9,049,339 | 9,049,107 (-232, -0.003%) |
| sgemm_tcu_wg_dxa (sgemm_tcu_wg_dxa-1) | 11,321 | 11,321 |
| sgemm_tcu_wg, 8x8, ISSUE_WIDTH=2, WGMMA (tensor_wg) | 7,642 | 7,642 |
| sgemm -n16 | 18,364 | 18,364 |
| sgemm2 | 38,980 | 38,980 |
| vecadd | 1,631 | 1,631 |

Instruction counts are equal in every pair.

Checks: `python3 ci/testcase.py lint` OK (965 cases); `ci/check_simx_mt_boundary.sh`
passed; SimX builds with `-Werror` in every configuration above.

Not run / failed:
- XLEN 32: no 32-bit toolchain in this environment, so nothing above ran at
  XLEN 32. That is why the new case is pinned to XLEN 64, and why it is a
  plain simx+rtlsim case rather than a `model_parity` case (the gate is pinned
  to XLEN 32 by `ci/testcase.py`).
- `sgemm_tcu` without WGMMA (`-DVX_CFG_NUM_THREADS=8 -DVX_CFG_EXT_TCU_ENABLE
  -DVX_CFG_ISSUE_WIDTH=2 -DITYPE=fp16 -DOTYPE=fp32`, CI sgemm_tcu-3): failed
  to build on upstream/master and on this branch alike,
  `sim/simx/tcu/tcu_unit.cpp:1239: 'class vortex::TcuUnit::Impl' has no member
  named 'decode_descs'` (the call is outside the `VX_CFG_TCU_WGMMA_ENABLE`
  block that defines the function, `tcu_unit.cpp:760-1129`). Unrelated to this
  change; replaced by `sgemm_tcu_wg` as the TCU control.
- Model-parity gates at XLEN 32, perf_gate, xrt: not run.
- No independent review: the review agent started for it ended without a
  report. The diff was checked against the `simx-review` checklist only by its
  author.

## Change class

`python3 ci/change_impact.py --repo . --policy docs/review/change-impact.yaml explain --base upstream/master --head HEAD`:
**R2** (`path:simx-model`; evidence `simx-rtl-lockstep` warn: "SimX model
changed with no RTL change: confirm it is functional only, or that it corrects
SimX toward rtlsim"). It corrects SimX toward rtlsim: rtlsim passes every
configuration that hung, and SimX now passes them within 0.04-1.3% of rtlsim
(the 6.2% gap on the 4-worker WGMMA shape cannot be compared with
upstream/master, which hangs there). CHANGELOG R0, `ci/testcases/dxa.yaml` R1.

Still required by the policy: a reviewer's pass over every hunk with the
`simx-review` skill; maintainer approval. Perf baselines: the DXA baselines in
`ci/baselines/perf/dxa.json` are rtlsim cases only, and the RTL is unchanged,
so none moves. The SimX leg of `model_parity-sgemm2-dxa` moves by -232
cycles at XLEN 64 (see table); its XLEN 32 value was not measured. No
Beacon run (no RTL).

## Dependencies

None. Merges with every sibling branch except a textual conflict in
`CHANGELOG.md` with `pr/dxa-mem-tag-width` (neighbouring Fixed entries).
`pr/bar-drain-scope` also edits `local_mem.cpp` (adds `holds_store()` after
`perf_stats()`); `git merge-tree` merges the two cleanly.

## Open questions for the maintainer

1. Should the new case also run at XLEN 32, or become a `model_parity` case?
   It was verified at XLEN 64 only.
2. Existing cases `sgemm2_dxa-5` to `-10`, `sgemm2_dxa-l2-*` and
   `sgemm2_dxa-npot-*` pass `-DVX_CFG_NUM_DXA_UNITS=4`, the knob's name before
   7179aba90 renamed it to `VX_CFG_NUM_DXA_CORES`. Those cases therefore run
   one worker, which is why CI did not see this. Should they move to the new
   name (in this PR or a separate one)? Their shapes were not run here with 4
   workers.
3. The 4-worker WGMMA shape is 6.2% faster on SimX than on rtlsim. Is a
   multi-worker parity gap of that size known?

---

## Notes for the sender

- Commit trailers use `Co-Authored-By: Claude Opus 5.5` (the model that made
  the commit), not the "Claude Fable 5.2" line of the brief; amend if the
  series must match.
- The `sgemm_tcu` build failure on upstream/master (above) is a separate
  upstream bug, probably from eaea0c79a (#488), the last commit to touch
  `tcu_unit.cpp`. It was not bisected.
- 20 of this branch's pending Slurm jobs were cancelled by the same account at
  19:19 (not by this agent) and were resubmitted; the results directories hold
  only the resubmitted runs.
- The base job `pr_simx-lmem-wake_base_dxa_copy_16x16` was cancelled by this
  agent after 37 minutes of no progress; its `meta.txt` says so.
- Helper scripts: `vortex-exp/pr/scripts/lmem_wake_pair.sh` (submits a
  base/fix pair), `lmem_wake_table.sh` (tabulates the results),
  `lmem_wake_local.sh` (local runs, used only for the first sgemm2_dxa
  timeout).

### Findings-list entry

**SimX local memory slept with a packet in flight to it (bug, hang;
reproduces on upstream/master, branch `pr/simx-lmem-wake`).** Problem:
SimX hangs with DXA. With two or more workers per socket every warp stalls at
its transfer barrier. With the default single worker a kernel can fail to end
after every core is done. Cause: `LocalMem` slept when no DMA input held a
*delivered* packet (`empty()`). The DXA core ticks before the cores, so a row
sent that cycle was still in flight, and a delivery does not wake a sleeping
object. With two workers both slots of a DMA input fill in that cycle and no
worker can send again. With one worker, rows left behind at the end keep
`SimPlatform::idle()` false. Fix: the sleep test counts queued and in-flight
packets (`size()`) on the DMA and request inputs, as the tick-gating contract
and the cache do (`sim/simx/mem/local_mem.cpp`; same change as fork
fad1f31d7). Evidence, upstream/master vs fix, SimX, XLEN 64: `sgemm2_dxa -n64
-t8 -m1` at 16x16 times out with 2/4/8 workers on 1 core, 4 workers on 4
cores, and the default (2 workers) on 16 cores; it passes with the fix at
309,094 / 311,340 / 307,346 / 97,692 / 42,206 cycles. rtlsim passes the
4-worker shape at 311,222. `dxa_copy` 128x128 with 8x8 tiles, 16x16, default
single worker, never ends on master (1,024 lost wake-ups, the last at cycle
95,456) and passes with the fix at 95,527. The smallest hang found is
`sgemm2_dxa -n32 -t4 -m1`, 8x8, 2 workers, the new CI case (rtlsim 66,016,
SimX fixed 66,892). Default and single-worker CI shapes are cycle-identical,
except `sgemm2_dxa -n128 -t4 -m1` (-232 cycles) and a 16x16 sgemm2_dxa
(+76). CI missed it because the multi-worker DXA cases still pass the old
knob name `VX_CFG_NUM_DXA_UNITS` and run one worker. Status: branch ready
(commit 53f5a0b34, R2), verified at XLEN 64 only; independent review not done.
