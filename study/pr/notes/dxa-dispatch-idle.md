# dxa: dispatch prefers a worker with no transfer running

## Summary

_Status (sender note, delete before posting): complete on its current base, verified at XLEN 64 only. No independent review was run._


A DXA worker accepts a new request as soon as its setup stage is free. That
happens while its previous transfer is still draining, because the setup
engine prepares the next transfer in the background. The dispatch took the
lowest-numbered ready worker, so a request that arrived while worker 0 was
copying was set up on worker 0 and waited for that copy, while other workers
sat idle. With this change the dispatch prefers a ready worker that has no
transfer running, in both the RTL and SimX. With one worker, which is the
default below nine cores, nothing changes.

## Motivation

`VX_dxa_setup` keeps `req_ready = (setup_state_r == SS_IDLE)`. That is
independent of `state_r` (TS_ACTIVE while a transfer runs). `VX_stream_dispatch`
picks the first ready output with a priority encoder. So with several workers
and a request arriving more than the setup latency after the previous one,
the request is staged on worker 0 behind its running transfer. Worker 1 and
the others stay idle. SimX's setup step did the same: it walked the workers
in order and staged the queue head on the first worker with no staged
request, whether that worker was running or not.

Measured on upstream/master (ccf811e11): with four workers, `sgemm2_dxa
-n32 -t4 -m1` is *slower* than with one, on both drivers (rtlsim 203,317 vs
202,997 cycles; SimX 204,066 vs 203,361). The extra workers do not help
because the second tile load of a CTA is queued behind the first on worker 0.

On a bandwidth microbenchmark that keeps many transfers in flight, the effect
is larger. These are fork measurements, made on a research fork with a
fork-only test and an H200-like profile, not on master: `bwstream_dxa -n8`,
one SM, 8 workers, rtlsim 66,067 -> 60,307 cycles (-8.7%) and SimX 61,767 ->
57,629 (-6.7%).

## What changed

- `hw/rtl/dxa/VX_dxa_worker.sv`: new output `xfer_idle = ~transfer_active`
  (the setup stage's TS_IDLE state).
- `hw/rtl/dxa/VX_dxa_core.sv`: collects the workers' `xfer_idle` bits and
  passes them to the dispatch.
- `hw/rtl/dxa/VX_dxa_dispatch.sv`: new input `prefer[NUM_OUTPUTS]`. The
  ready vector given to `VX_stream_dispatch` is the ready workers that are
  also preferred, or all ready workers when none is preferred. The dispatch
  accepts a request under the same condition as before (some worker ready);
  only the choice of worker changes. Added logic: NUM_DXA_CORES AND gates, an
  OR-reduce over those NUM_DXA_CORES bits and a NUM_DXA_CORES-wide 2:1 mux in
  front of the existing priority encoder. Both inputs come straight from
  registers (`setup_state_r`, `state_r`). At one worker it reduces to the
  old wire (`r & (p | ~(r & p)) = r`).
- `sim/simx/dxa/dxa_core.cpp`: the setup step makes two passes over the
  workers. In the first, idle workers start a staged transfer or take the next
  request. In the second, the rest is staged behind running transfers. A
  worker still does not take a request in the cycle it starts one (as the
  single-pass `else if` did before), and `reset()` clears that per-worker
  cycle stamp.
- `ci/testcases/dxa.yaml`: new `model_parity-sgemm2-dxa-workers`, which runs
  `sgemm2_dxa -n32 -t4 -m1` with `-DVX_CFG_NUM_DXA_CORES=4`. Every existing DXA
  case and perf baseline runs one worker, so no gate saw the choice.
- `CHANGELOG.md`: a Changed entry.

## How verified

Build copies of the branch and of upstream/master (ccf811e11). XLEN 64 (the
riscv32 toolchain is not installed on the machine used), one core, default
config plus `-DVX_CFG_EXT_DXA_ENABLE` and the worker count shown. rtlsim in
two batched Slurm jobs, SimX in local build trees. Results are under
`vortex-exp/results/pr_ddi_<test>_w<workers>_<base|br>_<simx|rtlsim>/`.
Every run printed PASSED, and instruction counts are identical between
drivers and between trees.

| test | workers | SimX master | rtlsim master | gap | SimX branch | rtlsim branch | gap |
|---|---|---|---|---|---|---|---|
| sgemm2_dxa -n32 -t4 -m1 | 1 | 203,361 | 202,997 | 0.18% | 203,361 | 202,997 | 0.18% |
| sgemm2_dxa -n32 -t4 -m1 | 4 | 204,066 | 203,317 | 0.37% | 202,987 | 202,352 | 0.31% |
| sgemm2_dxa -n32 -t4 -m1 | 8 | 204,066 | 203,317 | 0.37% | 202,987 | 202,352 | 0.31% |
| dxa_copy -d1 -t0 4 | 1 | 6,613 | 6,614 | 0.02% | 6,613 | 6,614 | 0.02% |
| dxa_copy -d1 -t0 4 | 4 | 6,538 | 6,542 | 0.06% | 6,538 | 6,542 | 0.06% |
| dxa_copy -d1 -t0 4 | 8 | 6,538 | 6,542 | 0.06% | 6,538 | 6,542 | 0.06% |
| dxa_copy -d1 | 1 | 6,466 | 6,470 | 0.06% | 6,466 | 6,470 | 0.06% |
| dxa_copy -d1 | 4 | 6,466 | 6,470 | 0.06% | 6,466 | 6,470 | 0.06% |
| dxa_copy -d1 | 8 | 6,466 | 6,470 | 0.06% | 6,466 | 6,470 | 0.06% |
| dxa_copy_mcast | 1 | 3,559 | 3,561 | 0.06% | 3,559 | 3,561 | 0.06% |
| dxa_copy_mcast | 4 | 3,563 | 3,563 | 0.00% | 3,563 | 3,563 | 0.00% |
| dxa_copy_mcast | 8 | 3,563 | 3,563 | 0.00% | 3,563 | 3,563 | 0.00% |

- 1 worker (the default formula gives 1 for every shape the CI catalog runs):
  identical to the cycle on both drivers for all four apps, as the
  construction implies.
- `dxa_copy -d1` has one transfer outstanding at a time, so it cannot move.
  `dxa_copy -d1 -t0 4` (four one-warp CTAs) and `dxa_copy_mcast` do not move
  either. The likely reason, inferred and not traced: their requests reach the
  dispatch within the setup latency of each other, so worker 0's setup stage
  is still busy and the next request already goes to another worker.
  `sgemm2_dxa`'s second tile load evidently arrives after worker 0's setup is
  free again, which is the case this change addresses.
- `hw/unittest/dxa_core` builds and PASSES at the default on both trees. At 4
  and 8 workers it fails to build on both trees with the same Verilator
  WIDTHEXPAND in `VX_mem_bus_arb.sv:97/126` (the harness's `lmem_arb` tag),
  so that failure predates this change and is unrelated (results/pr_ddi_ut_{base,br}). The full GPU
  (rtlsim) builds lint-clean at 1, 4 and 8 workers with the change.
- `python3 ci/testcase.py lint`: OK (964 cases).
- What the new parity case can and cannot catch: at 4 workers the two
  models move together (SimX -1,079, rtlsim -965 cycles) and stay 0.31%
  apart. A change on one side only would leave them 0.85% apart (branch
  rtlsim 202,352 vs master SimX 204,066), well inside the 5% tolerance. So the
  case puts the multi-worker dispatch under the gate's instruction-exact and
  cycle checks, but it does not by itself detect a one-sided change of this
  size. The before/after numbers above are the evidence that both sides
  moved.
- Verified at XLEN 64 only. SimX and rtlsim at XLEN 32, the model_parity gate's width, are out of scope and were not run. The
  WGMMA+DXA case (`sgemm_tcu_wg_dxa`, `model_parity-wgmma-dxa`'s config) at 4
  workers was not run: its rtlsim build was killed at 24 GB. Beacon was not run
  either (no CIRCT/foldsim here).

## Change class

`ci/change_impact.py explain --base upstream/master --head HEAD`: **R2** (reasons `path:rtl`, `path:simx-model`; no R3 detector hit: the new parity case is an added case, and no baseline or knob changes).

Still required, and not done here:
- Beacon on the DXA DUT: `ci/logic_gate.py -b dxa --against-rev $(git
  merge-base HEAD origin/master)`. The catalog's `dxa` DUT builds one worker,
  where the new logic folds away, so it cannot show the multi-worker cost
  (expected +1 to +2 LUT levels on the dispatch's `ready -> valid` and queue
  pop paths at 8 workers).
- The new parity case at XLEN 32 in CI.
- Maintainer approval (R2).
- An independent review of the diff (none was run).

Perf baselines: none move. Every `ci/baselines/perf/dxa.json` entry runs one
core, hence one worker, where the change is the identity (shown above to the
cycle on rtlsim at XLEN 64).

## Is "idle" the same in both models; ordering

- RTL: a worker is preferred while `state_r == TS_IDLE` in `VX_dxa_setup`,
  that is from the cycle after `transfer_done` (the last LMEM write *request*
  of a transfer fires) until the next promotion. SimX: `WState::IDLE`, from
  `finish_worker` (all lines written) until `start_worker`. A worker that is
  idle but holds a staged request is not ready in either model, so it is never
  chosen.
- No request starves. The dispatch accepts the queue head exactly when some
  worker is ready, as before, and requests stay in order. A high-numbered
  worker may go unused, which is harmless.
- Completions can finish in a different order than before. Two transfers that
  used to run back to back on worker 0 can now run at once on two workers and
  finish in either order. With several workers this was already possible
  before, whenever worker 0's setup stage was busy. Each transfer completes its
  own barrier arrival (counted per transfer), and nothing in the DXA ABI
  promises completion order between transfers.

## Dependencies

The branch sits on upstream/master (ccf811e11) and builds and passes there. The intended base is
`pr/tests-bwstream`, which adds `bwstream_dxa`, the fork test that shows the
effect best. A `bwstream_dxa` multi-worker parity case was planned once that branch is final, but it was not done
(the effort was wound down). The branch has not been rebased. `pr/dxa-mem-tag-width` also edits `ci/testcases/dxa.yaml`, in a
different place (after `dxa_copy-2c2s`), so there is no textual conflict.

## Open questions for the maintainer

- `docs/designs/dxa_async_copy_multicast.md` already says the dispatch routes
  "to the first idle worker". Before this change the RTL took the first
  *ready* one, so this change makes the code match the document; the document
  is not edited.

- `sgemm2_dxa-5` to `-10` in `ci/testcases/dxa.yaml` set
  `-DVX_CFG_NUM_DXA_UNITS=4`. No such knob exists, so those cases run one
  worker.
- Should the parity case instead be the existing `model_parity-sgemm2-dxa`
  (`-n128`) with more workers? Its rtlsim run is ~8.9 M cycles. The new case
  is ~200 K.

---

## Notes for the sender

- Branch `pr/dxa-dispatch-idle`, one commit (56d2fd657) on upstream/master ccf811e11. The runs used the code of 08a453708, which differs from 56d2fd657 only in `CHANGELOG.md` and `ci/testcases/dxa.yaml`.
- The fork's `bwstream_dxa` numbers are fork measurements (h200s profile,
  fork-only test); drop them if you prefer master-only evidence.
- Results: `vortex-exp/results/pr_ddi_*`. Scripts:
  `vortex-exp/pr/scripts/dxa-dispatch-idle_*`.
