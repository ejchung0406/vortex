# NoC Step 2 — L2 slice-to-channel binding

## 2026-11-24 — base and address contract

`feature/noc-step2` was created at the requested merge `313246121ee7711253dbb3e1861053b3d4bd3e9e` in `vortex-exp/wt_noc_step2`. Its second parent is the NoC3 tip `ecaa43a00`. The baseline is the merge, before the later notes-only workspace changes.

Power-of-two slices share an invertible mapping. With `G = log2(granule)` and `S = log2(slices)` for more than one slice, take bit `G` and the `S-1` bits from `G+2` through `G+S` and XOR-fold higher address bits into them for the slice number. The channel-local address removes those `S` bits, compacts PA bit `G+1` to local bit `G`, and preserves all lower bits. With 32 slices at a 256 B granule, PA8/10/11/12/13 contribute to the raw selector and PA9 stays local. The set is the low set-index bits of the channel-local line address. The inverse folds the retained bits again to restore the removed bits. One slice is the identity. The default granule is 256 B. The fold is a modeling choice, and the separate partition hash remains Step 3 work.

The cache and external full-GPU bus carry physical addresses. Native rtlsim emits channel-local beat addresses, and its harness reverses the mapping before touching physical backing RAM. SimX keeps full physical addresses through backing RAM, and the timing model receives the equivalent channel-local address. Intra-channel DRAM row/bank hashing stays in both DRAM backends.

A host C++ probe passed the static reference vector, full-width addresses, same-line byte and sector offsets, and equal slice distribution over 2^20 lines for 1, 2, 4, 16, 32, and 64 slices. The expanded test catalog lint passed (1,162 entries).

## 2026-11-24 — intermediate gates and Astra review

GPT-6.2 Astra reviewed the design and implementation independently at `1a490d2c9`, `1452390fb` and `0127ac699`. A directed Verilator test reproduced the old stalled NC request payload and passed the registered two-entry correction. A second directed test reproduced the grouped arbiter holding back a cached channel on an unrelated NC grant, then passed concurrent cached and NC traffic across two channels and clients with independent round-robin grants. The bound adapters reject unsupported geometry before simulation. Generated symbolic C++ and SV checks with nine flat clients and four DRAM channels selected 16 L2 banks at the base, candidate default, and explicit 0, and four banks at explicit 1.

ICE disabled-mode comparisons for `1452390fb` against `313246121` showed exact instructions and cycles for all eight one-SM apps and the ten SimX and native RTL cache legs. The 22-SM pair was running when these results were recorded. The remaining binding changes use the enabled generate/model branch. In the first bound multi-core pair, sector merge retired 42,768 in each model (24,939 SimX cycles, 25,039 native RTL). Cold VM chase retired 2,412 (25,777 and 25,867) and strided uncached IO retired 248 (458 and 459). The AMO variant passed functionally in both and can vary retired instructions because it retries LR/SC. A separate fixed-instruction parity case covers the timing contract.

An exact `1452390fb` SimX `bwstream -n2048 -u4 -b32` sweep on the 32-SM profile spread active cores across both groups. Bandwidth is completed test traffic in 64-byte DRAM reads divided by the workload cycle count at 1.98 GHz. The 32 controllers were balanced within 0.8% by request count. Ramulator read row conflicts rose from 2.74% at one SM to 32.09% at 32, and average read queue occupancy rose from 2.73 to 53.96 bursts. The table is measured SimX output. The design target remains 1.544 TB/s from nominal two-stack bandwidth scaled by the part's measured efficiency.

| Streaming SMs | DRAM read GB/s |
|---:|---:|
| 1 | 96.872 |
| 4 | 385.205 |
| 6 | 575.166 |
| 8 | 746.131 |
| 10 | 878.183 |
| 12 | 883.705 |
| 16 | 841.057 |
| 20 | 856.875 |
| 22 | 867.701 |
| 24 | 860.789 |
| 28 | 870.116 |
| 32 | 872.334 |

Ten of 32 SMs (31%) reach 98% of the observed peak. The full 32-SM shape reaches 56.5% of the calibrated target. The part's 17% is the stated hard-minimum fraction at calibrated single-SM throughput. The design's scaled hard minimum is 23%, and its predicted 98% point is about 11 of 32 SMs.

The two-channel Beacon comparison at `0127ac699` preserved maximum and top-50 logic depth (26 and 23.3). Estimated model Fmax was 240.7 to 253.2 MHz, LUT 430,053 to 432,815, and FF 329,772 to 335,240. Both L2 memory-facing omega hierarchies disappeared. Pin-matched L2 egress depth rose from 6 to 14 across 4 bits, compared with 1,248 bits at depth 14 before the per-lane change. The pending counter and several AMO-to-response controls still had deeper endpoints. This is a small native-top logic comparison. It does not cover placement, routing, nor the external adapter.

## 2026-11-24 — checked-in design and handoff boundary

The named branch is `feature/noc-step2` at `dff377467e092540aa4224d3cde8d297622a12ba`, with implementation `1b82fc175` followed by the separate profile commit. Its base is `313246121`. There is no merge or push.

For the bound path, uncached client requests remain at the upstream queue head until the chosen channel accepts them. Requests from the cache and bypass merge independently at each channel, and returns merge independently at each client. Native rtlsim samples ready and valid before the edge and admits against the corresponding finite channel. SimX releases DRAM credit at completion and offers a mature response at most once per channel per cycle. A directed one-slot DRAM probe held the consumer stopped, then released it. Both Ramulator and BankDram accepted five reads and returned the five tags in order on consecutive cycles. Before the correction both stopped admission after the third read. Astra 6.2 cleared all static F2 and F3 findings on the final branch head.

Step 3 can place slices behind their owning partitions and route the inter-partition path using the same physical byte address and channel-local inverse. The slices already separate logical channel selection from the within-channel DRAM bank and row functions. The current 32 slices share one L2 across both SM groups. Partition address selection, replication, and fabric traffic measurements belong to Step 3.

The next part of Step 2 is the 48-channel shape from design §4.5. Select 24 channels per side with a remap whose inverse reconstructs the quotient and the removed address fields. Use the remap in the cache set/tag calculation and both memory-model adapters, and extend the RTL and C++ geometry for a non-power-of-two bank count. Run round trip and equal distribution across all 24 entries on each side before using that shape. An interim 32-channel configuration has only two thirds of the intended channel-level parallelism.

## 2026-11-24 — final-head validation

Final bound SimX ran in Phoenix job `16103192` and passed 13 checks, including address mapping, DRAM round trips, concurrent cached and uncached requests, same-channel I/O under a one-entry DRAM queue, fences, cold VM translation, and printf. The output-stall probe is archived as `results/noc_step2_dff377467/response-blocked.log` (Phoenix `16103178`). The native RTL checks ran in Phoenix job `16103217`.

For small Beacon, `0127ac699` and `dff377467` have byte-identical `hw/`, native RTL shim, and config. The archive `results/noc_step2_slice_beacon_0127ac699/` includes the two-tree comparison and the 214,748,364,800-byte Rover cgroup evidence. The raw endpoint diff flags deeper local paths. The previous omega instances registered part of the route. With the direct wiring, the per-channel merge drives the next buffer through a single combinational path. The endpoint growth remains visible as a timing tradeoff even though the top-level peak depth and the top-50 mean are unchanged. No golden data was updated.

All seven new cache cases passed on native RTL and SimX. The repository's `model_parity` comparator evaluated the archived XLEN64 run logs with its 5% threshold. The stand-alone runs enabled memory counters for diagnostics. The functional AMO case passed both legs with 93,484 and 93,771 retired instructions, respectively, and 146,292 and 146,294 cycles. Its retry loops can change instruction counts. The deterministic lazy case checks cycle parity separately.

| XLEN64 parity case | Retired instructions on each | SimX cycles | Native RTL cycles | Gap |
|---|---:|---:|---:|---:|
| Sector merge | 42,768 | 24,984 | 25,040 | 0.22% |
| Finite DRAM queue | 9,888 | 8,044 | 7,930 | 1.44% |
| Lazy fill | 413,664 | 778,443 | 761,299 | 2.25% |
| Cold VM chase | 2,412 | 25,894 | 25,958 | 0.25% |

`profile_check.py --metrics=peak,latency` at a 10% threshold reports the same `h200s` metrics on the base `313246121` (Phoenix `16104403`) and the final head (Phoenix `16103760`). L1 is 30.125 vs. 35 cycles and L2 is 241.438 vs. 273 cycles, outside the threshold on both revisions. DRAM is 628.652 vs. 658.7 cycles, within the threshold. The lane and configured nominal bandwidth metrics match their references.

### Final `h200g2` bandwidth

Phoenix job `16103219` completed all 12 streaming points at `dff377467` with `bwstream -n2048 -u4 -b32`. The 32-SM configuration stayed fixed. Partial counts activated cores across the two groups in the order 0, 16, 1, 17, and so on. Bandwidth uses the device DRAM read count at 64 B per read, the measured maximum core cycle count, and 1.98 GHz. The complete counts and cycles are in `results/noc_step2_dff377467/bwstream-table.tsv`.

| Streaming SMs | DRAM read GB/s |
|---:|---:|
| 1 | 97.724 |
| 4 | 387.270 |
| 6 | 575.519 |
| 8 | 745.590 |
| 10 | 877.079 |
| 12 | 1,051.373 |
| 16 | 1,350.051 |
| 20 | 1,459.027 |
| 22 | 1,449.173 |
| 24 | 1,436.190 |
| 28 | 1,438.293 |
| 32 | 1,420.205 |

The 32-SM result is 92.0% of the 1,544 GB/s calibrated target. The earliest measured point at or above 98% of the observed peak is 20 of 32 SMs (62.5%). The ideal minimum calculated from the final measured one-SM rate and observed peak is 14.93 SMs (46.7%). Design §4.2 gives the part's ideal minimum as 17% and the scaled design's ideal minimum as 23%, with about 11 SMs expected at 98% of peak. The modeled one-SM rate of 97.724 GB/s is below the design's calibrated assumption of about 210 GB/s.

At 32 SMs the spread between the busiest and quietest channel, divided by the per-channel mean, is 0.79%. The mean Ramulator queue depth grows from 2.85 bursts at one SM to 150.80 at 20 and 219.41 at 32. Per-read row conflicts grow from 2.74% to 14.99% and 26.22%, respectively. The final channel-local path raises the all-SM result from the earlier 872.334 GB/s run to 1,420.205 GB/s. The plateau and remaining calibrated throughput gap are open. The shape models 640 KiB of L2 per SM versus about 465 KiB on the part and has one shared L2 across both groups.

The final `h200g2` profile check (Phoenix `16103760`) reports 262.559 vs. 290 cycles for L2 and 512.344 vs. 546 cycles for DRAM, within 10%. L1 reports the same 30.125 vs. 35 cycles as `h200s` and remains outside 10%. Configured peaks agree with the profile. The measured bandwidth above comes from the streaming runs. The check and streaming phases were run separately, so job `16103219` was stopped after the sweep to avoid a duplicate profile check.

The 48-channel remap is deferred while the power-of-two bandwidth calibration and the local Beacon endpoint regressions remain open. The remap and its cache and memory verification requirements are recorded above.

### Disabled-mode identity and branch state

All 26 `SLICE_BIND=0` checks at `dff377467` matched exact base `313246121` for retired instructions and cycles. The ICE matrix covers the eight requested `h200s` applications on each of 1 and 22 SMs, and five cache cases each on SimX and native RTL. All four candidate jobs exited 0 (`7295460`, `7295462`, `7295467`, `7295469`). The strict comparison checks source revisions, argv, environment, exit status, and counters. Its table is archived at `results/noc_step2_dff377467/identity-disabled.tsv`.

Final local handoff is `feature/noc-step2` in `vortex-exp/wt_noc_step2`. Its clean HEAD is `dff377467e092540aa4224d3cde8d297622a12ba`, directly following `1b82fc17589e8cf0bb24fa232e90c424d073c704` on top of `313246121`. The last commit contains only `profiles/h200g2.json`.
