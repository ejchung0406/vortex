# simx: order same-word dcache requests at admission, as the RTL does at S0

Branch `pr/simx-dcache-order`, one commit `534e91a8e` on `upstream/master` (ccf811e11).

Status (wind-down, 2026-11-21): **complete as a branch, verified at XLEN 64
only.** The commit is final and the worktree is clean. Intended base:
`upstream/master`, which is also its current base. The announced restacking of
`simx-dcache-fwd-hold` onto this branch is dropped. Still open: the three
questions at the end, and an independent review on this branch.

## Summary

SimX's data cache could return a younger store's data to an older load of the
same word. The RTL bank already keeps these in order; this change makes SimX
decide the same-word hazard where the RTL does, when the request enters the
bank, and adds a regression test, `war_order`, that fails on SimX before the
change and passes after it, and passes on rtlsim.

## Motivation

A store that hits skipped the line's MSHR chain at admission. Only an MSHR
lookup at the pipe exit kept it in order. An older load of the same word that
had already left the chain for replay was invisible to that lookup, so the
store merged first and the replayed load returned the younger store's value.
Write-back and write-through dcaches both had the hole.

The RTL handles this at S0. `VX_cache_bank.sv` demotes a same-word hit to a
chained miss while an older chain entry could still change the word (a chained
write) or observe it (any chained entry, against a write). The probes are in
`VX_cache_mshr.sv`.

The bug was first seen as wrong results in a 16-tile GEMM on a research fork:
1 to 30 of 262,144 outputs were off by 1-3%, and the host check's 1% tolerance
hid most of them. That is a fork measurement and is not reproduced here. The
evidence below comes from this branch.

`war_order` (new): every thread loads its own word of a cold line twice, stores
to it, reloads it, stores twice, reloads it, waits and reloads it again. Swept
delays move the stores across the line's fill, forward and replay. The host
checks every value each thread read back. 12 argument points per configuration,
SimX, XLEN 64, 1 core; failing points out of 12:

| configuration | upstream/master | this branch |
|---|---|---|
| 16 warps x 16 threads, write-back dcache | 8 (1-5 bad words each) | 0 |
| 16 x 16 behind an L2 (write-through dcache) | 4 (1-8) | 0 |
| 64 x 32, write-back dcache | 11 (1-4) | 0 |
| 64 x 32 behind an L2 | 12 (1-153) | 0 |
| default 4 x 4 | 0 | 0 |

Most failures set error bit 0x10: the second load of the initial value returned
a value stored later, which is the write-after-read inversion. The 64 x 32 L2
runs also set 0x2, 0x4 and 0x8 (a load after a store did not see it).

## What changed

- `sim/simx/mem/cache.cpp`
  - At admission, a load joins its line's chain behind an undrained same-word
    write, and a store joins behind any undrained same-word entry (new
    `has_word_write` / `has_word_reqs` MSHR probes). Every younger request's
    admission probe then sees it.
  - A write-through store with that hazard queues only its line merge in the
    chain (`merge_queued`). It still sends its memory write and response at the
    pipe exit, in pipeline order with the other stores.
  - At the pipe exit, a hitting store is deferred behind a same-word entry
    only, not behind any entry of the line. Loads keep their existing rule.
  - A hit demoted to a chained miss at the pipe exit counts as a read or write
    miss, as in the RTL bank.
- `tests/regression/war_order/` (new) and its entry in
  `tests/regression/Makefile`.
- `ci/testcases/cache.yaml`: `war_order-wb` and `war_order-wt` run on SimX at
  64 x 32 (write-back, and write-through behind an L2). Both fail on
  upstream/master. `model_parity-war_order-wb` runs at 16 x 16.
- `CHANGELOG.md`: an entry under Fixed.

No RTL, config knob or default changes.

## How verified

Build copies: `src_pr_simx-dcache-order` (this branch) and
`src_pr_simx-dcache-order_base` (upstream/master plus only the `war_order` test
files), both with the same `third_party`. Results are under
`vortex-exp/results/`.

| what | driver | config | result | results dir |
|---|---|---|---|---|
| war_order sweep, before / after | simx | 16x16, 1 core (WB dcache) | 8/12 fail / 0/12 fail | `pr_simx-dcache-order_{base,fix}_w16t16_x64` |
| same | simx | 16x16 + L2 (WT dcache) | 4/12 / 0/12 | `..._{base,fix}_w16t16_l2_x64` |
| same | simx | 64x32, 1 core | 11/12 / 0/12 | `..._{base,fix}_w64t32_x64` |
| same | simx | 64x32 + L2 | 12/12 / 0/12 | `..._{base,fix}_w64t32_l2_x64` |
| same | simx | default 4x4 | 0/12 / 0/12 (does not reproduce) | `..._{base,fix}_default_x64` |
| war_order -n65536 | rtlsim | 16x16 | pass, 1,296,287 cycles | `pr_simx-dcache-order_par_rtlsim_w16t16` |
| war_order -n65536 -s16 -d2, -s8 -d7 | rtlsim | 16x16 | pass, 1,461,416 / 1,810,623 | `pr_simx-dcache-order_rtl_w16t16_x64` |
| parity, SimX vs rtlsim cycles | simx | 16x16 | 1,295,606 / 1,460,824 / 1,810,519, within 0.05% of rtlsim (before: 1,295,994 / 1,460,339 / 1,810,726, wrong results) | `..._par_simx_w16t16`, `..._fix_w16t16_x64` |
| defaults unchanged: sgemm -n16, vecadd, dxa_copy -d2, sgemm_tcu (WGMMA), raw_order -n16384 -s64 -d2 (L2), lazy_fetch, ldst_order, 4-core sgemm -n32 (L2) | simx | defaults | identical cycles and every `PERF` line identical at `--perf=7` | `pr_simx-dcache-order_cmp_{base,fix}_*` |
| catalog lint (`python3 ci/testcase.py lint`) | - | - | OK, 966 cases | - |

The rtlsim runs use this branch's tree. The PR changes no RTL, so they stand
for upstream/master too.

At the 16x16 point with `-s16 -d2`, the dcache write-miss count moves toward
rtlsim (16,647 before, 16,697 after, rtlsim 16,866). Read misses are 29,690
before, 29,720 after, and 29,632 on rtlsim.

Not run:
- XLEN 32 (out of scope for this effort): verified at XLEN 64 only. The CI
  cases run at XLEN 32 and 64, and the `model_parity` case runs at XLEN 32
  only. To run them from a 32-bit build tree
  (`../configure --xlen=32`):
  `./ci/blackbox.sh --driver=simx --app=war_order --warps=64 --threads=32 --args="-n65536"`,
  the same with `--l2cache`, and
  `./ci/blackbox.sh --driver={simx,rtlsim} --app=war_order --warps=16 --threads=16 --args="-n65536 -s16 -d2" --perf=4`.
- xrt.
- `perf_gate`. The default-configuration runs above are cycle-identical, so no
  perf baseline should move, but the gate itself was not run.

## Change class

`python3 ci/change_impact.py explain --base upstream/master --head HEAD`: **R2**
(`path:simx-model`, `path:build-config`). Evidence checks:
- `simx-rtl-lockstep` (warn): SimX changed without RTL. This change corrects
  SimX toward rtlsim. The parity numbers above show it, and
  `model_parity-war_order-wb` keeps it checked.
- `configure-copied` (info): the tests Makefiles. All runs used freshly
  configured build trees.

Still required by the policy: a review of every hunk with `simx-review` by
someone other than the author (the same change was reviewed twice on the
research fork, not on this branch), and the maintainer's approval. No Beacon
run is needed because no RTL changed.

## Dependencies

None. The commit applies to `upstream/master` on its own. The sibling branch
`simx-dcache-fwd-hold` also edits `sim/simx/mem/cache.cpp`, in different hunks
(the forward-drain hold in `on_tick` / `processForward`, the response-queue
size in `CacheRspMcast`). `git merge-tree` of the two branches currently
reports no conflict.

## Open questions for the maintainer

1. **Loads demote on a write to any word of the sector; the RTL demotes per
   word.** At admission, a load joins a draining chain that holds any write or
   atomic of the sector (`must_order` / `has_ordered_reqs`). At the pipe exit,
   a hitting load is deferred under the same any-word rule. The RTL's load
   hazard is per word (`mshr_pending_wr_raw` uses `word_matches`). This is
   already on upstream/master, and this change does not alter it for loads. It
   costs SimX extra load misses and cycles where the RTL would hit, so it is
   timing and counter fidelity, not correctness. Should it be made per word in
   a separate change?
2. **LR/SC reservation and a deferred write-through line merge.** On a
   write-through cache that is the LLC (for example several cores without an
   L2), a store's reservation break happens when its memory write is emitted.
   The deferred line merge that replays later breaks nothing (`cache.cpp`
   around line 1985: the break is limited to `write_back`). An LR that sits in
   the same chain ahead of that merge, and replays after the store's exit,
   takes its reservation after the break and reads the line before the merge.
   An SC by that hart could then succeed over the store. This is reasoned from
   the code, not reproduced. The deferral path exists on upstream/master, and
   this change keeps that structure for the stores it orders. It is a separate
   atomicity question, not this load ordering bug, so it is left out. Should a
   write-through merge replay at the LLC also break other harts'
   reservations?
3. A store demoted at admission on a write-through cache holds its MSHR entry
   from admission and also its in-pipe reservation (`pending_mshr_size_`) until
   the pipe exit. That over-reserves by one entry for a few cycles and can add
   an admission stall only while the hazard is live. Acceptable, or should the
   pipe request stop counting itself?

---

## Notes for the sender

- Upstream/master does not build SimX with `-DVX_CFG_EXT_TCU_ENABLE` alone.
  `sim/simx/tcu/tcu_unit.cpp:1239` calls `decode_descs`, which is defined only
  under `VX_CFG_TCU_WGMMA_ENABLE` (guard ends at line 1129). This came with
  #488. It is unrelated to this PR and was found while running the
  default-cycle check (`results/pr_simx-dcache-order_cmp_base_sgemm_tcu`). That
  check was re-run with WGMMA on.
- The build copies' `third_party` comes from the fork's `src_all`, whose
  ramulator library also carries the fork's windowed controller. Upstream
  configurations do not select it, and base and branch share the copy, so the
  before/after comparisons are like for like.
- The 64 x 32 single-core shape in CI is large for a CI core, but SimX runs it
  in about 25 s per point. It is the shape that fails most reliably before the
  change, in both cache modes. 16 x 16 fails at 8 of 12 points, with 1 to 5 bad
  words each.
- The fork's first attempt (dab77b7f2) is not on this branch: it opened a new
  hole, where a younger load could miss the deferred store's bytes. Only the
  final state is here. The unrelated rtlsim runtime change in the fork's
  51a4cce00 is left to `runtime-cp-host-tag`.
- Sweep script: `vortex-exp/pr/scripts/simx-dcache-order_sweep.sbatch`.
