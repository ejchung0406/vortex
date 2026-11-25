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

## Codex progress | 2026-11-23 02:03 EST | Step 4 XLEN 64 suites

ICE bundles 7249196 (base SimX), 7249198 (base RTL), 7249199 (WIP SimX), 7249200 (WIP RTL) compare dd94ad142 and 871067f3f. All 15 existing fetch-ahead parity cases meet their thresholds. All five fetch_squash functional cases passed. All 20 cycle counts are identical before and after within each model, and all four runs per row agree on instruction counts.

| Case | RTL before / after | SimX before / after | Instructions | Gap | Tolerance |
|---|---:|---:|---:|---:|---:|
| parity-bar-vis-lanes | 43,747 / 43,747 | 43,857 / 43,857 | 16,328 | 0.251% | 1.0% |
| parity-warp-rate-ahead-int | 88,726 / 88,726 | 88,720 / 88,720 | 133,820 | 0.007% | 5.0% |
| parity-warp-rate-ahead-fma | 116,156 / 116,156 | 116,150 / 116,150 | 67,260 | 0.005% | 5.0% |
| parity-warp-rate-ahead-branch | 95,960 / 95,960 | 95,954 / 95,954 | 67,260 | 0.006% | 5.0% |
| parity-warp-rate-ahead-lds | 153,676 / 153,676 | 153,703 / 153,703 | 33,219 | 0.018% | 5.0% |
| parity-warp-rate-ahead-shared | 116,579 / 116,579 | 116,575 / 116,575 | 67,952 | 0.003% | 5.0% |
| parity-warp-rate-branch-noreg-ahead | 117,324 / 117,324 | 117,318 / 117,318 | 133,820 | 0.005% | 5.0% |
| parity-warp-rate-fetch-nobuf-ahead | 91,278 / 91,278 | 91,320 / 91,320 | 67,260 | 0.046% | 5.0% |
| parity-warp-rate-flush-nobuf-ahead | 150,108 / 150,108 | 150,105 / 150,105 | 16,824 | 0.002% | 5.0% |
| parity-warp-rate-alu-lanes-gapless-slice | 13,357 / 13,357 | 13,344 / 13,344 | 4,341 | 0.097% | 0.5% |
| parity-warp-rate-alu-lanes | 15,244 / 15,244 | 15,241 / 15,241 | 17,340 | 0.020% | 0.2% |
| parity-warp-rate-lsu-lanes-store | 22,451 / 22,451 | 22,445 / 22,445 | 4,341 | 0.027% | 1.0% |
| parity-warp-rate-lsu-lanes-load | 22,506 / 22,506 | 22,500 / 22,500 | 4,353 | 0.027% | 1.0% |
| parity-warp-rate-lsu-lanes-store-gapless | 18,547 / 18,547 | 18,541 / 18,541 | 4,341 | 0.032% | 0.5% |
| parity-warp-rate-lsu-lanes-load-gapless | 18,591 / 18,591 | 18,585 / 18,585 | 4,353 | 0.032% | 0.5% |
| fetch-squash | 6,453 / 6,453 | 6,440 / 6,440 | 964 | 0.201% | functional |
| fetch-squash-ahead | 5,098 / 5,098 | 5,095 / 5,095 | 1,928 | 0.059% | functional |
| fetch-squash-ahead-shared | 7,069 / 7,069 | 7,079 / 7,079 | 3,856 | 0.141% | functional |
| fetch-squash-ahead-lone | 5,098 / 5,098 | 5,095 / 5,095 | 1,928 | 0.059% | functional |
| fetch-squash-ahead-vm | 5,367 / 5,367 | 5,352 / 5,352 | 489 | 0.279% | functional |

Raw logs are under results/fcb/cases_*.txt. Planner lint passed for 1,080 expanded cases and its three pytest tests passed. Next: collect the sweep and dual-XLEN functional confirmation.

## Codex progress | 2026-11-23 02:05 EST | Step 4 warp_rate sweep

The four ICE XLEN 64 bundles finished the 52-row sweep. Every row is cycle-identical before and after within each model. All four runs per row agree on instruction counts and output hashes. The maximum cross-model gap is 0.142% at k1_w4 (int_ind4). 0 rows exceed 5%, with the same gap on both revisions.

| Kind and active warps | Kernel | RTL cycles (both) | SimX cycles (both) | Instructions | Gap |
|---|---|---:|---:|---:|---:|
| k0_w1 | int_dep | 10,725 | 10,722 | 1,224 | 0.028% |
| k0_w2 | int_dep | 10,686 | 10,683 | 2,440 | 0.028% |
| k0_w4 | int_dep | 10,700 | 10,694 | 4,872 | 0.056% |
| k0_w8 | int_dep | 12,419 | 12,415 | 9,736 | 0.032% |
| k1_w1 | int_ind4 | 4,298 | 4,295 | 1,220 | 0.070% |
| k1_w2 | int_ind4 | 4,197 | 4,194 | 2,432 | 0.071% |
| k1_w4 | int_ind4 | 4,211 | 4,205 | 4,856 | 0.142% |
| k1_w8 | int_ind4 | 12,376 | 12,372 | 9,704 | 0.032% |
| k2_w1 | int_ind8 | 4,358 | 4,355 | 1,221 | 0.069% |
| k2_w2 | int_ind8 | 4,224 | 4,221 | 2,434 | 0.071% |
| k2_w4 | int_ind8 | 4,236 | 4,230 | 4,860 | 0.142% |
| k2_w8 | int_ind8 | 12,360 | 12,356 | 9,712 | 0.032% |
| k3_w1 | fma_dep | 21,862 | 21,859 | 1,219 | 0.014% |
| k3_w2 | fma_dep | 21,832 | 21,829 | 2,430 | 0.014% |
| k3_w4 | fma_dep | 21,846 | 21,840 | 4,852 | 0.027% |
| k3_w8 | fma_dep | 22,502 | 22,498 | 9,696 | 0.018% |
| k4_w1 | fma_ind4 | 8,644 | 8,641 | 1,221 | 0.035% |
| k4_w2 | fma_ind4 | 8,604 | 8,601 | 2,434 | 0.035% |
| k4_w4 | fma_ind4 | 8,618 | 8,612 | 4,860 | 0.070% |
| k4_w8 | fma_ind4 | 16,381 | 16,377 | 9,712 | 0.024% |
| k5_w1 | fma_ind8 | 8,653 | 8,650 | 1,221 | 0.035% |
| k5_w2 | fma_ind8 | 8,622 | 8,619 | 2,434 | 0.035% |
| k5_w4 | fma_ind8 | 8,636 | 8,630 | 4,860 | 0.069% |
| k5_w8 | fma_ind8 | 16,227 | 16,223 | 9,712 | 0.025% |
| k6_w1 | br_every1 | 20,312 | 20,309 | 1,219 | 0.015% |
| k6_w2 | br_every1 | 20,091 | 20,088 | 2,430 | 0.015% |
| k6_w4 | br_every1 | 20,106 | 20,100 | 4,852 | 0.030% |
| k6_w8 | br_every1 | 20,782 | 20,778 | 9,696 | 0.019% |
| k7_w1 | br_every4 | 7,552 | 7,549 | 1,221 | 0.040% |
| k7_w2 | br_every4 | 7,427 | 7,424 | 2,434 | 0.040% |
| k7_w4 | br_every4 | 7,439 | 7,433 | 4,860 | 0.081% |
| k7_w8 | br_every4 | 8,724 | 8,720 | 9,712 | 0.046% |
| k8_w1 | br_every16 | 5,372 | 5,369 | 1,221 | 0.056% |
| k8_w2 | br_every16 | 5,244 | 5,241 | 2,434 | 0.057% |
| k8_w4 | br_every16 | 5,256 | 5,250 | 4,860 | 0.114% |
| k8_w8 | br_every16 | 11,130 | 11,126 | 9,712 | 0.036% |
| k9_w1 | lds_own | 10,127 | 10,124 | 2,259 | 0.030% |
| k9_w2 | lds_own | 10,115 | 10,112 | 4,510 | 0.030% |
| k9_w4 | lds_own | 10,128 | 10,122 | 9,012 | 0.059% |
| k9_w8 | lds_own | 22,918 | 22,914 | 18,016 | 0.017% |
| k10_w1 | lds_chase | 23,064 | 23,061 | 1,222 | 0.013% |
| k10_w2 | lds_chase | 23,027 | 23,024 | 2,436 | 0.013% |
| k10_w4 | lds_chase | 23,054 | 23,048 | 4,864 | 0.026% |
| k10_w8 | lds_chase | 23,684 | 23,680 | 9,720 | 0.017% |
| k11_w1 | sts_own | 4,436 | 4,433 | 1,221 | 0.068% |
| k11_w2 | sts_own | 4,303 | 4,300 | 2,434 | 0.070% |
| k11_w4 | sts_own | 5,547 | 5,541 | 4,860 | 0.108% |
| k11_w8 | sts_own | 12,422 | 12,418 | 9,712 | 0.032% |
| k12_w1 | lds_nouse | 4,541 | 4,538 | 1,233 | 0.066% |
| k12_w2 | lds_nouse | 4,498 | 4,495 | 2,458 | 0.067% |
| k12_w4 | lds_nouse | 4,537 | 4,531 | 4,908 | 0.132% |
| k12_w8 | lds_nouse | 12,522 | 12,518 | 9,808 | 0.032% |

Raw logs are under results/fcb/sweep_*.txt. The rebase target advanced to fd151ee95 with study files only since a470ffa5c. The tested hardware, simulator and CI inputs are unchanged by those two base commits. Next: collect reduced-warp RTL, full-size n=2048, RV32 fetch_squash and Beacon.

## Codex progress | 2026-11-23 02:10 EST | Step 5 commits and rebase

Checkpointed the reviewed implementation, CI case and changelog while the detached follow-up jobs run. Rebased feature/fetch-credit onto current feature/dxa-borrowing fd151ee95 (only study material changed from the a470ffa5c test source).

| Scope | Rebased commit |
|---|---|
| RTL, SimX and fetch-depth documentation | 4e95ed5c4b2e193e43c66c14b5d1d01fc05e5d9c |
| XLEN 64 model_parity case | 4829b9222da2fc71c2764820b2ecd69251eb04ea |
| CHANGELOG | 180e49dcbe4185a708821adadb242b2f9dc12e9d |

Every commit ends with the requested Codex co-author trailer. The implementation files match the ICE fixed source checksums and the CI case hunk matches the exact pytest patch. Post-rebase diff check, 1,091-case lint and three planner tests passed. The worktree is clean. No merge or push was performed. A stale empty worktree index lock left at 01:32 was moved aside before committing, after confirming no Git process held it.

ICE jobs 7249774 and 7249775 are rerunning the XLEN 64 cases and sweep on the reviewed version with the strengthened assertions. Rover Beacon comparison 1109898 queues behind the shared build lock and will compare the measured non-default fetch-depth-4 reference against the fix. Next: collect the remaining RTL and final assertion evidence.

## Codex progress | 2026-11-23 02:10 EST | Step 6 open items

Open for separate work: the depth-one h200s bwstream SimX residual (+2.02% at n=512, +2.48% at n=2048) and SimX’s IBUF_SIZE + 1 representation with an uncharged staging head. The smaller pre-fix depth-four penalty on SimX is also untraced. These are documented here without expanding the fix. Historical smprobe cross-model timing remains a separate variable-work comparison, and no vendor place-and-route was run.

## Codex progress | 2026-11-23 02:22 EST | RV32 fetch_squash and Beacon

The real CI runner passed all ten RV32 fetch_squash driver/case combinations on both the baseline (ICE 7249728) and the reviewed fix (7249729). The lone-warp-per-slice case adds 19 cycles in RTL and SimX, keeps the three-cycle cross-model gap and retires identical instructions. The other four rows are cycle-identical before and after within each driver.

| Case | RTL before / after | SimX before / after | Instructions |
|---|---:|---:|---:|
| fetch-squash | 5,990 / 5,990 | 5,977 / 5,977 | 948 |
| fetch-squash-ahead | 4,694 / 4,694 | 4,691 / 4,691 | 1,896 |
| fetch-squash-ahead-shared | 6,331 / 6,331 | 6,340 / 6,340 | 3,792 |
| fetch-squash-ahead-lone | 4,675 / 4,694 | 4,672 / 4,691 | 1,896 |
| fetch-squash-ahead-vm | 4,964 / 4,964 | 4,947 / 4,947 | 480 |

The two-core top_vortex Beacon comparison on rover 1170648 passed at XLEN 64 with fetch depth 4. Maximum logic depth stayed at 26, the mean of the 50 deepest paths at 23.3, FF at 299,164, BRAM at 100 and DSP at 66. LUT count fell from 376,684 to 376,676. No register got deeper or harness tie changed. Tool versions are CIRCT 892a65a01, foldsim d8350bc0f and pyslang 11.0.0. The earlier comparison launch needed the pinned Python dependency. The catalog gate also reported NO-BASELINE for the non-default configuration. Its direct reference comparison passed and no baseline was edited. Logs and JSON are under results/fc_credit_beacon. Next: collect the final RTL suite and long bwstream jobs.

## Codex progress | 2026-11-23 11:43 EST | Final verification and report

All requested runs were collected. The three-commit series is headed by `180e49dcbe4185a708821adadb242b2f9dc12e9d`, rebased on `fd151ee95ce404baa4a45ace92c9f4d9b131cdc3`. Its implementation is `4e95ed5c4`, CI case `4829b9222`, and changelog `180e49dcb`. During the detached runs, the local target branch independently gained merge `5f855c2ae`, with `180e49dcb` as its second parent and an identical tree across all six changed files. The separate feature branch and registered worktree were removed. Codex made no merge or push and did not alter the target branch.

### Full-size h200s RTL and SimX

One core, 64 warps × 32 threads, `bwstream -u4 -b32`. Cycle pairs are RTL / SimX. The baseline columns use the coordinator’s earlier study. The fix uses the rover WIP tree with the same behavior as the final patch, whose later refinements strengthen simulation assertions and clarify comments.

| n | Instructions | Depth 1 reference | Depth 4 before | Depth 4 after | Fixed RTL vs depth 1 | Fixed SimX vs RTL |
|---:|---:|---:|---:|---:|---:|---:|
| 512 | 120,832 | 102,205 / 104,270 | 140,758 / 126,128 | 102,481 / 104,279 | +0.27% | +1.75% |
| 2,048 | 464,896 | 395,381 / 405,204 | 548,386 / 419,474 | 395,176 / 406,539 | -0.05% | +2.88% |

The n=512 and n=2048 RTL measurements reduced depth-four cycles by 27.19% and 27.94%, with 0% ibuffer stalls in both. Rover sessions 750686 and 1079264 finished successfully in 1,703 s and 6,059 s. WIP smprobe also passed at 50,477 RTL cycles, matching the historical RTL reference. Its WIP SimX run took 94,455 cycles with more retired instructions because the neighbours run until a timing-dependent stop flag. On the a470ffa5c source pair, SimX smprobe was identical before and after at 78,268 cycles.

### Reduced-warp h200s and CI

ICE `bwstream -n512 -u4` with one core and block size equal to the hardware warp count. Cycle pairs are RTL / SimX. The source pair is dd94ad142 / 871067f3f.

| Hardware warps | Instructions | Depth 1 before | Depth 4 before | Depth 4 after |
|---:|---:|---:|---:|---:|
| 16 | 30,208 | 97,511 / 98,538 | 111,244 / 102,040 | 97,180 / 96,437 |
| 32 | 60,416 | 104,952 / 99,348 | 122,812 / 124,701 | 98,521 / 97,846 |

All six reduced-warp RTL points passed (7248929, 7249027, 7248937, 7248959, 7249029, 7249031). The control, test and sweep runs on the reviewed source (7249774, 7249775) reproduced every WIP result on both models byte for byte in the 20-case and 52-row summaries, with strengthened assertions enabled. The default depth-one sgemm control stayed at 792,858 RTL and 792,837 SimX cycles. The XLEN 64 parity table and the seven h200s SimX measurements, including the two corrected GEMM define sets, are recorded in the Step 4 sections above.

New XLEN 64 parity case: `parity-bwstream-fetch-credit`, 32 warps × 4 threads, fetch depth 4 and platform memory latency 300. The two models retired 31,520 instructions on both revisions. RTL / SimX cycles were 40,957 / 41,362 before (0.989%, fail) and 32,427 / 32,520 after (0.287%, pass), against the 0.5% bound. Exact pytest logs are `results/fcb/fc_ci_{base,fixed}.log` (7249679, 7249680). Beacon’s depth-four comparison passed with 26 levels before and after.

The reservation covers a fetch from selection through decode and buffering. A buffer pop or dropped response returns it. The reviewed WIP refinements check RTL credit at the pick before the counter can wrap, make the SimX count assertion active in release builds, and clarify the fetch-depth knob. The two full-size RTL points ran on the older rover WIP profile. Full-size RTL with the later profile changes and vendor place-and-route were outside the verified set. The depth-one residual and SimX staging-entry accounting remain open as recorded above.
