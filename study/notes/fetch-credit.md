# Fetch ahead and the instruction buffer: hand-over note (2026-11-23)

State: draft committed as WIP, **not verified on rtlsim**. Worktree
`vortex-exp/wt_fcred`, branch `feature/fetch-credit`, commit `871067f3f`
(parent `dd94ad142`, the pushed work branch). Nothing pushed, nothing merged.

## 1. The finding (coordinator's lab-server runs, h200s, one core)

`bwstream -u4 -b32`, perf class 1: at depth 4 (`VX_CFG_FETCH_INFLIGHT=4`) the
RTL is 38-39% slower than at depth 1 (n=512: 140,758 against 102,205; n=2048:
548,386 against 395,381), with fetch / ibuffer stall 128% / 171%. SimX shows
the loss too weakly (n=512: 126,128; n=2048: 419,474).

## 2. Mechanism (from reading the code at dd94ad142; line numbers of that revision)

1. The scheduler keeps a per-warp count `ibuf_size_r`
   (`hw/rtl/core/VX_warp_sched.sv:360-373`): picks, minus buffer pops, minus
   dropped fetches. It covers everything between the pick and the buffer's
   exit. A warp at `IBUF_SIZE` is full and not picked
   (`preferred_warps = ready_warps & ~ibuf_full`, :467).
2. The exception is the all-full path (:470-476; `VX_scheduler.sv:261-265`):
   with an L1, once every buffer of the core is full, any ready warp may
   fetch, one instruction past its buffer. That instruction cannot enter the
   buffer. Decode has no stage of its own (`req_buf` SIZE 0,
   `VX_decode.sv:1226`) and `decode_if.ready = ibuf_ready_in[decode_wis]`
   (`VX_ibuffer.sv:38`), so it stands at the frontend slice's icache response
   output and every response behind it, of any warp of that slice, waits.
   The blocking is per frontend slice.
3. Depth 1 limits itself: full is `size == IBUF_SIZE` (:377-378), so the
   over-picked warp at size 5 no longer reads as full, the core is no longer
   all-full, and over-picking stops at about one instruction in the core.
4. With fetch ahead I had made it `size >= IBUF_SIZE` (commit 4b0ce2689), so
   the core stays all-full and every fetch-idle ready warp is over-picked in
   turn. A warp whose buffer then frees a slot still reads as full (5 - 1),
   gets no normal pick, and its next instruction is the queued one, served
   only at the head of the slice's response queue. The slice's warps are
   chained in pick order behind the slowest load. It needs every buffer of
   the core full, which a load stream produces and the existing parity cases
   never did.
5. SimX has the same two rules (`sim/simx/core.cpp:697-707`, :1294-1301) and
   holds a response at the icache output for a full buffer (:820-838), but
   counts its buffer differently (IBUF_SIZE + 1 entries with a staging head,
   a separate queued count). Why its loss is smaller was not traced.

## 3. The rule (approved by the coordinator as option 1: no new knob)

With `VX_CFG_FETCH_INFLIGHT > 1` a warp is picked only while its count is
below `IBUF_SIZE`; the all-full path does not exist. The count is the existing
one, so a fetched instruction always has a slot in its own warp's buffer and
never waits at decode, hence never blocks another warp.

- Squashed fetches: a dropped response returns its credit one cycle after the
  drop (the existing `ibuf_drop`, through the registered `decode_sched_if`).
  An instruction that is accepted and squashes its stream keeps its slot: it
  is in the buffer.
- The in-flight window (`FETCH_INFLIGHT`), the lone-warp condition and the
  drain rule are unchanged.
- Depth 1 keeps today's behaviour, all-full path included, and must be
  cycle-identical. Its residual against rtlsim (+2.0 to +2.5% on bwstream at
  h200s) and SimX's different buffer accounting are open items, not part of
  this change.

### What the WIP commit changes

- `hw/rtl/core/VX_warp_sched.sv`: `full_warps = FETCH_AHEAD ? preferred_warps
  : ready_warps`; `ibuf_full_n` back to exact equality; `IBUF_CW` back to
  `$clog2(IBUF_SIZE + 1)`; assertion that the count never passes `IBUF_SIZE`
  with fetch ahead.
- `hw/rtl/core/VX_ibuffer.sv`: assertion that with fetch ahead no instruction
  waits at decode (`decode_if.valid && ~decode_if.ready`).
- `sim/simx/core.cpp`: `all_full` only at `FETCH_INFLIGHT == 1`; full flag
  exact equality; the same two assertions.
- `VX_config.toml`: the knob's comment states the rule.

Not yet done in the commit: CHANGELOG entry, the new parity case, the h200s
profile comment (which quotes pre-change figures).

## 4. SimX measurements so far (ICE, h200s, one core, perf class 1)

`bwstream -n512 -u4`, `-b` = warps (32 at 64 warps); cycles and ibuffer stall.
"today" = dd94ad142, "rule" = the draft.

| Warps | depth 1 today | depth 4 today | depth 4 rule |
|---|---|---|---|
| 8  | 95,502; 247% | 94,322; 281% | not run |
| 16 | 98,538; 183% | 102,040; 269% | 96,437; 0% |
| 32 | 99,348; 22%  | 124,701; 246% | 97,846; 0% |
| 64 | 104,177; 3%  | 117,159; 66%  | 104,279; 0% |

My 64-warp depth-4 value (117,159) is not the coordinator's 126,128; not
looked into.

h200s rows, depth 4, today / rule:

| Row | today | rule |
|---|---|---|
| fetch_squash | 10,018 | 10,018 |
| warp_rate | 113,382 | 113,382 |
| tmem_roof -n512 -i8 | 62,075 | 62,075 |
| gemm_block -m16 -n1024 -k4096 -s1 | 572,929 | 572,668 |
| gemm_tmem_decode -m16 -n8192 -k4096 | 1,854,288 | 1,854,288 |
| bwstream -n2048 -u4 -b32 | 452,761 | 406,539 |
| sgemm -n64, default configuration (no profile) | 792,837 | 792,837 |

gemm_block and gemm_tmem_decode differ from the coordinator's references
(576,342 and 1,320,528) in both columns, so the difference is in how those
references were built or run, not in the rule. The gemm_tmem_decode run has 8
of 64 warps active, so the all-full path cannot trigger there.

Small shapes at the default memory (16x8, 32x8, 32x4 warps x threads,
`bwstream -n256 -u4 -b<warps>`, ISSUE_WIDTH 4): 0% ibuffer stall at both
depths, identical cycles today and with the rule. The buffers only fill when
memory is slow, so the new parity case needs slow memory (first point with
`-DVX_CFG_PLATFORM_MEMORY_LATENCY=300` at 16x4, depth 1: 32,614 cycles, 173%
ibuffer stall on both trees; the rest of that exploration is running).

No SimX assertion fired in any run of the draft.

## 5. ICE jobs still running (all mine; results under `ice:~/scratch/vortex-ice/results/`)

Source copies on ICE: `src/src_fc_base` = dd94ad142, `src/src_fc_new` = the
draft (same content as 871067f3f). Their Phoenix originals are in this
session's node-local scratch and will disappear; re-push from the worktree
under a new name if a tree is needed again.

| Job id | Name | What | Result |
|---|---|---|---|
| 7248929 | fc0_rt_w16_d1 | rtlsim, today, h200s 16 warps, depth 1, bwstream -n512 -u4 -b16 | `fc0_rt_w16_d1/run.log` |
| 7249027 | fc0_rt_w16_d4 | same, depth 4 | `fc0_rt_w16_d4/` |
| 7248937 | fc0_rt_w32_d1 | rtlsim, today, 32 warps, depth 1, -b32 | `fc0_rt_w32_d1/` |
| 7248959 | fc0_rt_w32_d4 | same, depth 4 | `fc0_rt_w32_d4/` |
| 7249029 | fc1_rt_w16_d4 | rtlsim, draft, 16 warps, depth 4 (RTL assertions on) | `fc1_rt_w16_d4/` |
| 7249031 | fc1_rt_w32_d4 | rtlsim, draft, 32 warps, depth 4 | `fc1_rt_w32_d4/` |
| 7249196 | fcb_base_simx | bundle, today, SimX | `fcb/{shape,cases,sweep}_base_simx.txt` |
| 7249199 | fcb_new_simx | bundle, draft, SimX | `fcb/..._new_simx.txt` |
| 7249198 | fcb_base_rtlsim | bundle, today, rtlsim | `fcb/..._base_rtlsim.txt` |
| 7249200 | fcb_new_rtlsim | bundle, draft, rtlsim | `fcb/..._new_rtlsim.txt` |
| 7249368 | fcx_base | SimX, today, bwstream at 16x4 / 32x4 / 64x4 with memory latency 300, depth 1 and 4 | `fcb/shape_x_base_simx.txt` |
| 7249369 | fcx_new | same, draft | `fcb/shape_x_new_simx.txt` |

The bundle (`vortex-exp/slurm/fc/fcb.sbatch`, case list `slurm/fc/cases.txt`)
runs three parts: `shape` (small bwstream shapes, default memory), `cases`
(the 15 existing model_parity cases with fetch ahead and the 5 fetch_squash
regression cases), `sweep` (warp_rate 13 kinds x 1/2/4/8 warps at 8 warps x
8 threads, ISSUE_WIDTH 4, FETCH_INFLIGHT 4, FFLAGS_ACCUM 1: the 52 rows).
Failed runs leave `fcb/fail_<tag>_<label>.log`. h200s rtlsim builds need
about 150 GB; the first attempt at 32 GB was killed. Finished single points
are fetched with `vortex-exp/ice/ice_fetch.sh <name>`; the bundle files with
rsync.

Finished and on ICE: `fc0_sx_w{8,16,32,64}_d{1,4}`, `fc1_sx_w{16,32,64}_d4`,
`fch0_*` / `fch1_*` (h200s rows today / rule), `fcs0_sg` / `fcs1_sg`.
`fch0_sg` / `fch1_sg` are sgemm under h200s by mistake: ignore.

The coordinator is building the draft at h200s size on the lab server
(bwstream n=512 and n=2048, smprobe); the full-size RTL proof comes from there.

## 6. Remaining plan

- (a, unfinished) Reproduction on rtlsim at a small shape: the six `fc*_rt_*`
  jobs. If depth 4 is not slower than depth 1 on rtlsim at 16 or 32 warps,
  the small shape does not show the mechanism; the lab-server runs then carry
  the proof.
- (c) After the change, on both models: the same shapes; the two assertions
  on in every run; a new parity case that fills the buffers with load-blocked
  work (fails today, passes after), with `xlen: [64]` and a statement that it
  was measured at XLEN 64. It needs slow memory at a small shape: pick it
  from `shape_x_*` and confirm on rtlsim. Default configuration identical on
  both models (SimX done with sgemm; rtlsim not run). The 52-row sweep, the
  fetch_squash cases and the 15 parity cases on both models, before and
  after (the four bundle jobs).
- (d) h200s SimX rows before / after: done above. Blackbox lines for the
  coordinator's lab-server rtlsim, all with `PROFILE=h200s` exported before
  configure, `VORTEX_VM_MEGAPAGE_HEAP=1`,
  `CONFIGS="-DVX_DBG_STALL_TIMEOUT=20000000"`:
  `./ci/blackbox.sh --driver=rtlsim --profile=h200s --cores=1 --perf=1 --app=<app> --args="<args>"`
  for bwstream `-n512 -u4 -b32`, bwstream `-n2048 -u4 -b32`, warp_rate (no
  args), fetch_squash (no args), gemm_block `-m16 -n1024 -k4096 -s1`,
  gemm_tmem_decode `-m16 -n8192 -k4096`.
- (e) CHANGELOG entry, the h200s profile comment if its figures move, then
  one independent review (R3: RTL and SimX timing) before the coordinator
  merges.

Open items to record, not to chase here: the depth-1 residual on bwstream at
h200s (+2.0 to +2.5%); SimX's buffer accounting (IBUF_SIZE + 1 with a staging
head); why SimX's depth-4 loss today is smaller than the RTL's.

## Codex progress | 2026-11-23 01:37 EST | Step 1

Reviewed WIP 871067f3f. A pick reserves credit through fetch and decode; an accepted response transfers the reservation to the buffer, a stale or overtaking response returns it through the registered drop path, and a buffer pop returns it even when the warp is stalled. A wstall response keeps its slot. Branch redirects wait for stale fetches to drain, and the VM boundary guard blocks only speculative picks. Reset clears credit and warp activation does not discard a live reservation. SimX reserves through its fetch/decode path and returns credit when its head crosses into staging, with the staging entry counted separately from the queue. Normal rtlsim builds enable runtime assertions. The WIP SimX count assertion compiled out under NDEBUG, so it now uses the always-on assertion macro. The RTL credit assertion checks each pick before the counter update, which also catches an over-pick if the counter would wrap. Clarified the configuration text. `git diff --check` passed. Branch still at 871067f3f with local edits. No new jobs. Next: collect ICE/rover evidence and verify the full-size RTL result.

## Codex progress | 2026-11-23 01:39 EST | Step 2 in progress

Fetched ICE bundle snapshots and the completed rover SimX logs. Rover source `vfcred` matches WIP 871067f3f on the four changed files and h200s profile. Rover n=512 RTL session 750686 and restarted n=2048 and smprobe RTL sessions 937094 and 937431 are using the existing `mfcred_h200s` model. The first n=2048 and smprobe launches exited 143 before running. ICE jobs 7249368–7249369 completed the slow-memory SimX exploration. The six reduced-warp h200s RTL jobs and four bundle jobs remain active.

| h200s one core | Depth 1 RTL / SimX reference | Depth 4 RTL / SimX reference | Depth 4 RTL / SimX WIP |
|---|---:|---:|---:|
| bwstream -n512 -u4 -b32 | 102,205 / 104,270 | 140,758 / 126,128 | running / 104,279 |
| bwstream -n2048 -u4 -b32 | 395,381 / 405,204 | 548,386 / 419,474 | running / 406,539 |
| smprobe -k0 -n2 -i20 -d5000 | — | — | running / 94,455 |

The coordinator's depth-4 references are from the earlier study source. ICE baseline dd94ad142 is distinct (117,159 at n=512 and 452,761 at n=2048), so the two sources' counts are tracked separately. ICE's prior GEMM Blackbox arguments match the request, but its metadata omits `-DMTILES=1 -DPROFILE_ENABLE` and `-DKSTEPS=2 -DPIPE=6 -DXFER=8192`. The slow-memory 32-warp × 4-thread shape is a candidate (41,362 → 32,520 SimX cycles, 211% → 0% ibuffer stalls, 31,520 instructions). It needs both RTL measurements with CI flags. No commit yet. Next: run the small candidate and collect the remaining full-size and bundle results.

## Codex progress | 2026-11-23 01:44 EST | Step 3 candidate runs

Submitted 32×4 and 16×4 slow-memory candidates on both models using the same XLEN 64 no-profile Blackbox invocation CI uses. The comparison trees on ICE are `fc_credit_base` (a470ffa5c) and `fc_credit_fixed` (that base plus the reviewed fetch-credit diff). The first submission 7249575–7249578 failed before configure because the node had no per-user temporary directory. The corrected jobs are 7249594 (base RTL), 7249595 (base SimX), 7249596 (fixed RTL), and 7249597 (fixed SimX). Each also runs default `sgemm -n64`. Results will be in `results/fcb/fccg_*.txt`. No YAML case has been selected yet.

The h200s follow-up jobs 7249598 and 7249599 use the same source pair, export PROFILE=h200s before configure, set VORTEX_VM_MEGAPAGE_HEAP=1, and supply the requested GEMM macros. The earlier reference discrepancy is confirmed in the old logs: 812,352 instructions in gemm_block and 271,171 in gemm_tmem_decode with the defines missing. The 1,320,528-cycle decode reference retires 211,722 instructions with the requested defines. Next: select a candidate after validating the before/after parity gap, then fetch the remaining runs.

## Codex progress | 2026-11-23 01:48 EST | Independent review

A separate reviewer found no blocking credit-accounting issue and confirmed that release builds keep both new assertions enabled. True VM-fault recovery remains an existing limitation. The speculative page guard prevents the invalid requests at issue here, and normal CTA reuse does not discard credits. Submitted ICE Beacon job 7249659 for the top_vortex catalog design with XLEN 64, fetch depth 4 and the a470ffa5c comparison. It uses the pinned focal tools in a per-job directory since the corresponding RHEL 9 bundles are unavailable. Next: collect its result alongside the simulation jobs.

## Codex progress | 2026-11-23 01:52 EST | Step 3 case selected

The 32×4 candidate on the a470ffa5c source pair executes 31,520 instructions on each model. At XLEN 64, RTL / SimX cycles moved from 40,957 / 41,362 (0.989% gap) to 32,427 / 32,520 (0.287%). Added `core:parity-bwstream-fetch-credit:rtlsim` with `xlen: [64]` and a 0.5% bound between those measured gaps. The exact CI runner jobs are 7249679 (base) and 7249680 (fixed). CI metadata lint passed and the selector resolves to XLEN 64 and both models. The 16×4 candidate has a narrower available tolerance margin (1.333% before and 0.627% after), so it was not selected.

Beacon job 7249659 could not start CIRCT on ICE because its glibc lacks GLIBC_2.35. Retrying with the same pinned tool bundles on Ubuntu rover, session 1046678. No simulation was stopped. Next: read the CI runner verdicts and finish the larger measurement table.

## Codex progress | 2026-11-23 01:57 EST | Steps 2–4 in progress

The default depth-one sgemm -n64 passed on the a470ffa5c source pair (ICE 7249594–7249597). RTL stayed at 792,858 cycles, SimX at 792,837, and all runs retired 334,880 instructions. The first six paired h200s SimX rows passed. Correctly configured gemm_block reproduced the 576,342-cycle reference and took 571,376 cycles with the fix (814,144 instructions on each). The correctly configured gemm_tmem_decode is still running.

Rover WIP smprobe session 937431 finished at 50,477 RTL cycles and 33,445 retired instructions. The WIP SimX twin takes 94,455 cycles with 66,993 instructions, so it is not a qualified parity comparison. On the distinct a470ffa5c source pair, h200s SimX smprobe is cycle- and instruction-identical across the fetch-credit fix (78,268 cycles and 57,841 instructions). The queued n=2048 point 937094 exited before simulation. It was relaunched as 1079264 with the runner’s parallel-app option, and n=512 and n=2048 now use the same already-built model concurrently. The shared Ramulator statistics filename is unneeded for cycle evidence; each run has its own PERF and exit log.

The rover Beacon retry initially called the generated script, which cannot discover its source checkout. Session 1078017 reuses the pinned tools, source trees and build directories with the source-script entry point. ICE jobs 7249727–7249729 stage the pinned RV32 tools and run the five dual-XLEN fetch_squash cases on both drivers on the a470ffa5c source pair. The existing XLEN 64 parity and sweep bundles continue. Next: collect CI verdicts, paired suites and the full-size RTL proof.

## Codex progress | 2026-11-23 01:59 EST | Steps 2 and 3 results

The rover full-size n=512 RTL point (750686, fetched as `results/fcr_r_n512`) passed in 1,703 seconds. It retired 120,832 instructions and completed in 102,481 cycles, +0.27% against the 102,205-cycle depth-one reference and −27.19% against the 140,758-cycle depth-four reference. SimX WIP is +1.75% at 104,279 cycles with the same retired instructions. RTL fetch/ibuf stalls went from the depth-four reference’s 128%/171% to 3%/0%. The n=2048 full-size point remains active. The earlier 602af5c84 RTL smprobe run has the same 50,477 cycles and 33,445 instructions as WIP. The smprobe kernel's neighbours loop until a flag set by the probe, so timing affects the amount of executed work across models.

Step 3 is verified in the real test runner. ICE 7249679 failed specifically on the 0.99% cycle gap and 7249680 passed at 0.29%. Both runners reproduced 31,520 instructions and the standalone measurements. The runner's integer-percent log display rounds the configured 0.5% tolerance to 0%; the comparison uses YAML's exact 0.005. The result logs are `results/fcb/fc_ci_{base,fixed}.log`. No commits yet. Next: compare both completed h200s SimX result files and collect the paired suites.

## Codex progress | 2026-11-23 01:59 EST | Step 4 h200s SimX

ICE jobs 7249598 and 7249599 passed all seven cases on a470ffa5c and the same source with the reviewed fix. Configured with PROFILE=h200s before configure, VORTEX_VM_MEGAPAGE_HEAP=1, one core and perf class 7.

| Test | SimX before | SimX after | Instructions | Cycle change |
|---|---:|---:|---:|---:|
| fetch_squash | 10,018 | 10,018 | 978 | +0.00% |
| warp_rate | 113,382 | 113,382 | 8,511 | +0.00% |
| tmem_roof | 62,075 | 62,075 | 8,925 | +0.00% |
| gemm_block | 576,342 | 571,376 | 814,144 | -0.86% |
| gemm_tmem_decode | 1,320,528 | 1,320,528 | 211,722 | +0.00% |
| bwstream | 452,761 | 406,539 | 464,896 | -10.21% |
| smprobe | 78,268 | 78,268 | 57,841 | +0.00% |

The base values reproduce all six supplied references. Earlier gemm_block and gemm_tmem_decode jobs used the same arguments while leaving XCONF empty. With -DMTILES=1 -DPROFILE_ENABLE, gemm_block retires 814,144 instructions versus 812,352 in the earlier job. With -DKSTEPS=2 -DPIPE=6 -DXFER=8192, gemm_tmem_decode retires 211,722 versus 271,171 in the earlier job. Logs are under results/fcb/fch2_*. Next: collect the paired regression bundles and remaining RTL results.
