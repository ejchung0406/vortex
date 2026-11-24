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

## 2026-11-23 17:46 EST — Current-head continuation

Created local branch `fix/smem-phase` from `feature/dxa-borrowing` at `c0cf5eec1`, in `vortex-exp/wt_smem_phase`. Changes after `ccd8d3974` are study notes only. The paired fixed-work phase run is using the existing `mccd8_h200s_c1` and `sccd8_h200s_c1` rover builds (identical resolved vccd8 configs, PERFCLASS=1) and the same one-round phase app. Neither simulator is being rebuilt for that experiment. The separate stand-alone dcache channel mismatch is outside this follow-up.

The supplied current-head smprobe cycle pairs (RTL / SimX) are 39,238 / 45,984 at `-k2 -n2`, 45,597 / 61,399 at `-k0 -n2`, and 37,950 / 40,787 at `-k2 -n1` with `-i20 -d5000`. Phase results will choose a single isolated memory path for the next A/B.

## 2026-11-23 17:55 EST — Current-head fixed work

The same one-round instrumented app ran on the existing rover models with `-k2 -n2 -i8 -c16 -N1 -d0` and `PERFCLASS=1`. Both passed with 18,300 retired instructions and 15 validated CTA samples each. Whole-run cycles were 23,797 / 18,753 (RTL / SimX). Identical app hashes, exact configs and raw logs are in `results/bg_phase1_ccd8.A16DTg/`.

| Interval | RTL median | SimX median | Paired RTL − SimX median | Paired RTL − SimX mean |
|---|---:|---:|---:|---:|
| Before shared stores to BAR1 | 5,721 | 3,168 | +2,704 | +2,661.9 |
| BAR1 bracket | 355 | 640 | −46 | +75.7 |
| After BAR1 to BAR2 | 4,764 | 2,937 | +1,846 | +1,994.3 |
| BAR2 bracket | 86 | 139 | −55 | −131.9 |

The first memory interval accounts for 58% of the mean paired gap. It contains the outstanding global-load dependency tail, stack reloads and shared-store acceptance. The next experiment isolates only that interval: eight global loads feeding eight local stores, no barriers, two four-warp CTAs, cold and repeated input. Loads alone and stores alone are controls. The initial small config retains the h200s LSU lanes, shared banks, row engines and gapless dispatch, with VM/L2/DXA/TCU disabled.

## 2026-11-23 18:10 EST — Isolating the first memory interval

The first barrier-free kernel ran eight global 64-bit loads followed by eight shared 64-bit stores per thread. All pairs retired the same instruction counts.

| Run | RTL cycles | SimX cycles | RTL − SimX relative to SimX |
|---|---:|---:|---:|
| Small, 8 warps, cold load→store | 9,189 | 9,203 | −0.15% |
| Small, 8 warps, repeated load→store | 16,491 | 16,175 | +1.95% |
| Small, 8 warps, load reduction | 16,504 | 16,507 | −0.02% |
| Small, 8 warps, constant-fed stores | 16,702 | 16,702 | 0.00% |
| h200s, 64 warps, cold load→store | 15,207 | 15,013 | +1.29% |
| h200s, 64 warps, repeated load→store | 14,107 | 14,031 | +0.54% |
| h200s, 64 warps, load reduction | 13,686 | 13,550 | +1.00% |
| h200s, 64 warps, constant-fed stores | 11,788 | 11,568 | +1.90% |

The load reduction and constant-fed modes still have compiler-generated stack traffic. The core-cycle gap is below 2% across this experiment. Evidence is in `results/bg_small_ccd8_ice/` and `results/bg_prebar1_ccd8.wErypb/`.

With the original fixed-work kernel, lower occupancy kept the first memory-interval gap on h200s. The 4-CTA run (up to 16 warps) took 17,653 / 12,298 total cycles and 4,836 / 2,783 median pre-BAR1 cycles (paired +2,053). The 8-CTA run took 19,480 / 13,843 and 5,435 / 2,663 (paired +2,749). Retired work matched within each pair. See `results/bg_occ_ccd8.oZAyrs/summary.txt`.

The disassembly shows a distinction the first reduced kernel missed: after the first stamp, the original reloads eight shared destination pointers from the thread stacks and interleaves each reload with its shared store (PCs 0x1800002b8–0x1800002f4). The next barrier-free kernel preserves that dependency chain. The ICE builds also run the exact instrumented kernel at 16 warps with and without VM, and a separate h200s build disables only VM.

## 2026-11-23 18:15 EST — Barrier-free reproduction on h200s

The second reduced kernel preserves eight global loads followed by eight interleaved stack-pointer reloads and dependent shared stores. Its disassembly has zero barrier instructions. Both existing h200s models passed with the same retired instructions and the same global load/store counts within each pair.

| Active warps | RTL / SimX cycles | RTL / SimX window median | Paired RTL − SimX window median | Retired |
|---|---:|---:|---:|---:|
| 16 | 7,738 / 5,451 | 5,149 / 2,371 | +2,778 | 1,814 |
| 64 | 10,533 / 8,800 | 5,066 / 2,761 | +2,162 | 7,118 |

Source is `results/bg_phase1_ccd8.A16DTg/phase_stack_kernel.cpp`. Paired logs and disassembly are in `results/bg_stack_ccd8.v7VYEa/`. Switching the memory-controller implementation to the bank model on the original fixed-work kernel retained the gap: 23,688 / 19,023 total cycles and paired pre-BAR1 median +2,501 cycles for 18,300 retired instructions. Logs are in `results/bg_memctl_ccd8_bank/`. Small compiled traces are in progress with VM off/on; two additional small configurations add the new dcache settings, then a four-bank L2 and the h200s L2 link delay.

## 2026-11-23 18:18 EST — Small VM control

Both small configurations kept 16 active warps, four issue slices, 8 LSU lanes, the shared row engines, and 304 cycles of platform memory delay. The default D-cache geometry was used and L2, DXA and TCU were disabled.

| VM | Kernel | RTL / SimX cycles | RTL / SimX first window median | Retired per model |
|---|---|---:|---:|---:|
| Off | Exact fixed work | 16,189 / 16,108 | 3,391 / 3,675 | 4,512 |
| Off | Barrier-free stack replay | 7,189 / 7,220 | 3,830 / 3,780 | 1,702 |
| On | Exact fixed work | 17,766 / 17,635 | 3,639 / 3,418 | 4,512 |
| On | Barrier-free stack replay | 8,973 / 8,086 | 3,758 / 3,827 | 1,814 |

For the VM-on stack replay, the leader timestamps show that the whole-run delta is already present before the measured stack-load/store window. In warp 4 the address load at PC 0x18000010c takes 740 cycles from first LSU request to last response in RTL and 302 in SimX. The later stack reloads and shared stores then run at comparable intervals in the two models. Raw traces and the per-PC tables are in `results/bg_stack_ccd8_ice/`. The next small configurations use h200s D-cache word size, channels and response queue, then add four L2 banks and 94-cycle links.

## 2026-11-23 18:21 EST — Shared-store sink control

Replaced each dependent shared store with one `xor zero, address, value` instruction while preserving the eight source loads and the eight stack-pointer reloads. The disassembly confirms eight of those instructions and no barriers. On the unchanged h200s models, both retired 1,814 instructions and counted 9,226 load lanes. Whole-run RTL / SimX cycles were 7,713 / 5,402. The first-window medians were 5,123 / 2,358 (paired +2,765), essentially the same gap as the shared-store version. This places the reproducing dependency in the stack reload sequence. See `results/bg_stack_ccd8_xor2/`. The experiment leaves the independent streaming-load dcache issue out of scope.

## 2026-11-23 18:29 EST — L2 replacement evidence

The unchanged h200s 16-warp stack replay sent 3,024 requests to L2 and recorded 1,042 L2 read misses in both models. RTL emitted 474 dirty L2 writebacks and SimX emitted zero. Traffic to memory was 985 reads and 486 writes in RTL, versus 523 reads and 12 writes in SimX. Both models recorded zero coalescer misses and zero shared-memory bank stalls. Counters are in `results/bg_stack_ccd8_counters/`.

The h200s L2 uses 33 ways and tree PLRU. RTL's `VX_cache_repl.sv` selects the tree victim even with other invalid ways, updates the tree only on a committed lookup hit, and folds unused leaves into valid ways. SimX's `cache.cpp` chooses the first invalid way and then uses timestamp-based LRU. The 32-byte D-cache word control retains the stack gap at 7,792 / 5,765 cycles and also retains the original fixed-work gap at 18,064 / 12,294 cycles (4 CTAs). That existing control pair also disables the D-cache response combining queue. Logs are in `results/bg_stack_ccd8_word32/`.

Added an XLEN 64 parity case with a dependent pointer chase through a three-way L2. The baseline CI run and the small traced stack run are queued on ICE. The baseline source is still unmodified in SimX.

## 2026-11-23 18:37 EST — Parity case fails before the fix

The new `parity-chase-l2-plru` case failed in the unmodified SimX build. Both models passed the program and retired 678 instructions. RTL took 79,713 cycles and SimX took 17,389, a 78.19% gap against the unchanged 5% tolerance. The case uses a 3-way L2 with two dependent addresses per set and bypasses D-cache. Its baseline CI log is `results/bg_plru_ccd8_ci/before.log`.

The SimX change models the binary PLRU tree, updates it on successful pipelined hits, and uses the selected way for a new line even if another way is invalid. Replacement choices use the read-first admission ordering: a fill admitted at slot F sees only hits admitted at H where H + 2 < F. Admission slots are tracked separately from MSHR order so stalls cannot compress bubbles in this calculation. The fixed CI case and the patched SimX reruns are in progress.

## 2026-11-23 18:42 EST — First different victim in the small trace

The small, 16-warp barrier-free replay with the h200s D-cache geometry and a default FIFO L2 took 6,143 / 5,951 cycles (RTL / SimX). Substituting a 3-way PLRU L2 gave 8,396 / 8,606 cycles and pre-window medians of 4,448 / 5,117, both with 1,814 retired instructions. The three-way cache is too small to show the same timing direction as h200s: it has frequent writebacks under both policies. This trace still exposes the first replacement decision that disagrees. The fixed-work RTL control at this debug level exceeded its 120-second wall limit; the barrier-free replay completed.

In bank 1, set 0, the initial line at physical address `0xf0000080` occupied way 0. The last RTL hit to that way before the next new tag was at cycle 472. The next fill, for `0x10130000`, used way 2 at RTL cycle 813. Baseline SimX admitted the corresponding lazy fill at cycle 792, when only way 0 was resident, and its first-invalid rule chooses way 1. The RTL trace also shows a later dirty allocation replacing this line at cycle 1814. Paired raw traces, the extracted bank sequence and per-PC LSU timings are in `results/bg_stack_ccd8_ice/vm_cache_l2_plru/`. The h200s comparison continues with the same original workload and the patched SimX model.

## 2026-11-23 19:07 EST — Stage 1 reproduces the RTL replacement timing

Stage 1 changes SimX on local branch `fix/smem-phase`. The RTL reference remains `mccd8_h200s_c1`. SimX's I-cache and D-cache use `CacheCluster`, which instantiates the same `Cache` used directly for L2 and L3. The policy change applies wherever that cache selects PLRU. The future invalid-way preference can be enabled by moving the existing `find_invalid_way` guard to the start of `set_t::select_victim()` in `sim/simx/mem/cache.cpp`.

The three-way L2 test is green. RTL stayed at 79,713 cycles and SimX moved from 17,389 to 80,242. The gap fell from 78.19% to 0.66%, with 678 retired instructions in each run. The logs are `results/bg_plru_ccd8_ci/{before,after}.log`. ICE's source copy uses the existing `__vortex_assert` macro spelling for that environment. The repository source and rover build use `__assert`.

The small 33-way L2 configuration (4 banks, 128 sets per bank, 128 B lines) reproduced the h200s timing direction with 16 active warps and a ten-minute RTL build. The initial pair took 8,200 / 5,872 cycles on RTL / SimX and retired 1,814 instructions in both models. RTL recorded 481 dirty L2 writebacks versus zero in baseline SimX. The stage 1 SimX rerun uses the same configuration. A separate h200s ablation disabling VM alone retained the gap on the original fixed-work replay at 22,436 / 18,511 cycles and 18,300 retired instructions in both models.

The original h200s fixed-work replay has 15 matching CTA samples. With stage 1 the total is 23,797 / 18,753 / 24,124 cycles on RTL / original SimX / stage 1 SimX. Both large paired memory-interval gaps shrank to under 60 cycles. The paired columns below report the median of the per-CTA RTL minus SimX differences.

| Interval | RTL median | Original SimX median | Stage 1 SimX median | Paired gap before | Paired gap after |
|---|---:|---:|---:|---:|---:|
| Pre-BAR1 | 5,721 | 3,168 | 5,732 | +2,704 | −44 |
| BAR1 | 355 | 640 | 395 | −46 | −10 |
| Pre-BAR2 | 4,764 | 2,937 | 4,698 | +1,846 | +56 |
| BAR2 | 86 | 139 | 157 | −55 | −46 |

The requested h200s runs use shared host and kernel binaries within each case. The DXA copy uses the existing one-SM shape `-e4096 -n32 -d2 -c0`. The GEMM shape is `-m16 -n1024 -k4096 -s1` with `MTILES=1` and `PROFILE_ENABLE`. Its existing archived RTL result is from a different revision, so the table uses a fresh run on `mccd8_h200s_c1`. An earlier wall-clock measurement of the same GEMM workload took about 104 minutes. The current RTL run has a four-hour wall limit and builds no RTL.

## 2026-11-23 19:12 EST — Distinct PLRU issues for stage 2

The RTL bank uses the tree-selected way for a new line even with invalid ways available (`VX_cache_bank.sv`, `fill_way_st0`). `VX_cache_repl.sv` writes the tree only on a lookup hit, so successive new-line fills can reuse the same victim until a hit updates its path. The 33-way aliasing has a separate failure. From cleared bits, a hit to way 32 followed by a hit to way 17 makes the encoder return leaf 48, which folds to way 17. The most recent hit is therefore the next victim. With 33 ways, only way 32 has a real leaf in the right half, so the example follows from reachable tree bits. Alias leaf counts alone do not give the selection frequency.

The planned stage 2 policy is lowest-numbered invalid way first, a PLRU tree that skips empty subtrees, and a tree update on new-line installation as well as lookup hits. The stage 1 patch intentionally replicates the three original behaviors for the comparison. Stage 2 must account for a lookup hit and an allocating fill committing together against the current PLRU store's single write port. Continue to submit any future rover builds with `rover/rover_start.sh` so each build acquires only its own slot.

The exact final stage 1 source was rebuilt on rover and reproduced every SimX result. The 16-warp replay now matches RTL's L2 request and read-miss counts (3,024 and 1,042 respectively) and has 476 dirty writebacks, against 474 on RTL and zero in the old SimX. Memory reads and writes were 985/486 on RTL, 523/12 in old SimX, and 985/488 after stage 1. See `results/bg_stack_ccd8_counters/` and `results/bg_plru_ccd8_h200s/verified/stack.c4.p{5,7}.log`.

## 2026-11-23 19:15 EST — h200s stage 1 measurements

Each RTL result is the shared reference for before and after. The gap is `abs(SimX − RTL) / RTL`. Both `smprobe` rows use `-i20 -d5000`. Their time-based neighbor loops retire different amounts of work. In `-k2 -n2`, for example, loads per cycle are 5.44 on RTL, 7.36 before and 5.74 after.

| Workload | RTL cycles | SimX before | SimX stage 1 | Absolute gap before → after |
|---|---:|---:|---:|---:|
| Stack replay, 16 warps | 7,738 | 5,451 | 7,806 | 29.56% → 0.88% |
| Stack replay, 64 warps | 10,533 | 8,800 | 10,414 | 16.45% → 1.13% |
| `smprobe -k2 -n2` | 39,238 | 45,984 | 39,741 | 17.19% → 1.28% |
| `smprobe -k0 -n2` | 45,597 | 61,399 | 42,993 | 34.66% → 5.71% |
| `gemm_block` | Running | 515,695 | 516,746 | Pending |
| `bwstream_dxa` | 52,875 | 53,753 | 53,289 | 1.66% → 0.78% |
| `ldstream` lean | 168,615 | 182,466 | 180,255 | 8.21% → 6.90% |

The latest exact-source logs are in `results/bg_plru_ccd8_h200s/verified/`. The before runs and the two `smprobe` RTL replays are in `results/bg_plru_ccd8_h200s/extras/`. `ldstream` reuses the earlier two RTL and SimX logs for the same `ccd8d3974` model configuration and byte-identical app binaries. The separate D-cache discrepancy remains open.

## 2026-11-23 19:36 EST — Stage 2 changes and small RTL checks

Stage 1 is committed locally as `2f0a0cf31` on `fix/smem-phase`. Stage 2 is on `fix/cache-repl`, based on that commit. The shared cache implementation handles I-cache, D-cache, L2 and L3 in both models. The h200s configuration selects PLRU in L2. Stage 2 chooses the lowest invalid way first, prunes nonexistent PLRU leaves, and touches the chosen way when a new line is installed or a lookup hits. Sector refills keep their resident way and do not touch the tree. The bank admits one operation per cycle. When its S1 touch and S0 victim selection overlap, the older touch is visible to the younger selection, as is a same-set write on the preceding RAM read edge. SimX applies updates in bank operation order.

FIFO still selects its per-set pointer without reading validity, so it can evict a resident line if another way has become invalid. The non-LLC AMO passthrough can clear a way's last valid sector without moving that pointer. Stage 2 leaves FIFO selection and timing alone. The existing PLRU CI entries `cache:sgemm-16` and `cache:sgemm-19` are functional smoke cases. No existing PLRU `model_parity` or `perf_gate` case uses a stored cycle baseline. The XLEN64 three-way parity added in stage 1 has a live relative tolerance.

| Small check | Stage 1 RTL / SimX | Stage 2 RTL / SimX | Stage 2 gap |
|---|---:|---:|---:|
| 3-way L2 pointer chase, 678 instructions per model | 79,713 / 80,242 | 17,371 / 17,389 | 0.10% |
| 33-way L2 `cache_ways`, 6,288 instructions per model | — | 78,312 / 78,650 | 0.43% |

The original pointer-chase pair was 79,713 / 17,389 (78.19%, failed). The replacement unit test passed with 1, 3, 4, 5 and 33 ways, including back-to-back same-set fills, hit-to-fill forwarding, stall handling and the 33-way hit-alias sequence. ICE jobs: unit 7264840, parity 7264841, 33-way 7264842, FIFO controls 7264843. Scripts and fetched results are under `results/bg_cache_repl_stage2/`.

Submitted one h200s RTL build through `rover/rover_start.sh`, model `mcache_repl_h200s_c1`, point `bg_cache_repl_rtl_k2n2`. It waits for a normal rover build slot. The after-SimX model is `scache_repl_h200s_c1`. The FPGA Beacon comparison uses the VM wrapper with a 33-way PLRU L2 on both sides.

## 2026-11-23 19:43 EST — Targeted Beacon and h200s SimX

Virtex UltraScale+ HBM Beacon, using the same `VX_vm_top` with 128 sets and 33 ways in its L2 bank, measured 23 → 23 levels overall and 21 → 18 inside the L2 bank. The tag RAM's write-enable path moved from 19 to 16 levels, the valid RAM's maximum from 16 to 14, and the bank S1 register from 15 to 13. LUTs were 147,759 → 148,002, FFs 141,179 → 141,331 and BRAMs 254 → 254.

The raw endpoint-name comparison treats line-number-generated `BUFFER_EX` instance names as stable. For example, `__buffer_ex186` was the tag data register before and is the fill flag after. After matching the ten tag buffers by RTL signal name, the new L2 policy has no deeper matched endpoint. The low-threshold diff still flags 32 D-cache tag-data endpoints at 3 → 4 levels. The report and the aligned comparison are under `results/bg_cache_repl_stage2/beacon/`.

The seven Stage 2 SimX results and the eight-workload SimX comparison are in `results/bg_cache_repl_stage2/tables.md`. Both fixed-work stack replays again take 5,451 / 8,800 cycles in SimX, as on original CCD8. The two dynamic `smprobe` runs again take 45,984 / 61,399 cycles in SimX. Their RTL changes are pending. No new RTL build has started yet; the single submitted job is waiting for a rover slot. The old RTL GEMM measurement is still running independently.

Stage 2 is committed locally as `43a4b2036` on `fix/cache-repl`. Both worktrees are clean. `results/bg_cache_repl_stage2/refresh.sh` fetches receipts and refreshes the live tables. The small 33-way stack reproduction measured 8,200 RTL / 5,872 original SimX / 8,183 Stage 1 SimX cycles, all with 1,814 retired instructions. Its Stage 2 replay was submitted on ICE (7265263) while rover still waits for a build slot.

## 2026-11-23 19:50 EST — Rover build started

`bg_cache_repl_rtl_k2n2` acquired one build slot at 19:44:53 and entered Verilator. The follow-on replays wait on this model and will use its existing runtime library directly. The Stage 2 SimX seed and the original `smprobe` have byte-identical host and kernel binaries. The aligned Beacon comparison at the 12-level watch threshold reported zero deeper or newly deep endpoints and zero area-threshold failures. The wider GPU logic-gate suite is still unmeasured.

The optional default-width CI comparisons needed an RV32 runtime/compiler installation on ICE. The pinned packages are now installed in a scratch overlay (7265458), and the baseline/candidate FIFO controls and PLRU smoke runs are waiting for ICE slots (7265459–7265462).

## 2026-11-23 20:04 EST — Same-work 33-way stack replay

The isolated 16-warp replay on the original small 4-bank, 33-way L2 shape now takes 5,891 RTL and 5,872 SimX cycles, both with 1,814 retired instructions. Its RTL fell from 8,200 cycles by 2,309 cycles (28.16%), and the new model gap is 0.32%. This uses the same fixed-work kernel and geometry as the first reproduction. See ICE 7265263 and `results/bg_cache_repl_stage2/ice/stack33/`.

The old and new small replay host and kernel hashes match. L2 recorded 3,024 requests and 1,042 read misses in both Stage 2 models. Its dirty-writeback counter fell from 481 to zero on RTL, and Stage 2 SimX also recorded zero.

## 2026-11-23 20:08 EST — Existing CI cache cases

Three existing XLEN32 FIFO L2 model-parity controls passed on the original and Stage 2 sources with exactly the same instruction and cycle counts. Both existing D-cache PLRU smoke cases passed and have no golden cycle data.

| CI case | Driver | Original cycles | Stage 2 cycles |
|---|---|---:|---:|
| `core:parity-vecadd-lazy-l2` | RTL / SimX | 360,572 / 360,592 | 360,572 / 360,592 |
| `core:parity-sgemm-mc64-l2link` | RTL / SimX | 139,266 / 139,653 | 139,266 / 139,653 |
| `core:parity-chase-l2link` | RTL / SimX | 219,776 / 219,772 | 219,776 / 219,772 |
| `cache:sgemm-16`, D-cache PLRU | RTL | 797,373 | 791,708 |
| `cache:sgemm-19`, D-cache PLRU | SimX | 792,016 | 791,705 |

No existing `model_parity` or `perf_gate` case opts into PLRU. The CI cycle-baseline update list is empty. ICE logs 7265459–7265462 are in `results/bg_cache_repl_stage2/ice/`.

## 2026-11-23 20:31 EST — h200s RTL model built

The one rover build took 2,749 seconds (45m49s) and peaked at 73 GiB as sampled by rover. The seed `smprobe -k2 -n2` began at 20:30:42. The follow-on runners use its library directly.

## 2026-11-23 20:34 EST — h200s fixed-work stack replay

Both h200s replays passed and retired the same instruction count before and after. RTL went from 7,738 to 5,304 cycles for 16 warps (1,814 instructions), and from 10,533 to 8,358 cycles for 64 warps (7,118 instructions). Stage 2 SimX took 5,451 and 8,800 cycles, leaving gaps of 2.77% and 5.29%, respectively. The original gaps were 29.56% and 16.45%. See the live table at `results/bg_cache_repl_stage2/tables.md`.

Counter reruns reproduce those same cycles. For 16 warps, RTL L2 dirty writebacks went from 474 to 0 with 3,024 requests and 1,042 read misses on both models. The 64-warp replay also recorded zero dirty writebacks in both models (RTL / SimX requests 9,998 / 9,997 and read misses 4,123 / 4,117).

## 2026-11-23 20:45 EST — h200s dynamic probes and DXA copy

`smprobe -k2 -n2 -i20 -d5000` is 44,499 RTL / 45,984 SimX cycles (3.34% gap). The RTL retired 44,525 instructions, up from 31,317, and its load-throughput counter went from 5.44 to 7.45 loads per cycle (7.36 in Stage 2 SimX). Probe p50 still differs at 355 / 598 cycles. `smprobe -k0 -n2` is 66,230 RTL / 61,399 SimX cycles (7.29% gap); RTL now retires 59,165 instructions, up from 34,725. Their total cycles reflect different work completed by the time-based neighbor loops. The fixed-work DXA copy is 52,528 RTL / 53,753 SimX cycles (2.33% gap), with RTL down 347 cycles and the same 9,394 instructions. The h200s RTL `ldstream` and both GEMM results are still pending.

## 2026-11-23 20:56 EST — Rover connection stalled

Rover SSH stops responding before the server banner. The last fetched h200s matrix has five completed rows and all SimX results. The old and new RTL GEMM and the Stage 2 RTL `ldstream` had started before the connection stalled. Their exit status and the wider GPU Beacon run are unconfirmed until Rover is reachable. The targeted 33-way L2 Beacon comparison and the small parity receipts are already local.
