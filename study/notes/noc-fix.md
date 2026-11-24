# NoC rebase and fixes

## 2026-11-24 — scope

Started `feature/noc2` from `8a56e7483` and will replay the NoC changes from `f55faaa2d..2910aa910`. The original branch remains at `2910aa910`. The merged DXA rate engine and current h200s profile are the integration baseline. The prior review is linked below. [Independent review](noc-review-codex.md)

## 2026-11-24 — replay and focused checks

- Replayed the 25 NoC non-merge commits other than `ef5ca3923` onto `8a56e7483`. The seven merge commits brought in earlier work-branch history. Reapplied the ingress gate around the merged DXA arbiters in `0269b14cc`, merged the two CI additions, and resolved the timed DXA kernel to the current uniformly annotated workload. The original `feature/noc` and its worktree were left in place.
- The merged engine keeps interleaved worker-to-port selection, the local-memory row path, and the SimX RegMerge/RegSplit shared port. Dropping the NoC DXA fork discards that fork's worker arbitration. The NoC ingress design still covers both ports. The merged RTL offered an unbuffered request on the shared extra port, so `f0a9abbeb` registers that offer when grouping or ingress limiting is active and feeds every modeled port through the enabled limiter.
- There are no new NoC profiles in the original branch. The replay retains the current `h200s` and the work branch's dcache channels, dMMU response, LMEM row, and fetch-ahead code.
- `9b5b2898b` adds the KMU to the standalone and external tick completion check. The XLEN64 KMU repro shows a masked first tick with platform channels idle and the KMU still busy. The standalone ISA test is in the catalog.
- `5271f111c` mirrors recursive switch arbitration in SimX and counts every response stage in RTL. The isolated 72×128 constructor previously aborted and now drains 384 distinct responses at three cycles without stalls. The 72×64 control retains its two-cycle path. The 128×128 case also drains its 384 responses.
- `3ea3e0b7a` restores zero-mask flat placement in SimX, starts the same arbitration window in both models when masking activates, and keeps IDs above the DCR mask range eligible. The XLEN64 KMU probes match base dispatch ticks and locations after fresh and repeated launches with the mask zero. The old 304-core build aborts during construction. The new probe constructs and places one CTA on each of IDs 256–303 with the first 256 IDs masked.
- `f08550006` includes the dedicated page walker in L2 bank geometry. XLEN64 generated-header probes with four socket ports produce eight banks with an L2 PTW and four without, independent of grouping and one, two, or four uplink lanes. The old grouped PTW configuration produced four banks.
- `103961a3d` splits grouped producer storage and the link FIFO. With a blocked full link at delays 1, 2, 4, and 5 cycles, the probe shows upstream ready returning after two ticks at delay 1 or three ticks at higher delay. The old pooled model forwarded ready in one tick. Open-link propagation retains the chosen delays.
- `122b6dd76` keeps the normal bwstream kernel source identical to the base and builds timing instrumentation in a separate binary. `8a2d39ff0` pins the introduced catalog cases to XLEN64 and adds DXA overlap, grouped DXA, and VM/PTW cases. `21d1ec832` documents `-R` only for bwstream, which is the only parser that accepts it.
- `5cb4a9bdb` retains NoC switch and gate state across kernel launches. Isolated XLEN64 probes now select the next lane and keep depleted credits across launch reset, while device reset restores their initial state. Previously both returned immediately to initial state. The new catalog case repeats a gated launch three times.
- The catalog lint passed 1,149 cases in 28 categories. The exact-range whitespace check passed. Focused C++ probes validate component behavior. Application and RTL results are recorded below.

## 2026-11-24 — ICE measurements

The baseline h200s suite was submitted as job `7278873`. It stages the exact clean base revision, sets `PROFILE=h200s` before configure and runs with `VORTEX_VM_MEGAPAGE_HEAP=1`. The first candidate for exact-head identity and parity is `5cb4a9bdb`. Its jobs are tracked by the ICE harness.

## 2026-11-24 — first results and independent review

Seven valid h200s requests at `5cb4a9bdb` match `8a56e7483` exactly in retired instructions and cycles. `gemm_tmem_decode -n8192` returns exit 2 on both. The current profile exposes 22 cores, each of which needs a 512-column tile. The baseline at `-n11264` ran successfully with 312,532 instructions and 263,900 cycles. The paired final run uses the same 22-core profile and changes only N.

The independent R3 review found that the gated shared DXA port had one extra simulated output queue and that the ingress-only L2 request path still used pooled link storage. `bba3efcfb` uses the existing credited crossing before the gate and separates the flat request link stages when ingress is enabled. The reviewer checked the patch against RTL. The original flat request and response links still use the old path with ingress disabled. `8d1ee5b8b` adds an XLEN64 ingress-limited `dxa_rowmix` test that validates address-distinct data from the two DXA ports. The separate `dxa_corun` case stresses concurrent LSU reads. Together these cover contention and payload correctness, though only a directed ready/valid test can prove the offered beat stays stable every stall cycle. The updated catalog lint passed 1,151 cases across 28 categories.

## 2026-11-24 — verification of the code candidate

The code candidate is `8d1ee5b8b1a69f3e6da895e4c81631341b046567`. Relative to `5cb4a9bdb`, it changes two SimX translation units and the DXA test catalog. The late source update in the owned ICE candidate slot verified those three paths against their committed hashes. The remaining jobs stamped `5cb4a9bdb` were already configured and only build or run RTL, whose inputs are identical. Final SimX jobs use the updated source stamp. The other ICE slot remains at the exact base.

The directed RTL arbitration test passed on ICE (`7280997`). In the unbuffered mode the DXA payload `0xa5` changed to the new higher-priority dcache payload `0x3c` while downstream ready was low. With the selected registered output, the first beat stayed at `0xa5`, and the queue drained `0xa5` then `0x3c` exactly once each. The test exercises the same two-input priority stream arbiter and OUT_BUF settings as the shared socket arbiter.

The official standalone `run-simx-64im` target passed 65 ISA programs in both the base (`7281058`) and final candidate (`7281095`) at four cores and two cores per socket. It uses the standalone `cycle()` path. An earlier ad hoc ELF probe on the base timed out. The official suite is the end-to-end evidence, with the focused KMU liveness check covering the masked case. The exact final range classifies R3 because it adds KMU DCR encodings and configuration contracts. Catalog lint and the range whitespace check passed.

`fix/smem-phase` shares `ci/testcases/core.yaml` with this range. `fix/cache-repl` also shares `docs/designs/cache_subsystem.md`. After either lands, reconcile those file edits and rerun the grouped fence and VM/PTW pairs. NoC accounts for request delivery at L2 admission, and cache replacement timing can change when the cache takes that request. Recheck L2 bank and queue geometry and retake the off-path h200s comparisons against the new destination revision.

The full-size valid decode point (`-n11264`) is exact on the final candidate: 312,532 instructions and 263,900 cycles on both revisions. The one-core `j_dxap` ingress replay is 7,993 SimX vs. 8,330 RTL cycles (−4.05%). Removing only the ingress cap on the work-branch base gives 7,984 vs. 8,292 (−3.71%). The base already has 308 of the 337 cycles of model separation. The paired final no-cap control reproduces both base totals exactly. Turning on the cap adds 9 cycles in SimX and 38 in RTL. The 29-cycle increment in model separation is 0.35% of the on-cap RTL total.

## 2026-11-24 — focused and catalog evidence

The final focused component suite passed on ICE at XLEN64 (job `7281306`). It covers 72×64, 72×128, and 128×128 switches, stalled responses, registered link backpressure, switch and rate-gate state across both reset types, and 304-core placement including all 48 core IDs above the mask range. The original component job built with libc's debug `__assert` in scope. The test harness now uses the release compile flags while retaining Vortex's own assertion checks.

All available disabled-mode catalog pairs have exact cycle and instruction counts. The off test removes only the added NoC configuration defines and the bwstream mask, statistics, or repeat arguments introduced by this branch, keeping each case's remaining arguments and configuration. Identical resulting commands are deduplicated within each XLEN and driver. The RV64IM standalone cases have no comparable cycle counter in the ISA harness. The base and candidate both use the same performance-counter preload for the two `fence_mp` aliases because those programs omit automatic totals.

## 2026-11-24 — VM parity and Beacon attempt

The XLEN64 four-core grouped VM/PTW case retired 6,817 instructions on each engine. SimX took 127,588 cycles and rtlsim took 127,607, a −0.015% gap. This is an application-level check alongside the bank geometry probe.

The small four-core Beacon runs for the exact work-branch base and final candidate (jobs `7281500` and `7281511`) both stopped before elaboration. ICE's RHEL 9 installer requested the pinned v3.0.2 CIRCT bundle and received HTTP 404. The toolchain was isolated in each job's temporary directory. No logic-depth, LUT/FF, or Fmax numbers were produced. [Baseline installer log](../results/noc2_edvfx7nt_base_beacon_00/install.log) · [Candidate installer log](../results/noc2_edvfx7nt_final_beacon_00/install.log)

## 2026-11-24 — precise DXA ingress regression

The full XLEN64 four-core `P_dxp2i` pair passed with 22,816 retired instructions on each engine, 16,638 SimX cycles and 16,387 RTL cycles (+1.53%). The archived pair was +5.94%. The older workload retired 28,384 instructions on either model. The replay uses the merged DXA kernel's uniform annotation and worker arbitration.

`e1ce1e1452128034df4265b4982414ddd6ec27c9` updates the parity test catalog to carry the exact measured `P_dxp2i` flags in CI. The planner's XLEN64 arguments and effective defines match each measured leg. Removing the ingress flag yields the paired `P_dxp2` control. This commit only edits the catalog. The compiled sources and measured commands match the final catalog runs. Catalog lint and the exact-range whitespace check passed.

## 2026-11-24 — final h200s identity

Both XLEN64 SimX runs used the 22-core `h200s` profile and megapage heap. Each successful row gives the exact common instruction and cycle totals. The requested decode command returned exit 2 on both revisions before launching a kernel. Its smallest full-width input on this profile is `-n11264` with the other arguments held fixed.

| Workload | Retired instructions on both | Base cycles | NoC2 cycles |
| --- | ---: | ---: | ---: |
| `fetch_squash` | 978 | 10,109 | 10,109 |
| `warp_rate` | 8,511 | 113,488 | 113,488 |
| `tmem_roof -n512 -i8` | 8,925 | 62,546 | 62,546 |
| `bwstream -n2048 -u4 -b32` | 10,227,712 | 1,022,478 | 1,022,478 |
| `gemm_block -m16 -n1024 -k4096 -s1` | 814,144 | 135,871 | 135,871 |
| `gemm_tmem_decode -m16 -n8192 -k4096` | Input error | — | — |
| `gemm_tmem_decode -m16 -n11264 -k4096` | 312,532 | 263,900 | 263,900 |
| `war_order` | 361,856 | 24,497 | 24,497 |
| `ldstream -t2048 -b8 -u8 -m16 -a -l` | 5,504,312 | 1,207,895 | 1,207,895 |

`gemm_block` used `-DMTILES=1 -DPROFILE_ENABLE`. Both decode sizes used `-DKSTEPS=2 -DPIPE=6 -DXFER=8192`. [Base manifest](../results/noc2_edvfx7nt_base_h200_00/points.json) · [Candidate manifest](../results/noc2_edvfx7nt_final_h200_00/points.json) · [Candidate measurements](../results/noc2_edvfx7nt_final_h200_00/metrics.jsonl) · [Candidate decode measurement](../results/noc2_edvfx7nt_final_h200_decode_00/metrics.jsonl)

The full four-core no-cap `P_dxp2` control gives 16,752 SimX cycles and 15,598 RTL cycles at both revisions, with 22,816 retired instructions. The 7.40% absolute model gap therefore precedes the NoC replay. On the candidate the cap changes these totals by −114 SimX cycles and +789 RTL cycles, leaving a +1.53% cross-model gap. The base two-port gap is outside the NoC change and remains visible in the results.

## Final revision and finding status

`feature/noc2` is at `e1ce1e1452128034df4265b4982414ddd6ec27c9` in [wt_noc2](../wt_noc2). Its base is `8a56e7483faac78ec7808b411bc92a028b604e2e`. The range is R3. Final catalog lint passed, with 1,151 cases across 28 categories. The worktree is clean and the original `feature/noc` remains at `2910aa91090aec196d8701dc9dda09c446a6516a`.

| # | Finding | Status and before/after evidence |
| ---: | --- | --- |
| 1 | Standalone completion | Fixed in `9b5b2898b`. The first masked fan-out tick can leave the platform idle while the KMU is busy. The old `cycle()` stop test returned in that state. The shared completion test now checks the KMU. Both revisions passed the 65-program standalone RV64IM suite with four cores and two per socket. [Base](../results/noc2_edvfx7nt_base_standalone_isa_00/isa-64im-standalone-kmu-fanout.log) · [Candidate](../results/noc2_edvfx7nt_final_standalone_isa_00/isa-64im-standalone-kmu-fanout.log) |
| 2 | Recursive fan-in | Fixed in `5271f111c`. The 72×128 switch used to abort on the second slice level. The final switch takes two request ticks and three response ticks, holds 38 stalled beats, and drains all 384 replies without loss or duplication. The 128×128 case takes three ticks in both directions. The XLEN64 catalog has an 18-socket test that selects 128 lanes by default. [Component results](../results/noc2_edvfx7nt_final_components_release_00/noc-focused-components.log) |
| 3 | Zero-mask launch order | Fixed in `3ea3e0b7a`. Before the fix, four CTAs left on ticks 2–5 for cores 0, 2, 1, 3. The candidate and base both use ticks 1–4 and cores 0, 1, 2, 3. Back-to-back one-CTA launches return to core 0. With the low 256 cores disabled, the final 304-core probe reaches all 48 higher core IDs. |
| 4 | Shared DXA offers under stall | Fixed in `f0a9abbeb` and `bba3efcfb` around the merged DXA engine. The directed RTL test shows the old offered byte change from `0xa5` to `0x3c` before ready returns. The buffered form holds `0xa5` and delivers both bytes in order. The XLEN64 catalog adds a two-port row pattern with address-distinct data and a load/copy co-run. Both engines passed the row-pattern comparison. [Directed RTL log](../results/noc2_edvfx7nt_head_rtl_stall_00/shared-dxa-rtl-stall.log) · [RTL row-pattern log](../results/noc2_edvfx7nt_final_ci_on_07/dxa_rowmix-ingress.log) |
| 5 | L2 bank geometry with VM | Fixed in `f08550006`. Four sockets with L2 page walking used eight banks flat and four grouped. The generated XLEN64 configuration now gives eight in both modes across 1, 2, and 4 uplinks. The VM-off control gives four. The grouped VM/PTW workload is within 19 cycles across engines. |
| 6 | Registered backpressure | Fixed in `103961a3d` and `bba3efcfb`. The old pooled link exposed the first freed slot after one tick in the stalled test. The separated stages expose it after two ticks at delay 1 or three ticks at delays 2, 4, and 5. The test checks the original open-link propagation delay at each setting. |
| 7 | DXA parity coverage at the right XLEN | Fixed in `8a2d39ff0`, `8d1ee5b8b`, and `e1ce1e145`. The old implicit XLEN ran 32-bit instead of the measured 64-bit workload. The new case selects XLEN64 and uses the exact four-core `P_dxp2i` configuration, which now comes within 1.53%. Grouped DXA and co-running loads have separate XLEN64 pairs. |
| 8 | Arbitration state on relaunch | Fixed in `5cb4a9bdb`. The old switch reset to its first grant across launches and the gate restored its initial credits. The final probe preserves the next grant and takes six ticks to grant after a depleted launch. Full device reset restores both initial states. The XLEN64 gated bwstream case repeats a kernel three times. |
| 9 | Documented DXA argument | Fixed in `21d1ec832`. The old shared changelog advertised `-R` even though the DXA getopt table rejects it. The final entry scopes `-R` to bwstream, whose three-launch case exercises the flag. |
| 10 | Hardware timing and area | Open. The two ICE Beacon jobs could configure their exact revisions, then the pinned tool download stopped both before elaboration. Functional timing evidence here is in simulated cycles. Gate depth, logic area, and physical Fmax still need measurements. |

### Knobs off — touched catalog

All 51 XLEN64 catalog case/driver pairs match the work-branch head exactly in retired instructions and cycles. Each cycle cell is the common value for the two revisions on that driver. An em dash marks a driver outside that case's catalog. The eight successful h200s pairs above bring the combined count to 59 exact pairs. The original decode size is recorded separately as a matched input rejection.

| XLEN64 case | SimX cycles, both revisions | rtlsim cycles, both revisions |
| --- | ---: | ---: |
| `amo-noc` | 1,402,034 | 1,524,641 |
| `amo-noc-gate-req` | 1,402,034 | 1,524,641 |
| `dxa_rowmix-ingress` | 146,974 | 146,790 |
| `fence-noc` | 7,087 | 7,085 |
| `fence-noc-one-port` | 7,146 | 7,158 |
| `fence-noc-stress` | — | 33,116 |
| `fence_mp-noc-1` | 150,157 | 149,858 |
| `fence_mp-noc-2` | 150,157 | 149,858 |
| `flush_coherence-noc` | 286,518 | 286,534 |
| `flush_coherence-noc-stress` | — | 2,235,537 |
| `model_parity-bwstream-dxa-group-gate` | 16,022 | 15,893 |
| `model_parity-bwstream-dxa-ports` | 16,752 | 15,598 |
| `model_parity-dxa-corun-ingress` | 21,731 | 21,422 |
| `noc-recursive-fanin` | 6,537 | — |
| `parity-bwstream-coremask` | 74,445 | 74,519 |
| `parity-bwstream-coremask-relaunch` | 19,224 | 19,221 |
| `parity-bwstream-coremask-uneven` | 74,445 | 74,519 |
| `parity-bwstream-coremask-uneven-clusters` | 19,855 | 20,115 |
| `parity-bwstream-coremask-uneven012` | 74,445 | 74,519 |
| `parity-bwstream-ingress` | 13,786 | 13,658 |
| `parity-bwstream-noc-gate` | 17,483 | 16,609 |
| `parity-bwstream-noc-gate-req` | 17,483 | 16,609 |
| `parity-bwstream-noc-relaunch` | 8,715 | 8,669 |
| `parity-bwstream-noc-slices` | 16,628 | 16,379 |
| `parity-chase-noc` | 219,700 | 219,704 |
| `parity-chase-noc-ptw` | 122,459 | 122,478 |
| `war_order-noc` | 1,744,705 | 1,745,829 |

### Knobs on — touched catalog

All 51 case/driver executions passed their workload checks. All 15 configured parity pairs had exact retired instruction counts and finished within the existing 5% cycle tolerance. The replay uses uniform `--perf=7` for the counters. The full four-core ingress case also carries that flag in its final catalog definition. The signed percentage uses `(SimX − RTL) / RTL`.

| XLEN64 parity case | Retired instructions on both | SimX cycles | rtlsim cycles | Gap |
| --- | ---: | ---: | ---: | ---: |
| `parity-chase-noc` | 6,810 | 219,700 | 219,704 | -0.002% |
| `parity-chase-noc-ptw` | 6,817 | 127,588 | 127,607 | -0.015% |
| `parity-bwstream-noc-gate` | 15,760 | 18,291 | 18,275 | +0.088% |
| `parity-bwstream-noc-gate-req` | 15,760 | 18,231 | 18,224 | +0.038% |
| `parity-bwstream-noc-relaunch` | 8,055 | 23,815 | 23,759 | +0.236% |
| `parity-bwstream-noc-slices` | 31,520 | 16,590 | 16,295 | +1.810% |
| `parity-bwstream-ingress` | 3,940 | 13,861 | 13,732 | +0.939% |
| `parity-bwstream-coremask` | 41,744 | 73,937 | 73,941 | -0.005% |
| `parity-bwstream-coremask-uneven` | 62,616 | 74,307 | 74,423 | -0.156% |
| `parity-bwstream-coremask-uneven012` | 62,616 | 74,307 | 74,425 | -0.159% |
| `parity-bwstream-coremask-relaunch` | 41,340 | 54,637 | 54,679 | -0.077% |
| `parity-bwstream-coremask-uneven-clusters` | 22,048 | 19,886 | 20,101 | -1.070% |
| `model_parity-bwstream-dxa-ports` | 22,816 | 16,638 | 16,387 | +1.532% |
| `model_parity-dxa-corun-ingress` | 6,936 | 21,950 | 21,516 | +2.017% |
| `model_parity-bwstream-dxa-group-gate` | 22,816 | 16,917 | 17,005 | -0.517% |

### Knobs off — fin5 and DXA controls

Each model matches its own work-branch baseline with the NoC controls off. The first three rows retain the author's four-core wide configuration. The last control is the one-core two-port DXA workload with its 112 B/cycle ingress cap removed.

| XLEN64 case | Instructions on both models and revisions | SimX cycles, both revisions | rtlsim cycles, both revisions |
| --- | ---: | ---: | ---: |
| `P_off` | 31,520 | 17,834 | 17,537 |
| `P_dxoff` | 22,816 | 16,115 | 15,873 |
| `P_dxp2` | 22,816 | 16,752 | 15,598 |
| `j_dxap` with ingress disabled | 2,852 | 7,984 | 8,292 |

### Knobs on — fin5 replay

The archived totals are 18,281 SimX and 17,256 RTL cycles for `P_dxp2i` (+5.94%), and 17,562 SimX and 17,694 RTL cycles for `P_dxgrp` (−0.75%). The prose figures of +4.0% and −2.6% have no matching rows in that archive. Its source staging used a symbolic revision over a copy of `src_all`. The replay records the source of each point. The archived DXA runs retired 28,384 instructions. Their rebased equivalents retire 22,816 with the merged uniformly annotated kernel. [Archived table](nocb/fin5_res.txt) · [Archived launcher](nocb/fin5.sh)

All replayed RTL binaries use the identical RTL, runtime, kernels, and build inputs of the final branch. Four ingress SimX legs were replayed following `bba3efcfb`; the remaining SimX legs use the earlier stamp and compile the same effective datapaths because they disable ingress. The last commit changes the CI catalog only.

| XLEN64 case | Retired instructions | SimX cycles | rtlsim cycles | Gap |
| --- | ---: | ---: | ---: | ---: |
| `P_dxgrp` | 22,816 | 17,508 | 16,755 | +4.494% |
| `P_dxoff` | 22,816 | 16,115 | 15,873 | +1.525% |
| `P_dxp2` | 22,816 | 16,752 | 15,598 | +7.398% |
| `P_dxp2i` | 22,816 | 16,638 | 16,387 | +1.532% |
| `P_gate` | 31,520 | 18,923 | 18,752 | +0.912% |
| `P_gateq` | 31,520 | 18,836 | 18,749 | +0.464% |
| `P_grp` | 31,520 | 17,779 | 17,458 | +1.839% |
| `P_ing` | 31,520 | 17,447 | 17,327 | +0.693% |
| `P_off` | 31,520 | 17,834 | 17,537 | +1.694% |
| `j_chase` | 6,810 | 219,700 | 219,704 | -0.002% |
| `j_dxap` | 2,852 | 7,993 | 8,330 | -4.046% |
| `j_fstress` (RTL only) | 9,776 | — | 24,611 | — |
| `j_gate` | 15,760 | 18,291 | 18,275 | +0.088% |
| `j_gateq` | 15,760 | 18,231 | 18,224 | +0.038% |
| `j_ing` | 3,940 | 13,861 | 13,732 | +0.939% |
| `j_mask` | 62,616 | 74,307 | 74,425 | -0.159% |
| `j_one` | 2,444 | 7,173 | 7,183 | -0.139% |
| `j_slices` | 31,520 | 16,590 | 16,295 | +1.810% |

All 17 paired workloads retired equal instruction counts. The flush stress is RTL-only in the original run. The 3% investigation threshold identified three replay rows.

- **`P_dxp2` (+7.40%)** has all NoC knobs off. Both models exactly reproduce their work-branch totals. On that baseline, changing from one to two DXA ports adds 637 SimX cycles and removes 275 RTL cycles. This isolates the existing gap to the wide two-port configuration.
- **`P_dxgrp` (+4.49%)** shares the wide two-port control. Grouping and the response cap add 756 SimX cycles and 1,157 RTL cycles, reducing the model separation from 1,154 to 753 cycles. The grouped catalog case with fewer wide-core options is within 0.52%.
- **`j_dxap` (−4.05%)** reproduces a −3.71% gap in the no-cap control on both revisions. The cap increases the model separation by 29 cycles, around 0.35% of the measured RTL total.

[Wide replay logs 1](../results/noc2_edvfx7nt_head_fin5_02/metrics.jsonl) · [Wide replay logs 2](../results/noc2_edvfx7nt_head_fin5_03/metrics.jsonl) · [Wide replay logs 3](../results/noc2_edvfx7nt_head_fin5_04/metrics.jsonl) · [Final ingress SimX legs](../results/noc2_edvfx7nt_final_fin5_ingress_00/metrics.jsonl) · [Base two-port RTL control](../results/noc2_edvfx7nt_base_fin5_off_05/metrics.jsonl)

### Before merge

- Restore access to the pinned CIRCT and Foldsim packages and compare Beacon results against the destination revision. The logic-depth and area comparisons need the flat build and the grouped/gated shapes, including the rate gate, multi-level NoC switch, and KMU selector. Confirm any affected timing paths with synthesis and record physical Fmax and area.
- Run the XRT/AFU integration checks for the final configuration. This report contains SimX, rtlsim, and directed RTL results. The unchanged base-model timing gaps are recorded above with their controls.
- If `fix/smem-phase` or `fix/cache-repl` lands first, reconcile the shared CI and cache-subsystem files. Recheck L2 admission and fence ordering, repeat the grouped VM/PTW pair, and retake the disabled h200s comparisons against the new destination revision.

All ICE jobs for this task reached terminal states and their logs were fetched. Both owned ICE source copies were removed after collection. The candidate worktree and the fetched evidence are retained locally.

## NoC3 rebase onto cache replacement — 2026-11-24

`feature/noc3` is at `ecaa43a001fcddbc8a47197aafd8cd114dd0c555` in [wt_noc3](../wt_noc3). The base is `00186b8b3039560c078bdd3f3798544a83158876`, the merge that brought in stages 1 and 2 of `fix/cache-repl`. The rebase carries all 38 NoC2 commits. `git range-diff` matched 37 patches exactly. The sixth patch resolves a shared insertion point in `ci/testcases/core.yaml` by preserving the three-way `parity-chase-l2-plru` case and the NoC cases. `docs/designs/cache_subsystem.md` merged both sets of documentation. Catalog lint passed for 1,152 cases across 28 categories. The candidate worktree is clean and `git diff --check` passed.

The inherited cache changes give each PLRU way a distinct leaf, take the lowest invalid way on fill, and update recency on fill and hit. The independent R3 integration review checked the admission and fence boundaries. RTL still counts acceptance at the L2 valid/ready handshake. SimX counts the cache input pop, then delivers that event to the switch. The switch waits for prior requests to enter L2 before sending the port’s flush token and holds subsequent requests until the token is acknowledged. The PTW still uses a dedicated L2 client outside the groups and contributes to bank sizing based on the flat client count.

### Knobs off — 22-core h200s identity (XLEN64 SimX)

| Workload | Retired instructions on both | Base cycles | Candidate cycles | Logs |
| --- | ---: | ---: | ---: | --- |
| `fetch_squash` | 978 | 10,109 | 10,109 | [Base](../results/noc3_g18vgwmw_base_h200_00/fetch_squash.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/fetch_squash.log) |
| `warp_rate` | 8,511 | 113,488 | 113,488 | [Base](../results/noc3_g18vgwmw_base_h200_00/warp_rate.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/warp_rate.log) |
| `tmem_roof -n512 -i8` | 8,925 | 62,546 | 62,546 | [Base](../results/noc3_g18vgwmw_base_h200_00/tmem_roof.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/tmem_roof.log) |
| `bwstream -n2048 -u4 -b32` | 10,227,712 | 1,017,729 | 1,017,729 | [Base](../results/noc3_g18vgwmw_base_h200_00/bwstream.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/bwstream.log) |
| `gemm_block -m16 -n1024 -k4096 -s1` | 814,144 | 132,029 | 132,029 | [Base](../results/noc3_g18vgwmw_base_h200_00/gemm_block.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/gemm_block.log) |
| `gemm_tmem_decode -m16 -n8192 -k4096` | expected rejection | — | — | [Base](../results/noc3_g18vgwmw_base_h200_00/gemm_tmem_decode.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/gemm_tmem_decode.log) |
| `war_order` | 361,856 | 24,497 | 24,497 | [Base](../results/noc3_g18vgwmw_base_h200_00/war_order.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/war_order.log) |
| `ldstream -t2048 -b8 -u8 -m16 -a -l` | 5,504,312 | 1,206,207 | 1,206,207 | [Base](../results/noc3_g18vgwmw_base_h200_00/ldstream.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_00/ldstream.log) |
| `gemm_tmem_decode -m16 -n11264 -k4096` | 312,532 | 263,417 | 263,417 | [Base](../results/noc3_g18vgwmw_base_h200_decode_00/decode_full.log) · [Candidate](../results/noc3_g18vgwmw_head_h200_decode_00/decode_full.log) |

Both decode sizes use `-DKSTEPS=2 -DPIPE=6 -DXFER=8192`. The smaller size rejects before launch on both revisions and reports zero instructions and cycles. `gemm_block` uses `-DMTILES=1 -DPROFILE_ENABLE`. The h200s profile exercises the new PLRU code with a 33-way L2.

Compared with the previous work-branch base, the cache replacement merge reduces the common cycle totals by 4,749 for bwstream, 3,842 for gemm_block, 1,688 for ldstream, and 483 for the valid decode size. The other four successful inputs retain their earlier totals.

### Touched XLEN64 catalog

Off matches across revisions for 51/51 case/driver legs. On completed 51/51 case/driver legs. The off runs remove the NoC and ingress limits and the bwstream launch mask, placement check and relaunch arguments. DXA port counts and other architectural parameters remain as specified. The `perf` field in each linked metrics file records instructions and cycles in that order.

| Case | Off SimX instr / cycles | Off RTL instr / cycles | On SimX instr / cycles | On RTL instr / cycles | On gap |
| --- | ---: | ---: | ---: | ---: | ---: |
| [`amo-noc`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 330,585 / 1,402,034 | 356,493 / 1,524,641 | 298,335 / 1,396,127 | 354,863 / 1,493,163 | — |
| [`amo-noc-gate-req`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 330,585 / 1,402,034 | 356,493 / 1,524,641 | 305,908 / 1,535,470 | 366,413 / 1,640,328 | — |
| [`dxa_rowmix-ingress`](../results/noc3_g18vgwmw_head_matrix_32/metrics.jsonl) | 60,568 / 146,974 | 60,568 / 146,790 | 60,568 / 146,994 | 60,568 / 146,812 | — |
| [`fence-noc`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 9,776 / 7,087 | 9,776 / 7,085 | 9,776 / 7,103 | 9,776 / 7,055 | — |
| [`fence-noc-one-port`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 2,444 / 7,146 | 2,444 / 7,158 | 2,444 / 7,173 | 2,444 / 7,183 | — |
| [`fence-noc-stress`](../results/noc3_g18vgwmw_head_matrix_24/metrics.jsonl) | — | 9,776 / 33,116 | — | 9,776 / 24,611 | — |
| [`fence_mp-noc-1`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 10,071 / 150,157 | 10,051 / 149,858 | 10,071 / 150,157 | 10,051 / 149,858 | — |
| [`fence_mp-noc-2`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 10,071 / 150,157 | 9,991 / 149,858 | 10,071 / 150,157 | 10,011 / 150,205 | — |
| [`flush_coherence-noc`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 378,880 / 286,518 | 378,880 / 286,534 | 378,880 / 286,393 | 378,880 / 286,452 | — |
| [`flush_coherence-noc-stress`](../results/noc3_g18vgwmw_head_matrix_25/metrics.jsonl) | — | 1,209,344 / 2,235,537 | — | 1,209,344 / 2,248,413 | — |
| [`model_parity-bwstream-dxa-group-gate`](../results/noc3_g18vgwmw_head_matrix_32/metrics.jsonl) | 22,816 / 16,022 | 22,816 / 15,893 | 22,816 / 16,917 | 22,816 / 17,005 | -0.517% |
| [`model_parity-bwstream-dxa-ports`](../results/noc3_g18vgwmw_head_matrix_32/metrics.jsonl) | 22,816 / 16,752 | 22,816 / 15,598 | 22,816 / 16,638 | 22,816 / 16,387 | +1.532% |
| [`model_parity-dxa-corun-ingress`](../results/noc3_g18vgwmw_head_matrix_32/metrics.jsonl) | 6,936 / 21,731 | 6,936 / 21,422 | 6,936 / 21,950 | 6,936 / 21,516 | +2.017% |
| [`noc-recursive-fanin`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 44,424 / 6,537 | — | 44,424 / 6,615 | — | — |
| [`parity-bwstream-coremask`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 83,136 / 74,445 | 83,136 / 74,519 | 41,744 / 73,937 | 41,744 / 73,941 | -0.005% |
| [`parity-bwstream-coremask-relaunch`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 13,560 / 19,224 | 13,560 / 19,221 | 41,340 / 54,637 | 41,340 / 54,679 | -0.077% |
| [`parity-bwstream-coremask-uneven`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 83,136 / 74,445 | 83,136 / 74,519 | 62,616 / 74,307 | 62,616 / 74,423 | -0.156% |
| [`parity-bwstream-coremask-uneven-clusters`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 43,392 / 19,855 | 43,392 / 20,115 | 22,048 / 19,886 | 22,048 / 20,101 | -1.070% |
| [`parity-bwstream-coremask-uneven012`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 83,136 / 74,445 | 83,136 / 74,519 | 62,616 / 74,307 | 62,616 / 74,425 | -0.159% |
| [`parity-bwstream-ingress`](../results/noc3_g18vgwmw_head_matrix_32/metrics.jsonl) | 3,940 / 13,786 | 3,940 / 13,658 | 3,940 / 13,861 | 3,940 / 13,732 | +0.939% |
| [`parity-bwstream-noc-gate`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 15,760 / 17,483 | 15,760 / 16,609 | 15,760 / 18,291 | 15,760 / 18,275 | +0.088% |
| [`parity-bwstream-noc-gate-req`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 15,760 / 17,483 | 15,760 / 16,609 | 15,760 / 18,231 | 15,760 / 18,224 | +0.038% |
| [`parity-bwstream-noc-relaunch`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 2,685 / 8,715 | 2,685 / 8,669 | 8,055 / 23,815 | 8,055 / 23,759 | +0.236% |
| [`parity-bwstream-noc-slices`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 31,520 / 16,628 | 31,520 / 16,379 | 31,520 / 16,590 | 31,520 / 16,295 | +1.810% |
| [`parity-chase-noc`](../results/noc3_g18vgwmw_head_matrix_30/metrics.jsonl) | 6,810 / 219,700 | 6,810 / 219,704 | 6,810 / 219,700 | 6,810 / 219,704 | -0.002% |
| [`parity-chase-noc-ptw`](../results/noc3_g18vgwmw_head_matrix_30/metrics.jsonl) | 6,817 / 122,459 | 6,817 / 122,478 | 6,817 / 127,588 | 6,817 / 127,607 | -0.015% |
| [`war_order-noc`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 2,688,240 / 1,744,705 | 2,688,240 / 1,745,829 | 2,688,240 / 1,745,144 | 2,688,240 / 1,745,473 | — |

All parity comparisons use the catalog tolerance and exact retired-instruction equality. The gap is signed as (SimX − RTL) / RTL.

| Standalone RV64IM on 4 cores / 2 per socket | Base | Candidate |
| --- | ---: | ---: |
| `run-simx-64im` | [PASS](../results/noc3_g18vgwmw_base_standalone_isa_00/isa-64im-standalone-kmu-fanout.log) | [PASS](../results/noc3_g18vgwmw_head_standalone_isa_00/isa-64im-standalone-kmu-fanout.log) |

### Selected fin5 replay (XLEN64)

| Case | Retired instructions | SimX cycles | rtlsim cycles | Gap |
| --- | ---: | ---: | ---: | ---: |
| [`P_grp`](../results/noc3_g18vgwmw_head_matrix_34/metrics.jsonl) | 31,520 | 17,779 | 17,458 | +1.839% |
| [`P_gate`](../results/noc3_g18vgwmw_head_matrix_34/metrics.jsonl) | 31,520 | 18,923 | 18,752 | +0.912% |
| [`P_gateq`](../results/noc3_g18vgwmw_head_matrix_34/metrics.jsonl) | 31,520 | 18,836 | 18,749 | +0.464% |
| [`P_dxoff`](../results/noc3_g18vgwmw_head_matrix_29/metrics.jsonl) | 22,816 | 16,115 | 15,873 | +1.525% |
| [`P_dxp2`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 22,816 | 16,752 | 15,598 | +7.398% |
| [`P_dxp2i`](../results/noc3_g18vgwmw_head_matrix_32/metrics.jsonl) | 22,816 | 16,638 | 16,387 | +1.532% |
| [`P_dxgrp`](../results/noc3_g18vgwmw_head_matrix_34/metrics.jsonl) | 22,816 | 17,508 | 16,755 | +4.494% |
| [`j_gate`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 15,760 | 18,291 | 18,275 | +0.088% |
| [`j_gateq`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 15,760 | 18,231 | 18,224 | +0.038% |
| [`j_slices`](../results/noc3_g18vgwmw_head_matrix_31/metrics.jsonl) | 31,520 | 16,590 | 16,295 | +1.810% |
| [`j_chase`](../results/noc3_g18vgwmw_head_matrix_30/metrics.jsonl) | 6,810 | 219,700 | 219,704 | -0.002% |
| [`j_dxap`](../results/noc3_g18vgwmw_head_matrix_34/metrics.jsonl) | 2,852 | 7,993 | 8,330 | -4.046% |
| [`j_one`](../results/noc3_g18vgwmw_head_matrix_33/metrics.jsonl) | 2,444 | 7,173 | 7,183 | -0.139% |
| [`j_fstress`](../results/noc3_g18vgwmw_head_matrix_24/metrics.jsonl) | 9,776 | — | 24,611 | — |

Paired knobs-off controls and PLRU probes

| Case | Retired instructions | SimX cycles | rtlsim cycles | Gap |
| --- | ---: | ---: | ---: | ---: |
| [`P_dxoff`](../results/noc3_g18vgwmw_head_matrix_29/metrics.jsonl) | 22,816 | 16,115 | 15,873 | +1.525% |
| [`P_dxp2`](../results/noc3_g18vgwmw_head_matrix_28/metrics.jsonl) | 22,816 | 16,752 | 15,598 | +7.398% |
| [`j_dxap_off`](../results/noc3_g18vgwmw_head_matrix_30/metrics.jsonl) | 2,852 | 7,984 | 8,292 | -3.714% |
| [`parity-chase-l2-plru`](../results/noc3_g18vgwmw_head_matrix_30/metrics.jsonl) | 678 | 17,389 | 17,371 | +0.104% |
| [`parity-chase-l2-plru-grouped`](../results/noc3_g18vgwmw_head_matrix_30/metrics.jsonl) | 678 | 18,428 | 18,411 | +0.092% |
| [`fence-noc-stress-plru`](../results/noc3_g18vgwmw_head_matrix_26/metrics.jsonl) | 9,776 | — | 24,611 | — |

### L2 admission, fencing, and page walks

The grouped VM/PTW pair retires 6,817 instructions on each model. SimX takes 127,588 cycles and rtlsim takes 127,607 cycles. The flat VM/PTW controls also match the base within each model. The grouped fence, message-passing modes 1 and 2, and stalled flush and fence cases all pass. An extra RTL fence-stress run with explicit PLRU also passes in 24,611 cycles. An extra grouped three-way PLRU chase takes 18,428 SimX cycles and 18,411 RTL cycles with 678 instructions on each, while the flat three-way PLRU case matches the base in each model. [VM/PTW SimX](../results/noc3_g18vgwmw_head_matrix_30/parity-chase-noc-ptw__ci_on.log) · [VM/PTW RTL](../results/noc3_g18vgwmw_head_matrix_18/parity-chase-noc-ptw__ci_on.log) · [PLRU fence stress](../results/noc3_g18vgwmw_head_matrix_26/fence-noc-stress-plru__plru.log)

### Replay interpretation

All 15 catalog model-parity pairs pass, with a maximum absolute gap of 2.017%. The `amo-noc` and `amo-noc-gate-req` functional tests pass in both engines. Their model-specific instruction counts and timings reproduce the NoC2 measurements.

The selected fin5 replay covers 14 rows. Thirteen run in both models and retire the same instruction totals across models. The fence stress row runs in RTL. The three cycle gaps at or above 3% reproduce the previous replay.

- `P_dxp2` is +7.398%. The wide two-port control has all NoC knobs off and exactly matches the new base in each model. Relative to `P_dxoff`, the extra DXA port adds 637 SimX cycles and saves 275 RTL cycles.
- `P_dxgrp` is +4.494%. Grouping and the response cap add 756 SimX cycles and 1,157 RTL cycles over `P_dxp2`. The absolute difference falls by 401 cycles, from 1,154 to 753. The lighter grouped DXA catalog pair is −0.517%.
- `j_dxap` is −4.046%. The matching no-ingress-cap control is −3.714% on the base and candidate. The ingress cap increases the absolute difference by 29 cycles, about 0.35% of the capped RTL total.

### Two-core Beacon on Rover

Each cell ran in `vortex.slice` through `~/vortex-rover/vxrun` under the 200 GiB memory limit. The cells ran serially under the shared RTL build lock. The common shape was `top_vortex`, XLEN64, two cores, eight threads per warp, four warps, VM and atomics enabled, and L2 with a 20-cycle link. The enabled cell used one two-socket group, two lanes, an 8 B/cycle request-side uplink cap, and a 16 B/cycle socket ingress cap.

| Cell | Max logic levels | LUT | FF | BRAM | DSP | Estimated MHz |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| [Base, knobs off](../results/noc3_beacon_00186b8b3_20261124/base_off.json) | 26 | 630,694 | 437,120 | 402 | 66 | 234.3 |
| [Candidate, knobs off](../results/noc3_beacon_00186b8b3_20261124/candidate_off.json) | 26 | 630,712 | 437,138 | 402 | 66 | 234.3 |
| [Candidate, knobs on](../results/noc3_beacon_00186b8b3_20261124/candidate_on.json) | 26 | 635,399 | 454,546 | 402 | 66 | 235.4 |

The raw off/off verdict was `DEEPER` because the `BUFFER_EX` expansion moved from line 646 to 802. The old and new names map to the same `busy_r.D` register. The 13-level path from `req_pending.in_fire_r[5]` has the same operation sequence and fanout. The other 12 added and removed watched paths pair after accounting for the `g_l2_links` scope. Two KMU valid paths moved from 6 to 8 levels, below the fix threshold of 12. The off/off mean of the 50 deepest paths stayed at 23.3. [Raw diff](../results/noc3_beacon_00186b8b3_20261124/candidate_off.beacon_diff.txt) · [Base path](../results/noc3_beacon_00186b8b3_20261124/base_off.busy_path.txt) · [Candidate path](../results/noc3_beacon_00186b8b3_20261124/candidate_off.busy_path.txt)

The enabled shape adds about 0.74% LUTs and 3.98% FFs relative to candidate off. Its NoC switch is 15 levels deep and both request-side gates are 9 levels. The enabled cell changes the topology, so the off/off register match applies only to the two flat cells. Hardware synthesis, physical timing, and XRT integration remain outside this run.

All 56 ICE jobs reached terminal states and the results cover 219 measurement legs. The two h200s batches containing the undersized decode input have the expected nonzero batch status. Both owned ICE source copies and local source staging trees were removed after fetching. `feature/noc2` remains at `e1ce1e1452128034df4265b4982414ddd6ec27c9`. `feature/noc3` is ready for the maintainer's merge decision.
