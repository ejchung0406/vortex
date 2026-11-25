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

## 2026-11-24 — `h200g2` lean load and TMA bandwidth resweep

SimX runs use the unchanged `dff377467` implementation and the same 32-SM, two-group, 32-channel `h200g2` profile throughout. The 16 normal spread points and four packed points preserve the 648 B/clock cap per group. Spread masks choose cores `0,16,1,17,…`. Packed masks activate `0…N-1`. Only 8- and 16-SM points use both placements. A separate packed control at each of those counts sets `VX_CFG_NOC_UPLINK_BYTES=0` to measure the group return gate's contribution. The per-SM 113 B/clock ingress gate and the memory configuration stay the same in the controls.

`ldstream -t2048 -b8 -u8 -m16 -a -l -gN` gives `N` CTAs and 16 MiB of disjoint cold reads per CTA. A 2,048-thread CTA occupies all 64 × 32 warp slots of an SM. `ldstream` has no core-mask argument, so the scratch measurement wrapper writes the existing KMU core-disable DCR before enqueueing the original launch. It also reads per-core retired instructions to verify one CTA on each masked-in SM and zero retired instructions on every other SM. The app reports its own timed-loop rate separately from the device-level counters.

`bwstream_dxa -e4096 -n256 -d2 -c0` uses its built-in `-M` for subset masks and the default grid of seven CTAs per active SM. Each CTA transfers 4 MiB, so every SM moves 28 MiB. The ordinary benchmark kernel is measured. Retired instructions are checked on every SM, and DXA completed transfers must equal 256 per CTA. The first TMA check passed with no Ramulator file because the legacy descriptor writes kept the simulated device alive through a retained queue. A scratch-only interposer submits the same descriptor DCR writes synchronously on the app's queue. The 1-SM comparison before and after this change had identical instructions, cycles, and DRAM counts. The device closes and finalizes the Ramulator log with the interposer.

Both sweep tables use the prior `bwstream` accounting. DRAM read bandwidth is `reads × 64 B × 1.98 GHz / max(core cycles)`. Queue occupancy is the mean of the 32 Ramulator controller averages in bursts, and read row conflict rate is `conflicts / (hits + misses + conflicts)` over all classified reads. These Ramulator averages cover the device lifetime, including startup and counter queries. The table values come from Phoenix under `gts-hkim358`. The experiment changes no repository source.

### Lean load — `ldstream`

| SMs | Instructions | Cycles | DRAM reads | GB/s |
|---:|---:|---:|---:|---:|
| 1 | 250,196 | 169,613 | 263,955 | 197.204 |
| 4 | 1,000,784 | 177,439 | 1,063,967 | 759.844 |
| 6 | 1,501,176 | 181,934 | 1,595,945 | 1,111.602 |
| 8 | 2,001,568 | 207,095 | 2,127,921 | 1,302.060 |
| 10 | 2,501,960 | 256,264 | 2,660,422 | 1,315.552 |
| 12 | 3,002,352 | 309,658 | 3,191,936 | 1,306.222 |
| 16 | 4,003,136 | 414,708 | 4,256,019 | 1,300.488 |
| 20 | 5,003,920 | 522,088 | 5,320,143 | 1,291.293 |
| 22 | 5,504,312 | 584,474 | 5,853,565 | 1,269.113 |
| 24 | 6,004,704 | 644,252 | 6,384,631 | 1,255.814 |
| 28 | 7,005,488 | 755,165 | 7,448,609 | 1,249.909 |
| 32 | 8,006,272 | 873,574 | 8,512,782 | 1,234.858 |

### TMA — `bwstream_dxa`

| SMs | Instructions | Cycles | DRAM reads | GB/s |
|---:|---:|---:|---:|---:|
| 1 | 70,546 | 360,963 | 458,765 | 161.054 |
| 8 | 564,368 | 397,463 | 3,670,052 | 1,170.094 |
| 16 | 1,128,736 | 684,083 | 7,340,098 | 1,359.685 |
| 32 | 2,257,472 | 1,460,847 | 14,680,396 | 1,273.439 |

### Placement and return-gate control

| Kernel | SMs | Packed GB/s | Spread GB/s | Spread gain | Packed with gate off GB/s | Packed gain with gate off |
|:---|---:|---:|---:|---:|---:|---:|
| Lean load | 8 | 1,087.257 | 1,302.060 | +19.8% | 1,268.735 | +16.7% |
| Lean load | 16 | 1,109.223 | 1,300.488 | +17.2% | 1,223.690 | +10.3% |
| TMA | 8 | 1,141.729 | 1,170.094 | +2.5% | 1,107.045 | -3.0% |
| TMA | 16 | 1,211.640 | 1,359.685 | +12.2% | 1,293.963 | +6.8% |

### Queue and shared-L2 counters by active SM count

| Kernel | SMs | Mean queue (bursts/channel) | Mean read queue (bursts/channel) | Read row conflicts | Ramulator cycles per read burst | L2 MSHR stalls per 1,000 reads |
|:---|---:|---:|---:|---:|---:|---:|
| Lean load | 1 | 7.14 | 6.99 | 2.78% | 193.4 | 0.0 |
| Lean load | 4 | 52.68 | 32.14 | 4.33% | 141.5 | 0.1 |
| Lean load | 6 | 125.39 | 65.04 | 6.23% | 248.9 | 0.1 |
| Lean load | 8 | 246.20 | 153.49 | 8.69% | 502.5 | 31.2 |
| Lean load | 10 | 246.32 | 151.29 | 10.94% | 480.8 | 41.1 |
| Lean load | 12 | 264.43 | 177.31 | 15.31% | 573.0 | 60.2 |
| Lean load | 16 | 248.88 | 174.83 | 18.12% | 561.8 | 89.0 |
| Lean load | 20 | 264.03 | 173.84 | 20.99% | 607.0 | 86.3 |
| Lean load | 22 | 243.37 | 147.67 | 21.52% | 468.6 | 78.0 |
| Lean load | 24 | 249.50 | 153.36 | 23.14% | 554.4 | 82.1 |
| Lean load | 28 | 239.53 | 147.32 | 25.12% | 536.3 | 81.9 |
| Lean load | 32 | 223.93 | 142.25 | 26.56% | 561.1 | 76.4 |
| TMA | 1 | 5.24 | 5.22 | 2.90% | 106.1 | 0.0 |
| TMA | 8 | 103.82 | 102.55 | 4.84% | 285.7 | 3.1 |
| TMA | 16 | 283.53 | 230.96 | 7.37% | 554.8 | 139.3 |
| TMA | 32 | 222.81 | 197.73 | 9.94% | 507.0 | 139.6 |

The Ramulator latency column divides the controllers' summed latency by the accepted read burst count. The accepted count was exactly twice the device's 64-byte read count in every run. The host's XLEN64 SimX MPM DCR high-half reads return zero for these counters, so large accumulated values can wrap in the host readout. This affects the raw 250.6-cycle device latency reported for 32-SM TMA. The tables use Ramulator's independent measurements for latency and preserve the raw readout in the archive. L2 MSHR stalls and bank-arbitration collisions are events summed across banks. Dividing by accepted L2 reads gives events per 1,000 reads.

### Comparison with the design target

| Kernel | 1-SM GB/s | Observed peak GB/s (SMs) | First sampled SM count at 98% of observed peak | 32-SM GB/s | Ideal floor at observed peak | Ideal floor at 1,544 GB/s |
|:---|---:|---:|---:|---:|---:|---:|
| Lean load | 197.204 | 1,315.552 (10) | 8 of 32 (25%) | 1,234.858 | 6.67 SMs (20.8%) | 7.83 SMs (24.5%) |
| TMA | 161.054 | 1,359.685 (16) | 16 of 32 (50%) | 1,273.439 | 8.44 SMs (26.4%) | 9.59 SMs (30.0%) |

The ideal floor divides the bandwidth goal by the measured one-SM rate with linear scaling. The design's 23% uses about 210 GB/s per SM and the 1,544 GB/s roof. Using that same roof, the measured one-SM rates raise the floor by 1.5 percentage points for lean load and 7.0 points for TMA. The measured peaks are 85.2% and 88.1% of that roof. Neither sweep reaches 98% of the design roof (1,513.120 GB/s). Relative to the design estimate of about 11 SMs, lean load crosses 98% of its own lower peak with three fewer SMs at the first qualifying sample. TMA first crosses at the 16-SM sample, five above the estimate, with no measured point between eight and 16.

At one SM the lean kernel reads 99.6 B/clock, about twice the 49.4 B/clock of the earlier `bwstream -n2048 -u4 -b32` sweep. The same lean workload on `h200s` retired the same 250,196 instructions and took 173,098 device cycles for 194.441 GB/s. On `h200g2` it took 169,613 device cycles for 197.204 GB/s. The timed loop alone moved 16 MiB in 163,676 cycles on `h200g2`, or 203.0 GB/s. Every lean run retired 250,196 instructions on each selected SM. The measured aggregate is still 94% of linear single-SM scaling at six SMs, then flattens. The earlier 20-SM result depended on the kernel. The lean run crosses 98% of a lower peak and at 32 SMs is 6.1% below its own peak.

With lean load spread at eight SMs, the average Ramulator read queue has grown to 153.49 bursts per channel from 6.99 at one SM, and mean device DRAM read latency is 447.6 cycles, up from 255.6. The read queue stays in the 142–177 range over the remaining points. At 32 SMs the read row conflict rate is 26.56%, up from 10.94% at the 10-SM peak and 2.78% at one SM. L2 MSHR stalls rise from 41.1 per 1,000 accepted reads at the peak to 76.4 at 32 SMs, and L2 bank-arbitration collisions rise from 1,116 to 1,189 per 1,000 reads. More SMs increase pressure on the same L2 slices and HBM channels, and the bandwidth declines while the 32 channels remain balanced within 3.3% by read count.

The 648 B/clock return gate allows about 1,283 GB/s of charged responses per group at 1.98 GHz. Removing it recovers 84.5% of the lean-load placement gap at eight SMs and 59.8% at 16 SMs. For TMA it recovers 55.6% at 16 SMs. The remaining timing difference between ungated packed and spread also includes the single group's switch and its 32 return lanes versus two switches and 64 lanes. The control removes the gate's one-cycle grant stage as well as its credit limit. At eight SMs TMA slows when the gate is removed. Its mean Ramulator read queue rises from 101.53 to 120.63 bursts, its DRAM read latency from 369.3 to 417.0 cycles, and its L2 MSHR stalls from 6,873 to 48,084. This point shows no bandwidth benefit from bypassing the gate.

At 16 SMs the measured spread/packed ratios are 1.172 for lean loads and 1.122 for TMA. Design §4.2 predicts about 1.21 once the mixed-page memory roof and a single group gate are reached. The spread lean peak carries about 332 B/clock of DRAM-read traffic per group if split evenly. This is a proxy using the balanced SM work and the device read count. The gate also sees cached and control traffic and can throttle short bursts. The counters have no direct per-group credit-stall count. Together with the packed control and shared-path counters, the spread rates support shared L2 and DRAM pressure as the main limit at higher counts.

TMA's one-SM rate is 81.3 B/clock, and the DXA no-slot stall counter is zero throughout. The 1-SM result shows less offered bandwidth than lean load. These measurements do not isolate its per-SM limit further. At eight SMs, TMA reaches 90.8% of linear scaling from one SM. At its 16-SM peak, evenly splitting the device DRAM bytes gives about 343 B/clock per group. The read queue grows from 5.22 bursts at one SM to 230.96 at 16 and is still 197.73 at 32. Read row conflicts rise from 2.90% to 7.37% and 9.94%. L2 MSHR stalls grow from 3.1 per 1,000 reads at eight SMs to 139.3 at 16 and 139.6 at 32, and average DXA issue-to-completion time rises from 2,369.8 core cycles per tile at eight SMs to 4,762.5 and 10,728.7 cycles. TMA bandwidth falls 6.3% from 16 to 32 even as Ramulator's mean queue and mean burst latency both ease. The much longer DXA completion time and persistent L2 stalls show pressure in the shared path beyond the DRAM controller's mean latency. The counters establish the separate contributions of placement and shared-path contention, with no precise split between L2 and DRAM service.

All 24 accepted runs passed functionally and completed the per-SM placement and work checks. The complete metrics, raw controller and app logs, launch manifests, and Phoenix accounting are in `results/noc_step2_resweep_dff377467/`.
