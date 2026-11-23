# Load → barrier parity

## 2026-11-23 11:46 EST — Setup

Started `fix/bar-load` at `d353c1c33` (current `feature/dxa-borrowing` HEAD) in `vortex-exp/wt_bar_load`. The supplied measurements use `602af5c84`; a fresh six-row SimX baseline at the branch head is running separately from the small reproducer. Using rover and preserving the original worktree and existing jobs.

## 2026-11-23 11:52 EST — Reproducer and trace

The smprobe neighbours execute synchronous `vx_barrier` twice per iteration. The first follows eight global loads and their shared stores. The second follows peer shared loads. A scratch version of `bar_vis` runs two four-warp CTAs for eight fixed rounds, each issuing a global load and a barrier, with a second mode for shared stores and reads. Configuration is XLEN 64, 8 warps × 32 threads, 2 issue slots, 8 LSU lanes, drain scope 1 and memory latency 200. SimX completed both modes (8,909 and 9,067 cycles). Traces record the pending-instruction, queue-sent and store-landed gates at each barrier. The rover RTL slots are occupied. ICE job 7254807 was submitted as a second route.

Static comparison exposed two hypotheses to test. SimX computes last LSU response but uses the last dispatched packet for instruction retirement. SimX also observes live drain state after some queue mutations within a cycle. Neither comparison alone attributes the original six-row gap.

## 2026-11-23 11:58 EST — Split-load retirement isolated

Added a scratch case that warms the last three 8-lane packets and sends the first packet to cold lines. In SimX, warp 5's load at `0x180000140` had dispatch EOP on pid 3 at cycle 2296, with pid 0's final response still pending. The barrier at `0x180000150` passed at 2333. The true final response committed at 2544. Switching only instruction retirement to response EOP made the barrier pass at 2545. Both runs retired 2,000 instructions, with 6,354 → 7,185 cycles. RTL uses last-response EOP in `VX_lsu_block.sv`; the small RTL trace is queued at ICE (job 7254875). Two earlier ICE submissions failed at a glibc debug-build naming collision. The ICE scratch sources use a renamed internal assert macro.

This mechanism did not affect the original current-head h200s `-k0 -n2` case. A/B stock and response EOP each gave 61,399 cycles, probe p50 1,269, 54,965 instructions. None of 1,012 accepted sync barriers had an outstanding early-retired load. Logs are in `results/bg_smp_rsp_audit/`; the six current-head baseline logs are in `results/bg_smp_pre_k?n?`. The original gap still needs attribution.

## 2026-11-23 12:06 EST — RTL trace and candidate fix

ICE job 7254875 ran the same three scratch cases. The all-cold and shared-memory variants matched retired work and stayed within 1% of RTL (SimX/RTL cycles 9,763/9,821 and 9,703/9,628). The split-load variant isolates the mismatch. For warp 5 and load UUID 21474836541, at RTL trace time 4,699 the warm lanes committed with EOP=0. The cold lanes committed with EOP=1 at time 5,195. The waiting barrier accepted at time 5,201, after `warp_pending_alm_empty` asserted. RTL ran 7,052 cycles. At identical retired work, the scratch SimX result was 9.90% under RTL before the fix and 1.89% over RTL after it.

Updated `Core::retire` to account for the architectural instruction on response EOP for tracked LSU responses. Added an XLEN 64 `parity-bar-vis-split-load` case in the existing `bar_vis` application. The source catalog lints. The release CI case is being validated on ICE, and the six current-head SimX reruns are in progress on rover.

A separate h200s audit observed 23 of 1,012 sync barrier arrivals while a store sent that cycle had yet to appear in the local-memory channel's delivered queue. A write-only scratch blocker eliminated those arrivals and changed `-k0 -n2` from 61,399 cycles to 64,734, with 1,269 to 1,228 probe p50 and 54,965 to 56,213 retired instructions. This is a reachable SimX queue-timing discrepancy. It does not supply a matched RTL comparison or explain the supplied historical gap. The stock and change runs are in `results/bg_smp_drain_audit/typed-{pre,post}.log`. A fixed-iteration h200s comparison using both original 602af5c84 runtimes is running on rover.

## 2026-11-23 12:12 EST — h200s reruns and fixed work

All six requested SimX conditions passed on current head, before and after response-EOP retirement. Every `SMPROBE` and `PERF` line matched in the six pairs. Logs are under `results/bg_smp_{pre,post}_k?n?`. The small stock `smprobe -k2 -n2 -i8 -c2 -s1 -d0 -N2` on current head also matched the same retired work in both models (1,761 instructions, 11,716 SimX and 11,674 RTL cycles). Those two CTAs run serially at the small configuration's default local-memory capacity. The split-load trace excerpts are under `results/bg_d353_trace/warp5-{simx,rtlsim}.txt`.

The original h200s `602af5c84` matched-runtime comparison kept 15 neighbour CTAs at exactly two rounds each after removing the timing-dependent exit branch. With `-k2 -n2 -i8 -c16 -N2 -d0`, both models retired 22,585 instructions. SimX finished in 25,395 cycles versus RTL's 33,148 (23.39% fewer), while probe p50 was 459 versus RTL's 287. The difference survives equal work and skipping the probe's cycle-based delay loop. Provenance, scratch app diff and logs are in `results/bg_fixed_602.qHPxev/`. A phase-timed variant will check where those equal-work neighbours spend their cycles. The split-load patch is also being checked against the original `-k0 -n2` case in the 602af5c84 SimX build to determine whether its impact differs from this branch head.

## 2026-11-23 12:16 EST — Historical A/B

On historical `602af5c84`, a clean rover h200s `-k0 -n2 -i20 -d5000` SimX run gave exactly the same output with the response-EOP condition as the matching stock rover run. Both passed with 77,636 cycles, probe p50 1,475, 449,720 loads at mean latency 248.12, and 57,841 retired instructions. The stock replay differs from the supplied 78,268-cycle point. Model flags and app arguments match for this pair. See `results/spd4_s_k0/run.log` and `results/bg_smp_v602_post_k0n2/run.log`. This rules out the reproduced response-EOP defect as the cause of that historical comparison. The RTL phase run is still active.

## 2026-11-23 12:19 EST — CI regression verified

ICE job 7255196 ran the new release `model_parity` node with the ordinary pytest runner. The stock SimX leg ran 5,379 cycles versus RTL's 6,361, failing the default 5% parity gate at 15.44%. With response-EOP retirement, SimX ran 6,343, passing at 0.28%. All three legs retired 1,296 instructions and the app's own data checks passed. The controlled A/B uses a scratch-only assert-macro rename on ICE for GCC/glibc compatibility. Logs and the reproducible job script are in `results/bg_d353_ci/`. Independent static review found no additional defects in the patch. Catalog lint and both repository boundary checks passed.

The historical fixed-work SimX test was also rerun with the response-EOP fix. The application binaries were unchanged and all reported metrics matched the unpatched 25,395-cycle run. See `results/bg_fixed_602_eop/`. The original gap persists in both the historical dynamic case and its fixed-work variant.

## 2026-11-23 12:22 EST — Phase timing bound

The two-round historical phase probe recorded 30 samples in SimX. It took 31,682 cycles versus 25,395 without phase probes, so instrumentation materially changes contention. RTL reached its nine-minute limit before producing results. The phase measurements bracket global-load-dependent stores, BAR 1, shared reads and BAR 2. They include issue scheduling, dependencies and rendezvous. The scratch diff, disassembly, logs and timeout are in `results/bg_phase_602.1kiKer/`. A single-round retry keeps the original 16 CTAs and both barriers while shortening the run.

## 2026-11-23 12:28 EST — Historical phase result

The single-round phase probe passed in both models with 18,300 retired instructions and 15 validated samples per model. Total cycles were 25,537 RTL and 21,174 SimX. The new kernel is byte-identical to the two-round instrumented run. The shorter point only adjusts the scratch host's round guard and the invocation.

| Median cycles across neighbouring CTAs | RTL | SimX |
|---|---:|---:|
| Before shared stores to the first barrier | 6,212 | 3,705 |
| First barrier bracket | 832 | 802 |
| After the first barrier through shared reads | 4,836 | 2,387 |
| Second barrier bracket | 146 | 194 |

The measured gap accumulated mainly in the memory-side intervals. Those intervals include the outstanding global-load dependency tail, shared memory, stack and stop-flag traffic, and scheduling under contention. They establish where cycles accumulated in this experiment. The first differing memory-path event is still unknown. Raw samples and paired summary are in `results/bg_phase1_602.0ElFv3/`.

## 2026-11-23 12:28 EST — Final result

SimX had used dispatch EOP to remove a split load from a warp's pending count. The last issued packet can hit while an earlier packet is still outstanding. In the small reproducer, that first incorrect count changed at SimX cycle 2,296. The targeted barrier saw the pending count as empty at its first check at 2,321 and accepted at 2,333, 211 cycles ahead of the final load commit at 2,544. RTL's `VX_lsu_block.sv` produced response EOP only when the last outstanding request finished. The same warp's barrier waited until the scheduler observed that retirement. `Core::retire` now uses `rsp_last` for tracked memory responses. In the scratch trace the barrier first accepted at 2,545 with the fix.

The new `parity-bar-vis-split-load` case uses XLEN 64, eight 32-thread warps, eight LSU lanes, and drain scope 1. It warms the lines of later load packets and uses cold lines for the first packet. Both simulators check the returned data. The final build also passed the existing `parity-bar-vis-lanes` check at 0.39% against its 1% tolerance.

| XLEN 64 CI case | RTL cycles | SimX cycles | Gap | Gate |
|---|---:|---:|---:|---|
| Before | 6,361 | 5,379 | 15.44% | Fail |
| After | 6,361 | 6,343 | 0.28% | Pass |

Both versions retired 1,296 instructions. The ICE logs are in `results/bg_d353_ci/`.

All six h200s SimX reruns used one core and `-i20 -d5000` on the branch head. Each cell shows before → after. Every run passed.

| Args | Cycles | Probe p50 | Loads | Mean load latency | Retired instructions |
|---|---:|---:|---:|---:|---:|
| `-k1 -n1` | 46,759 → 46,759 | 970 → 970 | 302,240 → 302,240 | 466.71 → 466.71 | 62,740 → 62,740 |
| `-k2 -n1` | 40,787 → 40,787 | 705 → 705 | 303,136 → 303,136 | 469.66 → 469.66 | 62,176 → 62,176 |
| `-k0 -n1` | 47,768 → 47,768 | 1,247 → 1,247 | 343,616 → 343,616 | 454.67 → 454.67 | 69,961 → 69,961 |
| `-k1 -n2` | 65,697 → 65,697 | 1,164 → 1,164 | 452,314 → 452,314 | 242.29 → 242.29 | 58,389 → 58,389 |
| `-k2 -n2` | 45,984 → 45,984 | 598 → 598 | 338,618 → 338,618 | 253.88 → 253.88 | 45,317 → 45,317 |
| `-k0 -n2` | 61,399 → 61,399 | 1,269 → 1,269 | 424,017 → 424,017 | 240.95 → 240.95 | 54,965 → 54,965 |

The original 602af5c84 h200s gap remains open. The retirement fix had zero measured effect on the historical dynamic `-k0 -n2` replay and on the historical two-round fixed-work probe. The one-round instrumentation puts most of the measured gap in the memory-side intervals. A cycle-level comparison of those intervals would be needed to attribute it to a specific stage. The local-store transit audit remains a distinct, unpatched timing candidate.
