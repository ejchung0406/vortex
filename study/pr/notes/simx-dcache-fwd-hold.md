# simx: hold a fill-forward head on a full core-response queue

## Summary

When the head of a fill's pending chain is a plain read and the bank's
core-response queue is full, the RTL bank keeps the head waiting
(`VX_cache_bank.sv:1356-1360`: `fwd_head` does not depend on
`crsp_queue_ready`; it masks the replay and the fill, and core requests keep
entering). SimX sent the head down the replay path instead. The replay took the
input slot from core requests, then stalled the bank pipeline at commit while
the queue stayed full.

This branch makes SimX hold the head, unconditionally, in every SimX cache bank.
The RTL has no option here, and every RTL cache (dcache, icache, L2) is the same
`VX_cache_bank`. A second commit makes SimX size the dcache bank's core-response
queue from `VX_CFG_DCACHE_CRSQ_SIZE`, which the RTL reads and SimX ignored.

Commits on `pr/simx-dcache-fwd-hold` (off upstream/master ccf811e11):
- `afe8c2b52` simx: hold a fill-forward head on a full core-response queue
- `b069acbc8` simx: size the dcache core-response queue from VX_CFG_DCACHE_CRSQ_SIZE

## Motivation

Under streaming loads that a narrow LSU drains slowly, the replay costs SimX
several percent of cycles against rtlsim. Every number below comes from this
branch's build copies at XLEN 64. "SimX before" is upstream/master; it is
cycle-identical, in all 48 parity cases run, to the fork's knob version with
the knob off.

| case (XLEN 64) | SimX before | SimX branch | rtlsim | before -> after vs rtlsim |
|---|---|---|---|---|
| new `parity-sgemv-lsu-lanes` (sgemv -m512 -n512, 16w x 16t, 4 LSU lanes) | 592,967 | 564,882 | 559,514 | +6.0% -> +1.0% |
| `parity-sgemm-mc64` | 128,836 | 127,551 | 127,568 | +1.0% -> -0.0% |
| `parity-sgemm-mc` | 966,089 | 970,932 | 973,183 | -0.7% -> -0.2% |
| `parity-sgemm-mc64-l1link` | 141,609 | 142,715 | 143,369 | -1.2% -> -0.5% |
| `parity-raycast-nt16` | 3,510,116 | 3,492,776 | 3,495,885 | +0.4% -> -0.1% |
| `parity-raycast-mc` | 4,875,772 | 4,872,019 | 4,872,755 | +0.1% -> -0.0% |
| `parity-sgemm-mc64-l2link` | 148,367 | 147,157 | 148,022 | +0.2% -> -0.6% (away) |
| `parity-printf-ss2` | 372,477 | 371,216 | 373,068 | -0.2% -> -0.5% (away) |
| `parity-sgemm-fw2` | 408,850 | 408,957 | 405,603 | +0.80% -> +0.83% |
| bwstream -b32, 64w x 32t, 4 banks, 8 LSU lanes, MSHR/LSU pending 512 | 16,699 | 16,024 | 16,034 | +4.1% -> -0.1% |
| the same with 32 LSU lanes | 15,677 | 15,677 | 15,340 | no change (+2.2%) |
| bwstream -b32, 32w x 32t, 8 LSU lanes | 13,383 | 13,133 | 13,218 | +1.2% -> -0.6% |
| bwstream -b16, 16w x 16t, 4 LSU lanes | 13,834 | 13,722 | 13,836 | -0.0% -> -0.8% (away) |
| vecadd -n65536, 32w x 32t | 51,280 | 52,095 | 52,852 | -3.0% -> -1.4% |
| vecadd -n65536, 16w x 16t, 4 LSU lanes | 296,948 | 294,062 | 294,259 | +0.9% -> -0.1% |
| packld, 16w x 16t | 28,714 | 28,139 | 28,703 | +0.0% -> -2.0% (away) |
| sgemv -m512 -n512, 16w x 16t | 357,656 | 353,323 | 362,070 | -1.2% -> -2.4% (away) |
| uniform_ld, 32w x 32t | 176,704 | 176,632 | 177,809 | -0.6% -> -0.7% |

Over the 18 rows, the mean |gap| to rtlsim goes from 1.3% to 0.8% and the
largest from 6.0% to 2.4%. Five rows move away from rtlsim by more than 0.1
point, by up to 2 points. The change is the RTL's structure. I have not explained the rows that
move away; they point at another gap in the response path, which this branch
does not touch.

The fork's evidence (labelled fork measurements: `bwstream -n64 -r1 -p0 -u4
-b32` on the fork-only `h200s` profile, rtlsim 20,570, SimX 25,824 replay /
20,266 hold) reproduces on upstream RTL only with a narrow LSU. At 64w x 32t
with 32 LSU lanes the hold changes nothing. With 8 LSU lanes (the H100/H200
profiles' value) SimX goes from +4.1% to -0.1% (table). On SimX only, with
`--profile=h200 --cores=1` plus MSHR/LSU pending 512, it goes from 27,314 to
24,829 (rtlsim not run for that profile).

## What changed

- `sim/simx/mem/cache.cpp`: `processForward()` returns true when it completes
  the head or holds it for a full `core_rsp_out`. The replay slot then goes to
  core requests. Fills already wait while the forward window is open. A
  response sent by the pipeline in the same tick still sends the head down the
  replay path, as in the RTL (`~crsp_queue_valid` in `fwd_head`).
- `docs/designs/cache_subsystem.md` §4.6: one sentence on the full-queue case.
- `ci/testcases/core.yaml`: new `parity-sgemv-lsu-lanes`.
- `CHANGELOG.md`: Fixed entry.
- Commit 2: `Cache::Config::crsq_size`; `socket.cpp` passes
  `VX_CFG_DCACHE_CRSQ_SIZE`; `CacheRspMcast` sizes each bank's input channel
  `1 << log2ceil(2 + crsq_size)`, as the RTL's `CRSQ_QUEUE_SIZE`. A non-zero value
  without read merging asserts, because that response path (the bank crossbar)
  has no per-bank queue to size.

## How verified

Build copies: `vortex-exp/src_pr_simx-dcache-fwd-hold` (branch code, tree of
afe8c2b52 + b069acbc8 minus the yaml/CHANGELOG lines),
`src_pr_simx-dcache-fwd-hold_base` (upstream/master) and
`src_pr_simx-dcache-fwd-hold_knob` (upstream + the fork's two knob commits,
for off/on). bwstream was copied into the build copies only, never into the
branch. Driver script `vortex-exp/pr/scripts/fwd-hold_batch.sbatch`, lists in
`pr/scripts/fh_lists/`, table `pr/scripts/fh_collect.py`. Results:
`vortex-exp/results/pr_fh_<case>_{base,h0,h1,br,rtl}`. Verified at XLEN 64 only.

| what | driver | result |
|---|---|---|
| SimX builds and runs, branch | simx | run: 48 parity cases pass with instruction counts equal to upstream |
| defaults identical, knob off vs upstream | simx | run: 48/48 parity cases cycle-identical (`_h0` vs `_base`) |
| hold on vs off, 48 parity cases | simx | run: 6 move by more than 0.1% (table), 42 by at most 0.1% |
| rtlsim reference, 13 parity cases + 12 sweep shapes | rtlsim | run (table) |
| `parity-ldst-wide`, `-lanes`, `parity-ldst-matrix`, `parity-sgemm2-lmem-wide` | simx/rtlsim | not run: FLEN 32 only, and only an RV64 toolchain is installed |
| wgmma parity cases, the vm parity cases other than vm-vecadd | | not run |
| XLEN 32 (what the parity gate runs) | | not run |
| new case `parity-sgemv-lsu-lanes` through `pytest ci` | | not run (`ci/testcase.py lint` OK, 964 cases) |
| CRSQ_SIZE=2: upstream SimX ignores it | simx | run: same cycles as at 0 (bwstream 32w 13,383; vecadd 32w 51,280; packld 28,714; sgemm-mc64 128,836) |
| CRSQ_SIZE=2 rtlsim | rtlsim | run: 13,143; 51,938; 28,216; 128,626 |
| CRSQ_SIZE=2 on the branch | simx | not run (job timed out, wind-down) |
| L2 alone (knob copy, dcache hold on, L2 hold off/on) | simx | run: changes only sgemv on 4 cores (174,825 -> 173,134; rtlsim 173,946, +0.5% -> -0.5%) and on 8 cores with 2 L2 banks (176,241 -> 176,881; rtlsim not run, timed out) |
| perf baselines | | unaffected: `ci/baselines/perf` holds rtlsim cycles |

Bug-fix check (brief §4): with the default tolerance, `parity-sgemv-lsu-lanes`
fails on upstream/master (+6.0%) and passes on the branch (+1.0%) at XLEN 64.

## Change class

`python3 ci/change_impact.py explain --base upstream/master --head HEAD`:
**R2** (paths `simx-model`; evidence `simx-rtl-lockstep`: a SimX-only change,
answered by the parity table above). No R3 detector fires; the knob version
would be R3 (`config-knobs`).

Still required by the policy and not done: a review of every hunk with the
`simx-review` skill by a reviewer who did not write it (not done, wind-down);
parity at XLEN 32; maintainer approval.

## Alternative: a default-off knob

The fork's version (cbc696a98 + 462002754) keeps `VX_CFG_DCACHE_FWD_HOLD` /
`VX_CFG_L2_FWD_HOLD` = 0. Knob off is cycle-identical to upstream (measured). It
keeps a known divergence from the RTL as the default, adds an R3 `config-knobs`
obligation (profiles, CI catalog, docs), and would need the new parity case to
set the knob. I do not recommend it.

## Dependencies

Intended base (coordinator's decision, not carried out at the wind-down): upstream/master -> `pr/tests-bwstream` -> `pr/simx-dcache-order` -> this branch, with a `bwstream` model_parity case registered here. The branch stays on upstream/master (ccf811e11) as committed; the restack, the bwstream parity case and a re-run on the final stack are not done.

None. The bwstream rows need `pr/tests-bwstream` to reproduce. The parity case
uses `sgemv`, which is upstream. Trial merges (`git merge-tree`) with every
other `pr/*` branch are clean, including `pr/simx-dcache-order` (its hunks in
`cache.cpp` are around lines 456-640, 1252-1380 and 1936-1984; this branch
touches 940-1010, 1495-1525, 2940 and 3235). `CHANGELOG.md` will conflict
textually with any sibling that also appends to Unreleased/Fixed.

## Open questions for the maintainer

- Five shapes move away from rtlsim by up to 2 points (packld and sgemv at
  16x16, l2link, printf-ss2, bwstream 16x16). Is that acceptable for a
  structural fix, or do you want those explained first?
- The CRSQ depth is modelled only on the read-merge response path. Should the
  crossbar path (L2, caches without read merge) also take a depth, or is the
  assertion enough?

## Notes for the sender

- Branch B (`simx-l2-fwd-hold`) was not created: with the hold unconditional in
  the shared bank model, the L2 already gets it here. See `simx-l2-fwd-hold.md`.
- Account note: the first batch of jobs (`pr_fh_par_*`, `pr_fh_sw*`, `pr_fh_l2_*`,
  `pr_fh_r1..r6`) ran on `gts-hkim358-paid`, before the queue rule; later
  ones ran on `gts-hkim358`.
- Not explained, out of scope (fork measurement): `bwstream_dxa` on 4 cores,
  SimX 19,874 vs rtlsim 18,321.
