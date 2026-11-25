# DXA rate on h200s

## 2026-11-24 04:39 EST

Stage 1 only. Worktree `wt_tma`, branch `exp/tma-rate`, base `8a56e7483` on `feature/dxa-borrowing`. Measurements use one h200s SM and 2 MiB VM heap pages. The first batch remeasures the current head because the rates in the task came from several earlier heads. The H200 comparison uses the 105–108 B/SM-clock ceiling and the 32–128 KiB single-copy latency slope. The single-copy 16 KiB end-to-end rate is a separate measurement.

ICE is the first choice for these SimX jobs. The stage needs no rtlsim.

## 2026-11-24 05:25 EST

The clean and instrumented builds agree to the cycle on the three standard shapes and the single transfer. The clean rates are 66.13, 73.15, 73.84 and 10.49 B/clock. The 32→128 KiB single-transfer slope is 0.382 lines/clock. The source walks disjoint cold tiles. No worker runs out of credits at 16 KiB, but the 32/64/128 KiB tests record 20,237/23,653/24,922 issue attempts blocked on the 256 in-flight slots.

The current one-warp loop is 39 instructions. The older 48-instruction loop in the task preceded the uniform-control annotations. Frozen binary dumps and the current warp timing trace are in the ICE results. The split-two test improves the single-copy slope to 0.734 lines/clock and the 7×2 case to 80.24 B/clock. It passes the byte-pattern rowmix check.

## 2026-11-24 05:40 EST

Fixed four-way splitting crosses the single-copy target but only admits four transfers on 16 workers. With LPC it delivers 1.245 lines/clock on the 32→128 KiB slope, while the 8 KiB stream drops to about 39 B/clock and decode loses more than a third of its multiply bandwidth. The two-worker stack is faster on 7×2 (about 86 B/clock) and still below the target. Added three feed controls using the existing 228 KiB shared memory to test 14×2 at 8 KiB, 1×7 at 32 KiB and 1×3 at 64 KiB.

## 2026-11-24 06:08 EST

The main experiment matrix and the feed controls finished on ICE. The read-overlap probes use SimX on Phoenix `gts-hkim358` because the ICE queue reached its total-job gate. They use a scratch source snapshot and a scratch build per configuration. The main ICE results include a clean-versus-instrumented comparison on identical shapes.

## 2026-11-24 06:12 EST

Read overlap with LPC reaches 83.84/89.75/87.60 B/clock on the three main shapes. The four-way split is the only family that has cleared the single-copy gate so far. A fourth Phoenix job checks that split with LPC and read overlap together. All 168 ICE results were fetched and the two owned ICE source trees were removed.

## 2026-11-24 06:22 EST

All 203 SimX points passed, including 168 on ICE and 35 on Phoenix. The four Phoenix jobs completed on `gts-hkim358` with 4.23 allocated CPU-hours in total. The two-way stack with read overlap reaches 96.43 B/clock on the 7×2 shape. The four-way split with LPC and read overlap reaches 1.245 lines/clock alone and at most 69.92 B/clock on the three multi-transfer shapes. The stage ends with the dynamic line-scheduling recommendation below. The probes are uncommitted and there is no RTL implementation or rtlsim result.

## A. Rate budget

All simulator numbers below are one SM at h200s on the same base. A is 3 CTAs × depth 4 at 16 KiB, B is 7 × 2 at 16 KiB, C is 7 × 4 at 8 KiB, and 1 is one CTA × depth 1 at 16 KiB. Each CTA has one warp. The H200 multi-transfer reference is 105–108 B/SM-clock, choosing 107 for arithmetic. At 16 KiB and one outstanding transfer the H200's entire loop measures 11.5 B/clock over its DRAM source and 17.6 over its 8 MB source. The independently measured single-copy service target uses the 32→128 KiB transfer latency slope, 1.67 lines/clock. The SimX sweep touches distinct cold tiles. [H200 measurements](../results_gpu/h200_smr_summary.txt)

### Two-port request slots

The counters partition each of two 64 B input opportunities per sampled clock, from first issue through the copy stream and device tail. The rate columns are whole-kernel B/clock equivalents. The boundary correction reconciles the sampled interval with the kernel cycle counter. The 21 B/clock difference between the two-port capacity and the chosen H200 reference is subtracted once. `No transfer` includes issuer and boundary idle. `Other port` means work ran only on workers assigned to the other port. `Tail` means the port had active transfers with all lines already issued. `Offer blocked` measures downstream backpressure at the DXA port. It includes upstream effects of both full L2 response queues and capture/response backpressure, whose counter domains overlap.

| Shape | Measured B/clk | No transfer | Other port | Tail | Offer blocked | Setup/generator/credits | Boundary | Capacity slack | Gap to 107 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 66.134 | 4.357 | 0.946 | 28.468 | 30.509 | 0.127 | −2.542 | −21.000 | 40.866 |
| B | 73.150 | 2.962 | 3.458 | 28.142 | 21.323 | 0.182 | −1.217 | −21.000 | 33.850 |
| C | 73.840 | 2.253 | 0.317 | 50.939 | 1.645 | 0.233 | −1.228 | −21.000 | 33.160 |
| 1 | 10.487 | 37.187 | 46.899 | 36.327 | 0.044 | 0.369 | −3.313 | −21.000 | 96.513 |

The last column is a capacity budget, including for the single-copy loop. For that loop the like-sized measured part DRAM rate is 11.5 B/clock and the observed gap is 1.01 B/clock. The end-to-end methods and source sizes differ, so use the 32–128 KiB slope for the single-copy architecture gate. Capturing one line every two clocks cannot reach 0.835 lines/clock on one worker, even if each request arrives instantly.

| Measured diagnostic | A | B | C | 1 |
|---|---:|---:|---:|---:|
| Clocks per CTA arrival, after the first | 727.2 | 1,535.6 | 769.2 | 1,519.5 |
| Older logical transfers in flight at next issue, per CTA | 1.859 | 0.656 | 0.946 | 0 |
| Average outstanding over the measured kernel, all CTAs | 7.23 | 10.50 | 10.82 | 0.74 |
| Transfer enqueue→first capture, clocks | 796.6 | 709.3 | 660.0 | 610.4 |
| Last request issue→finish, clocks | 976.8 | 822.9 | 783.6 | 887.8 |
| Capture spacing within a transfer, clocks | 3.880 | 6.423 | 4.222 | 2.112 |
| L2 replay-full stalls, bank-clocks | 595,537 | 570,240 | 139,714 | 0 |
| Eligible LSU read on DMA-active clocks | 71,586/152,950 | 101,601/304,145 | 153,987/300,710 | 0/24,768 |

L2 replay-full accounts for all but six stalled bank-clocks across the three multi-transfer runs. The counts are aggregated over 16 banks. A large number of these stalls can coincide on one clock. C has 1.645 B/clock of blocked port offers, so removing every such offer stall at fixed demand still leaves a 31.5 B/clock gap. The stat also checks the row engines that service ordinary reads with `LMEM_RD_ROW=1`. The older probe of the legacy read engines missed those readers. Read eligibility on a DMA-active clock measures an arbitration conflict opportunity. Actual service also depends on banks and other requesters.

For A, LPC changes mean enqueue→finish from 1,791 to 1,146 clocks and capture spacing from 3.880 to 1.950. Port offer backpressure falls from 30.509 to 0.200 B/clock. With fewer requests to feed it, the tail-only bucket grows from 28.468 to 59.715. At the next CTA issue the mean number of older copies falls from 1.859 to 0.681 and the arrival interval grows from 727 to 868 clocks. Eligible-read overlap rises to 103,693 of 146,107 DMA clocks. Thus faster worker delivery removes contention on the response path and exposes both burstiness and issuing-loop delays. The 107 B/clock supply requirement alone calls for interarrival intervals no longer than 459/1,072/536 clocks on A/B/C.

With LPC, the disjoint-read probe reduces A's arrival interval from 868 to 570 clocks, and C's from 960 to 637. The remaining eligible read requests waiting on DMA drop from 103,693 to 2,698 DMA clocks on A and from 251,693 to 12,509 on C. The larger C stream then sees more L2 replay stalls, from 121,756 to 453,557 bank-clocks, as more transfers overlap. The earlier replacement check attributed 584 evictions to the whole depth-two run and found no DXA refetches. [A LPC counters](../results/tma8a56_lpc_a/run.log), [A LPC with read overlap](../results/tma8a56_lpcrw_a/run.log), [C LPC with read overlap](../results/tma8a56_lpcrw_c/run.log), [earlier L2 replacement check](dxa-port.md)

For the single 16 KiB loop, the exact 99,991 clocks are 39,065 from transfer enqueue through first capture, 34,464 from first to last capture, 192 from last capture through finish, 23,184 turnaround between transfers and 3,086 kernel boundaries. Its active port had 69 blocked offers and L2 had zero replay-full cycles. Per transfer, the observed 538.50-clock capture window for 255 line intervals partitions into 152.69 at the part's 1.67 line/clock slope, 102.31 for one 1-line/clock port, 255 at the one-capture-every-other-clock limit, and 28.50 above that limit. This last partition is a pacing model using the measured part slope. The 610.39-clock first-capture time includes a cold memory fetch and pipeline startup. The H200 fixed component is about 340 for L2-resident tiles and 480–570 for DRAM tiles, measured with different issue/poll boundaries. For the large-copy sweep the 256 resident credits matter in addition to capture pacing.

### Issuing warp

The 48-instruction historical loop's 13 control/SFU/gather instructions comprised two `SPLIT/JOIN` pairs to enter and reconverge the divergent issue and consume paths, one inverted `PRED` to select lanes for the consume path, three conditional branches for loop/control decisions, two synchronous `BAR` instructions, `BAR.EXPECT`, `WGATHER` and `DXA.ISSUE`. The split/join/predicate and branches force the front end to resolve the next PC or lane mask. The sync barriers also stop fetch. The first waits for the DMA transaction, and the second protects the slot from reuse before the reads finish. `BAR.EXPECT` and `DXA.ISSUE` can proceed asynchronously. They serialize on the SFU/event path, while the gather is an explicit register dependency feeding DXA. [Historical disassembly](../results/smr_objd_dxa/kernel.dump)

The current `-c0` loop executes 39 instructions and removes all five `SPLIT/JOIN/PRED` operations from the steady path through uniform control. The four remaining branches check loop exit (PC suffix 1b4), issuing warp (1b8), valid lookahead (1c0) and consume mode (238). Each measured branch takes 19–20 clocks, of which 9–10 reach resolve, 9 restart and decode, and one issues the next instruction. The two `BAR` sites (224 and 1ac) still stop fetch. `BAR.EXPECT` (1f8) incurs an overlapping core-wide LSU drain of 13.51/8.87/9.85/0 clocks on A/B/C/1. The following `WGATHER` (204) feeds `DXA` (208) in 13–14.4 clocks. The remaining serial chains include two unsigned remainders, slot/address arithmetic and three barrier-array stack loads. The first two reloads cost about 43 clocks to the first BAR and the third about 20–22 clocks to expect. [Current disassembly and trace](../results/tma8a56_stats_a/kernel.dump)

| Measured warp-0 issue→next DXA issue, clocks per steady iteration | A | B | C | 1 |
|---|---:|---:|---:|---:|
| Four branches | 77.00 | 78.48 | 77.61 | 77.00 |
| Sync BAR control, excluding drain and unresolved event | 41.64 | 44.70 | 50.65 | 36.00 |
| Sync BAR LSU drain | 10.53 | 10.51 | 7.71 | 12.00 |
| Unresolved BAR/DMA event wait | 57.27 | 863.74 | 3.13 | 1,053.54 |
| Shared load→dependent accumulator | 206.79 | 208.84 | 250.16 | 24.00 |
| WGATHER→DXA | 13.00 | 13.28 | 14.37 | 13.00 |
| Gaps following asynchronous expect/DXA | 2.00 | 2.58 | 3.06 | 2.00 |
| Remaining dependency, issue and frontend gaps | 323.62 | 331.15 | 365.11 | 302.00 |
| Total | 731.85 | 1,553.28 | 771.80 | 1,519.54 |

The H200 whole producer loop measures 289.5–302.5 clocks with a try-wait, and its issue-only loop 104 with small tiles. The H200 check-and-timestamp ring is a different loop and takes 496 clocks per issue, or 585–594 with the tight wait variants. SimX without unresolved BAR-event time in these traces still spends 675/690/769/466 clocks on warp 0. The current kernel's shorter ring experiment removes the issuer test, consume test, per-iteration lookahead branch, two remainders and barrier stack reloads, keeping the copy wait and slot reuse barrier. A deeper fetch buffer can feed independent instructions and keeps the same control round trips.

The short ring's current binary executes 29 steady instructions and seven control/SFU/gather operations. The two BARs, the loop branch and a compiler-generated predicate at the ring wrap stop fetch. The packet gather and one dependence into DXA remain. The kernel change only applies to the single-warp benchmark's `-c0` path.

### Measured conditional contributions

The port counters identify where capacity is lost. The following fixed sequence measures how much each intervention recovers at the observed demand. It starts at baseline and adds LPC, Q8, two-way splitting, then the short ring and faster expect together, followed by read overlap. Each entry is the throughput difference from the previous step. The contributions and remaining gap add to the original gap to 107 B/clock. The ordering matters because the changes affect backpressure, arrival rate and batching together. Totals use unrounded rates.

| Shape | Capture LPC | L2 Q8 after LPC | Two workers after Q8 | Short loop and expect after splitting | Read overlap after those changes | Remaining gap | Original gap |
|---|---:|---:|---:|---:|---:|---:|---:|
| A | −10.398 | −3.832 | +18.599 | +5.836 | +17.931 | 12.730 | 40.866 |
| B | −0.150 | +4.943 | +5.655 | +2.485 | +10.349 | 10.569 | 33.850 |
| C | −14.894 | −0.175 | +15.181 | +0.714 | +1.029 | 31.305 | 33.160 |
| 1 | +1.072 | 0 | +1.127 | +2.369 | 0 | 91.945 | 96.513 |

For the real 16 KiB single-copy DRAM loop reference, substitute a remaining gap of −3.555 and an original gap of 1.013 B/clock in the last row. The slope remains the single-transfer design criterion.

## B. SimX options

All rates in the table are measured in SimX. The main matrix and depth controls ran on ICE, and the read-overlap variants ran on Phoenix. The copy kernel disassemblies match across clusters for the 16 paired A/B/C/1 runs. `A`, `B`, `C`, `1` and `Loads` are logical bytes divided by reported kernel cycles. `Decode` is the app's FP16 weight bytes per cycle in its multiply window (`-m16 -n8192 -k4096`, `KSTEPS=2 PIPE=6 XFER=8192`). `GEMM` is 8 MiB of FP16 B weights divided by kernel cycles (`-m16 -n1024 -k4096 -s1`, `MTILES=1 PROFILE_ENABLE`). The GEMM's `-s1` selects DXA staging, so it is a consumer/correctness control. Loads (`bwstream -n2048 -u4 -b32`) are the DXA-free control. The single-copy slope is 1,536 additional lines divided by the 128 KiB minus 32 KiB transfer span from DXA perf. A dash means the size sweep was omitted for that option. The depth controls change the copy app arguments and use the corresponding baseline or LPC + short-loop bystander results.

All runs retain the two modeled L2 ports and a maximum of 128 B of DXA/store writes per clock. The 16 workers, 256 in-flight credits per worker, port count, queue depths and experimental timing choices are model settings. The part shows a 1.67-line/clock stream in a single transfer and sustained progress with up to 16 outstanding. Those measurements constrain the bandwidth and logical concurrency. They leave the internal lane, tag and port organization unobserved.

The depth-only controls use the same executables. Raising A from depth 4 to 8 reduces resident CTAs from 3 to 1, and raising C from depth 4 to 8 reduces them from 7 to 3. The rates go from 66.13 to 32.59 and 73.84 to 40.61 B/clock, respectively. On a fixed one-CTA 16 KiB run, depth 1→8 gives 10.49→29.32, or 13.49→42.00 with LPC and the short loop. Reusing the depth-4 16 KiB result for B's depth 2→4 control gives 73.15→66.13 as 7 CTAs become 3. At 4 KiB, seven CTAs at depth 8 reach 46.75 or 60.20 with LPC and the short loop. The independent issue count and the number of live transfer contexts determine supply as well as ring depth.

The extra feed placements use disjoint LMEM slots and stay inside the 228 KiB budget. Below, rates are in order for 8 KiB at 14×2, 32 KiB at 1×7 and 64 KiB at 1×3. Base gives 75.99/54.84/49.42, split 2 gives 68.64/39.23/72.05, split 2 + LPC + expect + short loop gives 70.24/59.19/67.62, and the equivalent split 4 gives 37.29/64.40/71.00. Adding Q8 and read overlap to the two-way fast configuration gives 71.21/88.28/82.25. In particular, increasing per-CTA depth cannot supply the engine fast enough when the one issuing warp remains the limiting producer. The four-way split's limited logical concurrency also binds in the 14-CTA feed.

| Option | A | B | C | 1 | Single slope, lines/clk | Decode | Loads | GEMM |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Baseline | 66.13 | 73.15 | 73.84 | 10.49 | 0.382 | 58.94 | 42.53 | 16.19 |
| Depth control | 32.59 | 66.13 | 40.61 | 29.32 | — | 58.94 | 42.53 | 16.19 |
| S2 | 70.03 | 80.24 | 73.64 | 12.52 | 0.734 | 58.63 | 42.53 | 16.54 |
| S4 | 67.86 | 66.76 | 38.76 | 12.85 | 1.206 | 36.54 | 42.53 | 16.96 |
| Pair | 65.74 | 73.24 | 74.50 | 10.13 | 0.382 | 58.97 | 42.53 | 16.61 |
| LPC | 55.74 | 73.00 | 58.95 | 11.56 | 0.377 | 59.01 | 42.53 | 16.13 |
| Q8 | 69.27 | 77.10 | 65.59 | 10.49 | — | 58.99 | 42.53 | 16.22 |
| Expect | 67.66 | 73.14 | 77.59 | 10.49 | — | 59.09 | 42.53 | 16.36 |
| Fetch8 | 65.84 | 72.88 | 66.27 | 10.47 | — | 58.80 | 43.63 | 16.33 |
| Short loop | 71.10 | 75.51 | 78.41 | 12.05 | — | 58.94 | 42.53 | 16.19 |
| S2 + LPC | 70.36 | 82.96 | 74.02 | 12.69 | 0.726 | 58.64 | 42.53 | 16.51 |
| S4 + LPC | 69.93 | 64.82 | 39.07 | 12.85 | 1.245 | 36.55 | 42.53 | 16.90 |
| Pair + LPC | 69.35 | 74.24 | 75.98 | 10.77 | 0.380 | 59.01 | 42.53 | 16.57 |
| LPC + short loop | 70.04 | 75.68 | 76.82 | 13.49 | — | 59.01 | 42.53 | 16.13 |
| LPC + short loop + depth control | 46.24 | 70.04 | 58.57 | 42.00 | — | 59.01 | 42.53 | 16.13 |
| LPC + Q8 | 51.90 | 77.94 | 58.77 | 11.56 | — | 59.03 | 42.53 | 16.24 |
| S2 + LPC + Q8 | 70.50 | 83.60 | 73.95 | 12.69 | 0.725 | 58.65 | 42.53 | 16.56 |
| S2 + LPC + Q8 + expect + short loop | 76.34 | 86.08 | 74.67 | 15.05 | 0.726 | 59.00 | 42.53 | 16.52 |
| Read overlap | 72.76 | 81.82 | 80.16 | 10.49 | — | 58.94 | 42.53 | 18.74 |
| LPC + read overlap | 83.84 | 89.75 | 87.60 | 11.56 | — | 59.01 | 42.53 | 18.67 |
| S4 + LPC + read overlap | 69.92 | 65.92 | 38.81 | 12.85 | 1.245 | 36.55 | 42.53 | 18.69 |
| S2 + LPC + Q8 + expect + short loop + read overlap | 94.27 | 96.43 | 75.69 | 15.05 | 0.726 | 59.00 | 42.53 | 18.98 |

The hardware costs below are inferred from the current RTL structure. The behavior and limits measured on the part are in the H200 log.

- **S2 and S4** split contiguous line ranges across groups of the existing 16 workers. They add setup range seeds, atomic group assignment and one completion join per transfer. The LD/ST links and LMEM beat widths are unchanged. Fixed resource reservation limits simultaneous logical transfers to eight or four. The measured concurrency calls for independent logical descriptors with parallel line delivery.
- **Pair** allows one worker to use both halves of an existing 128 B beat. It adds a second pending 64 B line per worker (1 KiB across 16 workers), metadata and matching and two-tag reclaim. The probe keeps an eligible half for up to two clocks to pair with its neighbor. The probe searches outstanding metadata. Equivalent hardware needs row tracking or a cheaper bounded compare and may have different hit timing. Both parts still fit a single row of writes. Each worker's input remains at most one line per cycle even with LPC.
- **LPC** enables the existing capture refill path in `VX_dxa_smem_wr`, replacing a bubble between pending fills. It adds neither storage nor ports. Its safe parallel delivery to one context is still bounded by that context's single port and credits.
- **Q8** sets `L2_CRSQ_SIZE=8`. The queue's rounded physical capacity goes from two to sixteen per bank. That adds 14 KiB of data storage across the 16 banks, plus tags/metadata and insertion logic, and retains both model ports. The part's queue depth is unknown.
- **Expect** skips the core-wide LSU drain for tagged expect-transaction events. It keeps the order of SFU acceptance and barrier arrival. The RTL change would select on the existing transaction-expect decode, with no additional queues or ports. It needs a check for callers depending on the old implicit fence. **Fetch8** raises both outstanding fetches and instruction buffer slots from four to eight, adding 256 slots at 64 warps, and retains branch resolution. **Short loop** is a software control at the same depth and data correctness check. Assembly and disassembly are available for the full and specialized loops.
- **Read overlap** reuses the existing SRAM read port for ordinary LSU reads to a disjoint row while DMA writes. Its probe gates reads with the held row requests, prioritizes the existing TCU row read, and keeps stores and atomics in the ordinary arbitration. It adds row comparisons and port arbitration. The one-128 B write-row budget stays shared by DMA and LSU stores. On the part, the measured combined load and TMA traffic is above 128 B/clock (0.9767 × 128 + 76.08 ≈ 201), while stores plus TMA contend. Physical read-port sharing is inferred. [H200 mixed shared-memory rates, M6](../results_gpu/h200_smr_summary.txt)

In isolation the fast capture path reduced throughput on A and C. A larger response queue helped B, lost rate on C, and changed A's result from a gain on baseline to a loss with LPC. On A, Pair reduced worker write grants from 196,608 to 184,641 for the same 196,608 source lines. This counter counts one grant for a pair. The isolated slope stayed at 0.382 lines/clock. Fetch8 gained 1.10 B/clock on plain loads and lost 7.57 B/clock on C. The shorter ring raised all three multi-transfer rates. It still relies on the rate at which the issuing warps reuse slots. [Pair run](../results/tma8a56_pair_a/run.log)

Read overlap raises all three multi-transfer rates at baseline and with LPC. It leaves the isolated-copy timings, decode and plain loads unchanged at the displayed precision for each reference configuration. GEMM's weight rate rises from 16.19 to 18.74 B/clock with read overlap alone. The four-way LPC case gains little from read overlap because transfer contexts stay occupied through their tails.

## C. Recommendation

No tested combination meets both gates. The acceptance rates are **96.3 B/clock** for several outstanding transfers (90% of 107 B/clock) and **0.835 lines/clock** for a lone transfer. Using the full measured H200 range gives a multi-transfer threshold of 94.5–97.2 B/clock. The two-worker stack with read overlap reaches 96.43 B/clock on B, 94.27 on A and 75.69 on C. Its single-copy slope is 0.726 lines/clock. It clears the nominal multi-transfer threshold on B and needs about 277 fewer clocks across the 32→128 KiB latency difference to meet the single gate. Four-way splitting with LPC and read overlap clears the single gate at 1.245 lines/clock and reaches at most 69.92 B/clock on A/B/C.

C exposes the cost of reserving multiple workers per copy. The baseline averages 10.82 outstanding transfers. The two-worker stack can run only eight at once and spends 1,969 clocks on average waiting to start a descriptor. Its tail-only request slots cost 49.77 B/clock, with zero full-queue bank-clocks where an input was also waiting. C reaches 87.60 B/clock with LPC and read overlap when each transfer can use a separate worker. [C with the two-worker stack](../results/tma8a56_stackrw_c/run.log), [C with LPC and read overlap](../results/tma8a56_lpcrw_c/run.log)

For the next prototype, make the two existing line paths share work at line granularity while retaining the model's capacity for 16 live transfer contexts. Allow a lone context to feed both paths and use available credits across workers. Return the issue bandwidth of a context with only replies outstanding to the ready contexts. Keep the existing 128 B write-row limit and the one-line/cycle capture capability per participating lane. The split experiments establish that the service rate benefits from parallel lines and that fixed group reservations lose throughput. Shared line scheduling across live contexts addresses both effects and reuses the existing line paths. Start with shared scheduling and LPC, exercising short and long transfers. Add disjoint-read overlap to restore producer availability where the mixed read/write counters show it binds. Use the short ring as a controlled traffic source and the original ring for acceptance. Measure the expect bypass separately. Keep the queue at baseline depth for the first test and add Q8 only if full-queue stalls still bind and the benefit holds across shapes.

The RTL changes to evaluate in stage 2 are grouped by module below.

- `VX_dxa_dispatch`, `VX_dxa_core` and `VX_dxa_worker_req_if` would allocate a live descriptor once and distribute line work from ready contexts across the existing port routes. Keep fair service for short descriptors and preserve backpressure from both paths. Context capacity would stay independent of the number of lanes serving any one descriptor.
- `VX_dxa_setup`, `VX_dxa_addr_gen` and `VX_dxa_worker` would carry the descriptor identity and a disjoint range or line index. Setup and edge/padding coordinates need to be consistent for all lines of one transfer. Reusing the current worker setup, line buffers and tags is the initial area assumption.
- `VX_dxa_gmem_req` would return each reply to its owner and release its original tag once. Measure whether distributing a single descriptor across the existing worker credit pools removes the 256-line per-worker bottleneck. In the two-worker stack the 32 KiB case has zero requests blocked for lack of worker slots, while the 128 KiB case has 22,457 blocked worker issue attempts. Test spreading a long transfer across more of the existing per-worker slots before changing total storage. [128 KiB counters](../results/tma8a56_stackrw_s128/run.log)
- `VX_dxa_smem_wr`, `VX_dxa_lmem_arb` and `VX_lmem_unit` would preserve the one-row write budget and bank, DMA/store and read arbitration. Start with the existing half-row placement from two lanes, with at most one write per bank per cycle. The row-pair experiment gives little reason to add a second pending line to every worker. If read overlap is selected, gate it on no row conflict and preserve TCU read service.
- `VX_dxa_completion` would join lane completions into one transaction event after the final destination write. `VX_wctl_unit` would change only if the transaction-expect drain can be narrowed without weakening required memory or barrier ordering.

Open measurements include both target rates with dynamic scheduling, reuse of tags with asymmetric reply timing, and the worst-case mixture of DMA writes, stores, atomics, ordinary reads and TCU reads. An RTL implementation needs a matching SimX cycle model, completion and ordering checks for the swizzled/zero-fill cases, mixed LSU tests, model parity, and Beacon timing and area on the affected units. The physical H200 credit pools, lane arbitration and buffering are unknown. This stage measures the probes in SimX and leaves RTL structure and its costs to phase 2.

The exact flags, job IDs and measurements are in the sweep manifests and per-point logs. [ICE matrix](../slurm/tma/matrix.tsv), [feed controls](../slurm/tma/controls.tsv), [read overlap](../slurm/tma/coissue.tsv), [ICE jobs](../slurm/tma/jobs.tsv), [Phoenix jobs](../slurm/tma/jobs_phoenix.tsv), [collector](../slurm/tma/collect.py), [counter analysis](../slurm/tma/analyze_stats.py), [warp trace analysis](../slurm/tma/warp_trace.py), [experiment patch](../slurm/tma/experiments.patch), [read-overlap patch](../slurm/tma/lmem_coissue.patch), [example run](../results/tma8a56_stackrw_b/run.log).

## 2026-11-24 14:45 EST · Stage 2a

The `feature/lmem-dma-rd` worktree starts at `00186b8b3`. The implementation adds the default-off `VX_CFG_LMEM_DMA_RD_OVERLAP` knob. The enabled path stages a DMA beat for the cycle when an LSU pass reaches the banks. Plain load passes compare their selected rows against each enabled part of the staged and incoming beats. LSU stores and atomics keep exclusive use of the write port. A waiting TCU row read keeps priority. DXA completion follows the final SRAM write.

ICE reference jobs 7290936, 7290937 and 7290939 use the pinned base. The new RV64 parity case exercises two DXA half rows, ordinary same-row and disjoint loads, LSU stores and atomics, and TCU reads. The case catalog and both SimX boundary checks pass. Runtime and timing verification are in progress.

## 2026-11-24 14:56 EST · Directed SRAM timing

Assertion-enabled Verilator 5.050 unit run. A single beat wrote row 11 on the lower bank half and row 19 on the upper half. At t=121 the row-7 load used the read port in the same clock and returned `0x7700`. A row-11 load targeting an unwritten bank held and then returned `0x1100`. A row-19 load held and then returned the newly written `0x3333`. A pending disjoint TCU read received the read port before the LSU while the DMA wrote. [Unit log](/storage/scratch1/7/echung67/tma-overlap-00186/lmem-lint.592WHI/lmem_overlap_tb.log)

Independent R3 review identified a TCU-only configuration that would have gained the pipeline without a DXA writer. Both models now require DXA before enabling the stage. SimX accounts for the completion FIFO and WCTL register after the SRAM write. SimX still abstracts contention at the software barrier consumer, which is outside the LMEM pass arbitration measured here.

The off control produced the same response values and held the disjoint LSU read until after the SRAM write. [Off and on unit fixtures](../results/lmem_dma_rd_unit)

## 2026-11-24 15:00 EST · First integration results

The RV64 `model_parity-lmem-dma-rd-overlap` app passed with 4,927 retired instructions on each model. SimX ran in 17,759 cycles and rtlsim ran in 17,774 cycles, a 0.084% gap. [SimX log](../results/lmd00186_v2_direct_sx_directed/run.log) · [rtlsim log](../results/lmd00186_v2_small_on_rtl_directed/run.log)

The initial two-core Beacon run kept the 26-level maximum and the mean of the deepest 50 paths at 24.6 levels. Enabling overlap changed LUTs 819,310 → 820,267 and FFs 625,724 → 628,084, with unchanged BRAM and DSP counts. The DMA-to-LMEM-engine path grew from 11 to 16 levels. The next layout factors the row reductions and bank-conflict arbitration before the only full-size rtlsim run. [Initial timing and area diff](../results/lmd00186_beacon/off_on_diff.txt)

## 2026-11-24 15:08 EST · Factored bank arbitration

The final RTL compares each DMA part's address with the winning lanes in parallel with the pairwise LSU bank checks. The block grants and the next round-robin position were exhaustively compared with the original occupied-bank algorithm for two, three and four blocks. Knob-off and knob-on SRAM traces matched the initial implementation byte for byte. [Arbitration equivalence](../results/lmem_dma_rd_unit/arb_equivalence.log) · [SRAM traces](../results/lmem_dma_rd_unit)

The two-core Beacon comparison keeps the 26-level maximum, 24.6-level mean of the deepest 50 paths and 239.8 MHz model advisory. The paths from the DXA arbiter through row admission to `s1_last` and `credits` measure 11 → 14 levels. The report flags 28 endpoints, down from 86 in the first implementation. LUTs measure 819,310 → 820,225 (+0.112%) and FFs 625,724 → 628,084 (+0.377%). BRAM, LUTRAM and DSP usage are unchanged. The row checks feed the existing pass arbitration in the same cycle. This comparison uses a custom two-core configuration. [Final timing and area diff](../results/lmd00186_beacon_v3_exact/off_on_diff.txt)

The tested SimX path compiles with DXA disabled after guarding its DXA-only request fields. The CI catalog reports 1,116 valid cases and the testcase and change-impact unit suites pass all 137 tests.

## 2026-11-24 15:09 EST · One-SM h200s SimX

The default-off build matches `00186b8b3` on all 14 requested points and the two extra stream shapes. The enabled runs retain the same retired instruction count. All points passed. The v2 SimX matrix predates the DXA-disabled request-field guard. DXA is enabled on each h200s point, so that guard leaves these results unchanged.

| Workload | Retired instructions | Base cycles | Overlap off | Overlap on | Overlap + LPC |
|---|---:|---:|---:|---:|---:|
| `fetch_squash` | 978 | 10,017 | 10,017 | 10,017 | — |
| `warp_rate` | 8,511 | 113,380 | 113,380 | 113,380 | — |
| `tmem_roof -n512 -i8` | 8,925 | 61,878 | 61,878 | 61,878 | — |
| `bwstream -n2048 -u4 -b32` | 464,896 | 394,065 | 394,065 | 394,065 | — |
| `gemm_block -m16 -n1024 -k4096 -s1` | 814,144 | 513,734 | 513,734 | 442,182 | 439,399 |
| `gemm_tmem_decode -m16 -n8192 -k4096` | 211,722 | 1,151,803 | 1,151,803 | 1,152,194 | — |
| `war_order` | 341,824 | 320,317 | 320,317 | 320,317 | — |
| `ldstream -t2048 -b8 -u8 -m16 -a -l` | 250,196 | 173,098 | 173,098 | 173,098 | — |
| `bwstream_dxa -e4096 -n256 -d2 -c0` | 70,546 | 400,310 | 400,310 | 359,797 | 326,102 |
| `bwstream_dxa -e4096 -n256 -d4 -c0` | 30,240 | 190,417 | 190,417 | 175,700 | 149,087 |
| `smem_rate -w64 -i16 -k14 -f150` | 146,568 | 139,069 | 139,069 | 139,069 | — |
| `smem_rate -w64 -i16 -k13 -f150` | 146,440 | 139,576 | 139,576 | 139,576 | — |
| `smem_rate -w64 -i16 -k22 -f150` | 147,672 | 139,931 | 139,931 | 139,931 | — |
| `lmem_access` | 1,428,002 | 7,713,082 | 7,713,082 | 7,713,082 | — |
| `bwstream_dxa -e2048 -n512 -d4 -c0` | 140,448 | 395,248 | 395,248 | 362,359 | 332,724 |
| `bwstream_dxa -e4096 -n32 -d2 -c0` | 9,394 | 54,072 | 54,072 | 47,258 | 42,938 |

The `-d2` / `-d4` 256-tile streams measure 73.34 / 66.08 B/clock before, 81.60 / 71.62 with overlap, and 90.03 / 84.40 with overlap and LPC. The third shape (`-e2048 -n512 -d4`) measures 74.28 → 81.02 → 88.24 B/clock. GEMM weight bandwidth, using 8,388,608 weight bytes over the full kernel, measures 16.33 → 18.97 → 19.09 B/clock. Decode multiply rate is 58.94 → 58.92 B/clock/SM.

[Suite and compiler flags](../slurm/lmem-dma-rd/suite.sbatch) · [ICE job manifest](../slurm/lmem-dma-rd/ice_jobs.tsv) · [Phoenix job manifest](../slurm/lmem-dma-rd/phoenix_jobs.tsv)

## 2026-11-24 15:28 EST · RV64 parity and the h200s selection

The final implementation is `36f0e32ab`. A baseline source snapshot with only the new application files, plus the committed implementation with overlap off, produced the same result on each simulator for the directed test. The `dxa_rowmix` parity row here enables the two LSU blocks, `LMEM_RD_ROW` and distinct DMA half-row addresses so it exercises the new arbitration. The original rowmix CI configurations were also run with overlap off. `lmem_rw_order` checks LSU ordering with the knob set in a configuration without a DXA writer. The new directed case exercises stores and atomics with a live DMA writer.

| Overlap-enabled case | Instructions on each model | SimX cycles | rtlsim cycles | Absolute cycle gap relative to rtlsim |
|---|---:|---:|---:|---:|
| `lmem_dma_rd` | 4,927 | 17,759 | 17,774 | 0.084% |
| `dxa_rowmix` | 644,224 | 1,559,339 | 1,558,640 | 0.045% |
| `dxa_corun` | 10,760 | 55,452 | 55,473 | 0.038% |
| `lmem_rw_order` | 952 | 1,516 | 1,520 | 0.263% |

With overlap off, the directed test matches `00186b8b3` at 4,927 instructions and 17,762 SimX / 17,775 rtlsim cycles. The two-block rowmix case matches the baseline at 644,224 instructions and 1,559,531 / 1,558,906 cycles, and the stock rowmix case matches at 644,224 instructions and 1,553,913 / 1,553,203 cycles. The stock corun and ordering workloads also match the baseline on each simulator. [Directed baseline SimX](../results/lmd00186_base_directed_sx_directed/run.log) · [Directed baseline rtlsim](../results/lmd00186_base_directed_rtl_directed/run.log) · [Final directed rtlsim](../results/lmd00186_v3_small_on_rtl_directed/run.log) · [Final rowmix rtlsim](../results/lmd00186_v3_small_on_rtl_rowmix_rd/run.log) · [Final corun rtlsim](../results/lmd00186_v3_small_on_rtl_corun/run.log) · [Final ordering rtlsim](../results/lmd00186_v3_small_on_rtl_rw_order/run.log)

Eight of eight small workload configurations are cycle- and instruction-identical to the baseline on each simulator with overlap off. The sweep includes both the functional and unsplit baseline rowmix geometries and the atomic and no-atomic ordering cases. The three original rowmix SimX comparisons use v2, whose enabled-DXA timing code is identical to v3. [Job and point manifest](../slurm/lmem-dma-rd/phoenix_jobs.tsv)

With overlap already on, enabling LPC raises 32-tile copy bandwidth in the reduced two-channel HBM2 RTL configuration from 37.26 to 37.74 B/clock (+1.27%), with 10,680 retired instructions in both cases and 112,559 → 111,150 cycles. SimX in that memory-limited probe measures 37.99 → 37.94 B/clock. SimX in the reduced 64-warp HBM3E configuration measures 85.84 → 90.51 B/clock on 32 tiles and 89.37 → 94.90 on 256 tiles. The HBM2 RTL result and the one-SM h200s SimX gains were available when selecting overlap plus LPC for the one full RTL build. [Small RTL LPC off](../results/lmd00186_v2_small_on_rtl_dxa_small/run.log) · [Small RTL LPC on](../results/lmd00186_v2_small_on_rtl_dxa_small_lpc/run.log) · [Full build submission](../slurm/lmem-dma-rd/large_job_scontrol.txt)

## 2026-11-24 15:45 EST · LPC on reduced HBM3E RTL

The reduced model retains HBM3E timing, 64 warps, 32 threads, 16 DXA workers and the 228 KiB LMEM. It uses a smaller L2 and in-flight limits and omits TCU and VM. For each model, the only change in each pair is LPC. The longer run reuses the 32-tile run's RTL artifact for each setting.

| Tiles | SimX cycles, LPC off → on | SimX B/clock | rtlsim cycles, LPC off → on | rtlsim B/clock | rtlsim bandwidth gain |
|---:|---:|---:|---:|---:|---:|
| 32 | 42,755 → 40,548 | 85.84 → 90.51 | 46,988 → 43,659 | 78.11 → 84.06 | 7.63% |
| 256 | 328,510 → 309,392 | 89.37 → 94.90 | 348,093 → 324,521 | 84.35 → 90.47 | 7.26% |

Retired instructions match across all four runs for each size, at 9,345 and 70,497. In the 32-tile RTL pair, average DXA global-memory latency falls from 2,306.2 to 2,106.1 clocks and no-slot stalls from 204,979 to 169,259. This isolated LPC comparison supports enabling both knobs in h200s. [n32 LPC off](../results/lmd00186_v3_hbm_lite_lpc0_rtl_dxa32/run.log) · [n32 LPC on](../results/lmd00186_v3_hbm_lite_lpc1_rtl_dxa32/run.log) · [n256 LPC off](../results/lmd00186_v3_hbm_lite_lpc0_rtl_b/run.log) · [n256 LPC on](../results/lmd00186_v3_hbm_lite_lpc1_rtl_b/run.log)

## 2026-11-24 20:24 EST · Stage 2a final result

`feature/lmem-dma-rd` ends at `ae692191d` (`ae692191de1dcf19e6ea207416e723c66f8cb74e`). `36f0e32ab` implements the default-off knob in RTL and SimX and adds the RV64 parity test. The last commit enables overlap and LPC in `profiles/h200s.json` only. All 312 resolved definitions for the tested one-SM profile match the frozen source's explicit overrides.

Both models admit ordinary LSU load passes on the existing read port during DXA writes when every selected bank row is disjoint from the byte-enabled halves of the staged and incoming beats. The two halves can carry separate row addresses. A conflicting load waits. TCU reads keep read-port priority, LSU stores and atomics still share write arbitration with DXA, and the last DMA beat completes after its SRAM write. The unit traces verify the concurrent read, deferred same-row loads, data visibility and TCU priority. With the knob off, the 14 requested h200s SimX workloads and two additional stream shapes match `00186b8b3` to the cycle. Eight small configurations match on both models. With overlap on, the four small parity cases retire identical instruction counts and stay within 0.263% of RTL cycles. [Unit traces](../results/lmem_dma_rd_unit) · [Job manifest](../slurm/lmem-dma-rd/phoenix_jobs.tsv)

Phoenix `16097444` completed in 4:53:26 under `gts-hkim358`, with 16 CPUs and 160 GB. All three runs passed using the same `librtlsim.so` on Phoenix scratch. The file's inode, size and modification time were unchanged across the suite. Its build tree is `/storage/scratch1/7/echung67/tma-overlap-00186/h200s_rtlsim_16097444`. The full build and runs used 78.25 allocated CPU-hours. Including the other Phoenix probes gives 98.20 CPU-hours, about 0.79 at the quoted rate. [Build and artifact metadata](../results/lmd00186_large_v3_lpc1/meta.txt) · [Slurm log](../slurm/lmem-dma-rd/phoenix_16097444.out)

| One-SM h200s workload | Retired on each model | SimX baseline cycles | SimX overlap + LPC cycles | rtlsim overlap + LPC cycles | RTL cycles vs selected SimX |
|---|---:|---:|---:|---:|---:|
| `bwstream_dxa -e4096 -n32 -d2 -c0` | 9,394 | 54,072 | 42,938 | 48,702 | +13.42% |
| `smem_rate -k22 -w64 -i16 -f150` | 147,672 | 139,931 | 139,931 | 140,238 | +0.22% |
| `gemm_block -m16 -n1024 -k4096 -s1` (`MTILES=1 PROFILE_ENABLE`) | 814,144 | 513,734 | 439,399 | 472,826 | +7.61% |

| Rate or mix | SimX baseline | SimX overlap only | SimX overlap + LPC | rtlsim overlap + LPC |
|---|---:|---:|---:|---:|
| 32-tile DXA, B/clock | 67.87 | 77.66 | 85.47 | 75.36 |
| GEMM weights, B/clock | 16.33 | 18.97 | 19.09 | 17.74 |
| k22 store/load warp clocks per iteration | 8,321.75 / 8,318.56 | 8,321.75 / 8,318.56 | 8,321.75 / 8,318.56 | 8,320.50 / 8,320.56 |

The full RTL runs use the selected overlap-plus-LPC configuration. The before/after h200s comparison is from SimX, and the same-model RTL LPC comparison is from the reduced HBM3E pair. The +13.42% DXA and +7.61% GEMM cycle differences remain in the full model. Both DXA models report 224 transfers and 57,344 lines written. RTL reports zero no-slot stalls and lower mean global-memory latency than SimX (1,703.8 vs 2,059.1 cycles), so that latency counter alone cannot explain the full gap. The two-core Beacon maximum is 26 levels in both builds. The local DMA admission path grows from 11 to 14 levels, with +0.112% LUTs and +0.377% FFs. [DXA](../results/lmd00186_large_v3_lpc1_dxa32/run.log) · [Shared-memory mix](../results/lmd00186_large_v3_lpc1_k22/run.log) · [GEMM](../results/lmd00186_large_v3_lpc1_gemm/run.log) · [Beacon report](../results/lmd00186_beacon_v3_exact/off_on_diff.txt)

A/B/C reach 84.40 / 90.03 / 88.24 B/clock with both features in h200s SimX, below the 96.3 B/clock gate. Stage 1 measured a 0.377 lines/clock isolated-copy slope with LPC at baseline worker allocation, below the 0.835 target. The remaining bottleneck candidates are feeding both existing line paths from one ready transfer and reusing issue capacity when other transfers wait for their final replies. The fixed worker splits improved the isolated slope and reduced live context capacity, which hurt the 8 KiB stream. The stage 1 counters support measuring line issue, worker credits and tail occupancy while retaining 16 live contexts. [LPC counters](../results/tma8a56_lpc_a/run.log) · [Two-worker 128 KiB counters](../results/tma8a56_stackrw_s128/run.log) · [8 KiB fixed-split counters](../results/tma8a56_stackrw_c/run.log)

## 2026-11-24 21:20 EST · LMEM and DXA parity follow-up

The diagnostic branch `fix/lmem-dma-rd-parity` starts at `cece299d3`, which includes the default-off overlap knob via `c4362ef63`. The h200s profile leaves overlap and LPC disabled. Experiments set them explicitly. The first paired matrix freezes that source and runs all four settings on a one-SM HBM3E configuration that retains 64 warps, 32 threads, 16 DXA workers, LMEM row reads and the 94-cycle L2 link. It omits TCU and VM and uses smaller L2 and in-flight limits. The 8-warp variant changes only the hardware warp count. Each model and knob setting reuses one simulator across tile counts. [Phoenix jobs](../slurm/lmem-dma-rd-parity/phoenix_jobs.tsv)

Source inspection found that an RTL GMEM port holds its response until the selected worker's capture register accepts it. SimX can move that response into a worker queue and advance the shared port while the worker waits. The previous LMEM comparison used workers' `avg_gmem_lat` counters, which have different endpoints in the two models. The new probes record shared port response handshakes, worker captures, write grants, SRAM writes, LSU bank accesses and TCU row reads. The first experiment tests how much shared-port backpressure contributes to the gap. [Trace instrumentation](../slurm/lmem-dma-rd-parity/instrument_trace.py) · [Trace comparison](../slurm/lmem-dma-rd-parity/compare_trace.py)

## 2026-11-24 21:33 EST · Eight-warp reproduction and the first response overtake

With both knobs enabled, reducing 64 hardware warps to eight leaves the seven resident CTAs and the cycle counts at 40,548 SimX / 43,659 rtlsim across 9,345 instructions. The gap is 7.13% relative to RTL, above the CI default 5% tolerance. With the bank DRAM backend on the same simulator builds, the counts are 40,886 / 43,811 and the gap is 6.68%. Neither the 64-warp scheduler, TMEM, VM, nor the Ramulator backend is required to reproduce the discrepancy. [Eight-warp RTL](../results/lmpar_cece299d3_hbm_w8_11_rtlsim_n32/run.log) · [Bank RTL](../results/lmpar_cece299d3_hbm_w8_11_rtlsim_n32bank/run.log)

The 16-tile instrumented pair matches 28,672 individual DXA lines by warp, tile, destination and global address. Each retires 4,977 instructions, with 22,986 / 23,805 cycles. The instrumented HBM3E-lite RTL build took 424 seconds at 24 threads on Rover. On RTL, all 28,672 accepted port responses are captured by their selected worker on the same clock. Port 0 holds the same tag at clocks 3612–3613 while worker 0 cannot capture slot 66, then delivers the next tag at 3614. Both port heads together hold for 2,633 port-cycles.

On the first SimX port-0 overtake, worker 2 slot 66 enters at 3669, remains blocked at its worker at 3672–3673, and captures at 3674. The next response, worker 0 slot 37, enters at 3670 and captures at 3671. Port 1 shows the same mechanism. An accepted SimX port response takes up to 18 clocks to reach worker capture. This observation describes the handshakes within each model. CTA dispatch order and line arrival order differ across the two runs, so pairing samples by absolute time would misidentify the first actual violation. There are 416 SimX and 435 RTL cycles with both DMA writes and LSU accesses at the banks. The pure copy has no TCU row requests. [Matched trace report](../results/lmpar_tracev1_hbm_11_rtl_n16/comparison.txt) · [RTL trace](../results/lmpar_tracev1_hbm_11_rtl_n16/run.log) · [SimX trace](../results/lmpar_tracev1_hbm_11_sx_n16/run.log)

The single full h200s RTL model started building on Rover as session `942497` with 32 threads inside `vortex.slice` (200 GiB cap). Its two-tile smoke seeds the reusable model. The RTL and configuration are byte-identical to the base while the candidate changes the SimX DXA response path. [Rover launch record](../slurm/lmem-dma-rd-parity/rover_full_jobs.tsv)

## 2026-11-24 21:43 EST · Shared GMEM response backpressure and RV64 regression

Commit `7966e9661` makes SimX keep each DXA GMEM response at its port head until the addressed worker captures it. The request arbiter retains its prior arbitration and delay. Workers can still accept an out-of-bounds line when their port is presenting a response for another worker. The eight-warp regression `dxa:model_parity-bwstream-dxa-shared-response:rtlsim` runs at RV64 with the default 5% threshold. Running the CI checker on the three measured logs reproduces the pre-fix cycle failure (40,548 vs. 43,659, 7.13%) and the fixed pass (45,317 vs. 43,659, 3.80%). Each retired 9,345 instructions and recorded 224 transfers and 57,344 GMEM reads and LMEM writes. Pytest discovers the case at RV64, and catalog lint passes. [Checker output](../slurm/lmem-dma-rd-parity/model_parity_replay.txt) · [Fixed copy](../results/lmpar_capture_v1_hbm_w8_11_sx_n32/run.log)

Using the same 16-tile instrumented binaries, one CTA retires 711 instructions at 18,487 SimX / 18,482 RTL cycles. Four CTAs retire 2,844 instructions at 20,382 / 20,054 cycles. Seven CTAs retire 4,977 instructions at 22,986 / 23,805 cycles. The increasing pressure on the two shared response ports changes both the gap and its direction. [One CTA SimX](../results/lmpar_tracev1_hbm_11_sx_g1/run.log) · [One CTA RTL](../results/lmpar_tracev1_hbm_11_rtl_g1/run.log) · [Four CTAs SimX](../results/lmpar_tracev1_hbm_11_sx_g4/run.log) · [Four CTAs RTL](../results/lmpar_tracev1_hbm_11_rtl_g4/run.log)

The directed LSU/DXA/TCU trace on both models passed at 4,927 instructions and 17,759 / 17,774 cycles, a 0.084% gap. Each model grants 22 simultaneous DMA writes and disjoint LSU reads at the banks. SimX and RTL record 18 and 19 disjoint TCU row reads during DMA writes, respectively, out of 416 TCU row reads each. RTL clock 3672 pairs a half-0 DMA write to row 3 with an LSU request to bank 0 row 12. At 6990 a TCU read waits for the same row as a DMA write and is granted at 6991. At 5081 a pending TCU row read takes priority over a DMA-disjoint LSU request. No conflicting grants were recorded. [Directed bank trace summary](../results/lmpar_row_trace_v1/row_trace_summary.txt)

All three fixed h200s SimX workloads passed and reproduced the previous instruction counts. DXA moved from 42,938 to 45,248 cycles, the load/store mix stayed at 139,931, and GEMM moved from 439,399 to 443,681. Against the Stage 2a RTL reference, the corresponding cycle gaps are 7.09%, 0.22%, and 6.16% of RTL. The current-head RTL suite will verify those reference points. The resolved profile comparison identifies DXA's 256 vs. 128 line slots, larger L2 and LSU queues, VM, and tensor-related settings among the remaining differences from HBM3E-lite. The 256-slot probe holds the rest of the eight-warp configuration fixed and compares both original models and the SimX response correction. [Fixed h200s DXA](../results/lmpar_capture_v1_h200s_11_simx_n32/run.log) · [Fixed h200s load/store](../results/lmpar_capture_v1_h200s_11_simx_k22/run.log) · [Fixed h200s GEMM](../results/lmpar_capture_v1_h200s_11_simx_gemm/run.log) · [Resolved configurations](../slurm/lmem-dma-rd-parity/resolved_hbm_lite_to_h200s.txt)

## 2026-11-24 21:50 EST · Separate overlap and LPC effects

The frozen pre-fix 64-warp HBM3E-lite runs all retire 9,345 instructions on the 32-tile copy. The signed cycle delta is `(RTL - SimX) / RTL`.

| Overlap | LPC | SimX cycles | RTL cycles | RTL minus SimX |
|---:|---:|---:|---:|---:|
| 0 | 0 | 53,580 | 54,526 | +1.73% |
| 0 | 1 | 54,243 | 56,915 | +4.69% |
| 1 | 0 | 42,755 | 46,988 | +9.01% |
| 1 | 1 | 40,548 | 43,659 | +7.13% |

In the 32-tile case, overlap alone exposes the larger timing discrepancy. LPC alone adds cycles on both models. With overlap already enabled, LPC reduces cycles on both models and narrows this particular gap. The absolute gap at 16 tiles can differ in sign and size, so the 32-tile case gates the sustained transfer behavior. [Neither knob](../results/lmpar_cece299d3_hbm_lite_00_rtlsim_n32/run.log) · [LPC only](../results/lmpar_cece299d3_hbm_lite_01_rtlsim_n32/run.log) · [Overlap only](../results/lmpar_cece299d3_hbm_lite_10_rtlsim_n32/run.log) · [Both knobs](../results/lmpar_cece299d3_hbm_lite_11_rtlsim_n32/run.log)

At eight warps with the same two GMEM ports, using four DXA workers gives 91,947 SimX and 92,796 RTL cycles (0.92% gap, 9,345 retired). Enabling the TCU/TMEM configuration in the copy's SimX build leaves it at 40,548 cycles with 16 workers. The copy only issues ordinary LSU loads after the DMA barrier. The separate directed trace exercises actual TCU bank access. [Four-worker RTL](../results/lmpar_cece299d3_hbm_w8_dxa4_11_rtlsim_n32/run.log) · [TCU and TMEM enabled](../results/lmpar_cece299d3_hbm_w8_tmem_11_simx_n32/run.log)

Replaying SimX's row-major LPC worker timing against the six original one-, four- and seven-CTA traces reproduces the RTL's 49,152 accepts and 3,378 stalls. It matches all 98,304 worker write grants across both models. Every accepted worker write appears in SRAM two cycles later. This rules out the row-major capture-to-write scheduling as an independent difference for those observed inputs. For the seven-CTA pre-fix pair, the different response schedules yield 9,955 paired DMA write cycles in SimX and 8,368 in RTL. [Worker replay](../slurm/lmem-dma-rd-parity/worker_trace_replay.txt) · [Replay source](../slurm/lmem-dma-rd-parity/replay_worker_trace.py)

With both knobs off, the response correction also changes h200s SimX from 54,072 to 55,624 cycles on the 32-tile copy and from 513,734 to 521,456 on GEMM, with the same retired instructions. Responses can overtake one another with either knob setting. The correction removes more cycle optimism at the chosen `11` point of the copy (2,310 cycles) than at `00` (1,552 cycles). [Fixed off copy](../results/lmpar_capture_v1_h200s_00_simx_n32/run.log) · [Fixed off GEMM](../results/lmpar_capture_v1_h200s_00_simx_gemm/run.log)

At 128 tiles and 35,553 retired instructions, both-knobs-on HBM3E-lite gives 155,671 old SimX, 163,037 SimX with shared response backpressure and 163,121 RTL cycles. The cycle gap falls from 4.57% to 0.052% as startup amortizes. The 16-tile result moves from 22,986 to 23,824 SimX versus 23,805 RTL, a 0.080% fixed gap. [Corrected 128-tile SimX probe](../results/lmpar_sharedrsp_hbm_lite_11_simx_n128/run.log) · [128-tile RTL](../results/lmpar_cece299d3_hbm_lite_11_rtlsim_n128/run.log)

A SimX-only h200s diagnostic restricts descriptor dequeue to the RTL fetch cadence of one descriptor per two cycles. It keeps worker setup and eligibility unchanged. With shared response backpressure already applied, this changes the 32-tile copy from 45,248 to 45,379 cycles and GEMM from 443,681 to 438,325. The copy shift contributes 131 cycles against a 3,454-cycle difference from the previous RTL reference, and GEMM moves further from that reference. The diagnostic stays outside the branch. [Descriptor probe diff](../slurm/lmem-dma-rd-parity/desc_fetch_probe.diff) · [Copy](../results/lmpar_desc_probe_h200s_11_simx_n32/run.log) · [GEMM](../results/lmpar_desc_probe_h200s_11_simx_gemm/run.log)

The complete 16/32/128-tile ablation preserves instruction counts in all 12 pre-fix pairs. At 128 tiles the gaps are 0.060% for `00`, 5.347% for LPC-only, 8.051% for overlap-only and 4.567% for `11`. LPC-only can cross 5% after startup amortizes. [Full ablation](../slurm/lmem-dma-rd-parity/reduced_ablation.tsv)

Enabling VM alone changes the copy's retired count from 9,345 to 9,394. In the eight-warp VM variant, applying the shared response correction moves SimX from 41,783 to 43,268 cycles. The no-VM leg moves from 40,548 to 45,317. This changes the size of the correction, and a reduced VM RTL run is needed to measure the remaining paired gap. On full h200s, an independent SimX probe removes the extra request-arbiter delay before the VM request register. It moves the copy by 28 cycles and GEMM by 591 cycles. [Reduced VM SimX](../results/lmpar_sharedrsp_hbm_w8_vm_11_simx_n32/run.log) · [VM request probe](../slurm/lmem-dma-rd-parity/vm_req_probe.diff) · [Probe copy](../results/lmpar_vm_req_probe_h200s_11_simx_n32/run.log) · [Probe GEMM](../results/lmpar_vm_req_probe_h200s_11_simx_gemm/run.log)

## 2026-11-24 22:02 EST · Response trace after the fix

On the same 16-tile HBM3E-lite copy, a trace build of `7966e9661` reproduces the fixed 23,824-cycle result. Each SimX port presents the same tag through every stall and accepts responses in its recorded arrival order. All 28,672 accepted responses are captured by the addressed worker on the acceptance cycle, as in RTL. The longest hold is six cycles in SimX and five in RTL. SimX records 1,226 and 1,993 hold cycles on the two ports, versus 1,188 and 1,445 in RTL. The ordering fix leaves different response arrival patterns across the models even when overall cycles are close. [Fixed trace comparison](../results/lmpar_capture_trace_7966e9661_11_simx_n16/comparison_to_archived_rtl.txt) · [Fixed FIFO check](../results/lmpar_capture_trace_7966e9661_11_simx_n16/fifo_summary.txt)

The same correction's effect depends on the capture mode. At 32 tiles, the shared-response SimX prototype gives 57,255 cycles at `00`, 55,207 at `01`, 52,766 at `10`, and 45,317 at `11`. Relative to RTL, SimX is 5.005% slower at `00`, 3.00% faster at `01`, 12.30% slower at `10`, and 3.80% slower at `11`. The 16-tile trace with both knobs off confirms 29,900 fixed SimX / 26,506 RTL cycles and 7,126 total fixed SimX port hold cycles. The correction enforces the RTL handshake across settings. Total cycle parity still depends on what reaches the ports and when. [Fixed off trace](../results/lmpar_capture_trace_7966e9661_00_simx_n16/fifo_summary.txt) · [Fixed overlap-only copy](../results/lmpar_sharedrsp_hbm_lite_10_simx_n32/run.log)

Turning off only `LMEM_RD_ROW` disables overlap in both models even with the raw overlap bit set. On the original eight-warp pair this gives 54,443 SimX / 57,151 RTL cycles (4.74% gap) with 9,345 retired. The earliest transfer in the fixed 16-tile trace starts from warp 0 on SimX and warp 3 on RTL. Matching 112 transfers by warp, LMEM destination and occurrence yields nine on the same worker and 49 on the same GMEM port. This start-order divergence changes the port input schedule before capture backpressure applies. [Row-read-off RTL](../results/lmpar_cece299d3_hbm_w8_row0_11_rtlsim_n32/run.log) · [Matched launch summary](../results/lmpar_capture_trace_7966e9661_11_simx_n16/input_schedule_summary.txt)

## 2026-11-24 22:08 EST · Single-factor controls and the VM residual

All rows below use the 32-tile copy with overlap and LPC enabled. Each row changes one factor from the eight-warp configuration. The sign is `(RTL - SimX) / RTL`. Both models match at 9,345 retired instructions, or 9,394 in the VM leg. The stock eight-warp RTL point built and ran in 7m54s on 16 Phoenix CPUs. Reduced RTL points used a scratch-compatible derivative of the point script under `gts-hkim358`, with source and builds on scratch. The full h200s RTL build uses Rover. [Geometry data](../slurm/lmem-dma-rd-parity/geometry.tsv) · [Phoenix submit records](../slurm/lmem-dma-rd-parity/phoenix_jobs.tsv)

| Change from eight-warp baseline | Original SimX cycles | RTL cycles | Signed gap |
|---|---:|---:|---:|
| Baseline with 16 workers, 128 slots and link 94 | 40,548 | 43,659 | +7.13% |
| DRAM bank backend | 40,886 | 43,811 | +6.68% |
| Eight DXA workers | 52,631 | 53,870 | +2.30% |
| Four DXA workers | 91,947 | 92,796 | +0.92% |
| Disable LMEM row read | 54,443 | 57,151 | +4.74% |
| Enable VM | 41,783 | 46,520 | +10.18% |
| Double L2 from 4 to 8 MiB | 41,133 | 43,517 | +5.48% |
| Increase slots from 128 to 256 | 44,309 | 42,314 | −4.71% |
| Reduce L2 link latency from 94 to 0 | 94,817 | 42,762 | −121.73% |
| Enable idle TCU and TMEM | 40,548 | — | — |

The eight-worker and four-worker legs retain two GMEM ports and show that the discrepancy grows with shared-port pressure. The zero-link leg retains 57,344 lines of work and reverses the gap. Its reported average DXA latency is 5,304.6 SimX / 2,112.4 RTL cycles versus 1,922.4 / 2,106.1 with link 94. The full profile retains link 94. [Eight-worker RTL](../results/lmpar_cece299d3_hbm_w8_dxa8_11_rtlsim_n32/run.log) · [Zero-link RTL](../results/lmpar_cece299d3_hbm_w8_link0_11_rtlsim_n32/run.log) · [Zero-link SimX](../results/lmpar_cece299d3_hbm_w8_link0_11_simx_n32/run.log)

With VM, the shared-response change leaves 43,268 SimX vs. 46,520 RTL cycles, a +6.99% signed gap. Increasing only the slot count to 256 moves the fixed copy to 45,347 vs. 42,314 cycles, a −7.17% gap. At 128 tiles, this 256-slot variant moves from 165,643 to 171,850 SimX cycles against 158,747 RTL. The correction reproduces the shared-port handshake while the overall timing residual depends on geometry. Marking only the consumer-side VM response channel as registered-ready shifts the full h200s copy and GEMM by +185 and +295 cycles. That probe leaves the separate two-entry upstream MMU response channel in place. [VM RTL](../results/lmpar_cece299d3_hbm_w8_vm_11_rtlsim_n32/run.log) · [Slot-256 RTL](../results/lmpar_cece299d3_hbm_w8_slots256_11_rtlsim_n128/run.log) · [Response-ready probe](../slurm/lmem-dma-rd-parity/vm_rsp_probe.diff)

The one full h200s RTL build took 2,420 seconds on Rover and peaked at 80 GiB for the session. The seed smoke passed at 1,204 retired and 5,997 cycles. The three required reruns started at 22:13 EST and used the same library (inode 52841541, size 201,979,016 bytes, mtime 22:11:51 EST). Each runtime make took one second. The one-CTA diagnostic queues behind the required DXA run and uses the same library. [Rover sessions](../slurm/lmem-dma-rd-parity/rover_full_jobs.tsv) · [Library identity](../slurm/lmem-dma-rd-parity/lmpar_cece299d3_h200s_11_rtlsim_model_identity.txt)

## 2026-11-24 22:17 EST · GEMM phases and a unit-MMU witness

Both archived h200s GEMM references and both fixed SimX runs use `-m16 -n1024 -k4096 -s1` and retire 814,144 instructions. With both knobs off, the earlier RTL total and maximum profiled K walk are 544,982 and 540,426 cycles, versus 521,456 and 516,734 in fixed SimX. With both knobs on, Stage 2a RTL gives 472,826 and 468,209 cycles, versus 443,681 and 437,756 in fixed SimX. The K walk contains nearly all of the total gap. It includes DXA staging, LSU traffic and MMA. The newer enabled pair reports 558,186 RTL and 558,212 SimX D-cache requests. SimX records more misses and bank stalls in that pair. The disabled RTL run uses an earlier revision and a different perf class, so the 5,619-cycle difference in the total gaps across settings does not isolate overlap or LPC. [Archived off RTL](../results/bg_cache_repl_8a56_ice_1124/gemm_block.rtlsim.stage2.log) · [Fixed off SimX](../results/lmpar_capture_v1_h200s_00_simx_gemm/run.log) · [Archived on RTL](../results/lmd00186_large_v3_lpc1_gemm/run.log) · [Fixed on SimX](../results/lmpar_capture_v1_h200s_11_simx_gemm/run.log)

The socket unit MMU has a second independent ordering discrepancy. RTL keeps a filled entry live and elects one replay head across its clients. SimX retires the entry on fill and moves requests into separate per-port queues. In a standalone SimX witness, two parked accesses share one VPN while port 0 is blocked. Legacy mode advances port 1 during the block. The shared-replay diagnostic holds it until port 0 can proceed, then drains the two requests on clocks 14 and 15. Both modes translate the same addresses with one miss request. The probe changes only normal replay for the socket unit MMU and stays outside the fix branch while the eight-warp VM and full h200s timing checks run. [Probe diff](../slurm/lmem-dma-rd-parity/vm_replay_probe.diff) · [Phoenix jobs](../slurm/lmem-dma-rd-parity/phoenix_jobs.tsv)

## 2026-11-24 22:20 EST · Fresh h200s copy and diagnostic deltas

The fresh RTL copy completed in 339 seconds and reproduced 48,702 cycles, 9,394 retired instructions, 224 transfers and 57,344 reads and writes. Original and corrected SimX give 42,938 and 45,248 cycles with the same work. Using the Stage 2a convention `(RTL - SimX) / SimX`, the difference fell from 13.42% to 7.63%. The CI denominator gives 7.09%, still above 5%. Reported average GMEM latency is 1,703.8 RTL vs. 2,218.2 fixed SimX; both report zero no-slot stalls. The counters aggregate different timing endpoints and do not identify the remaining discrepancy. [Fresh RTL copy](../results/lmpar_cece299d3_h200s_11_rtlsim_n32/run.log) · [Fixed SimX copy](../results/lmpar_capture_v1_h200s_11_simx_n32/run.log)

The shared-replay probe shifts the 32-tile copy by +26 cycles on eight-warp VM and −25 cycles on h200s. It shifts h200s GEMM by +95 cycles. Marking the worker-side arbiter inputs as registered-ready leaves both eight-warp control copies and both h200s kernels unchanged. Bypassing the extra SimX unit-MMU response stage shifts eight-warp VM by +41 cycles, h200s copy by +630 cycles and GEMM by −2,043 cycles. That bypass also changes latency and serves only as a sensitivity check. All probe workloads passed with the expected work. These tests leave the full residual substantially intact. [Shared replay copy](../results/lmpar_ummu_replay_probe_h200s_11_simx_n32/run.log) · [Worker ready probe](../slurm/lmem-dma-rd-parity/req_ready_probe.diff) · [Unit response bypass](../slurm/lmem-dma-rd-parity/unit_rsp_wire_probe.diff)

## 2026-11-24 22:26 EST · h200s concurrent copy control

These three pairs reuse the same full builds and keep tile size, depth and consume mode constant. The first two rows also hold the number of tiles per CTA fixed.

| Active CTAs | Tiles per CTA | Fixed SimX cycles | Fresh RTL cycles | RTL minus SimX | Retired in each model |
|---:|---:|---:|---:|---:|---:|
| 1 | 16 | 14,059 | 14,078 | 19 (0.13% of RTL) | 718 |
| 7 | 16 | 25,872 | 29,633 | 3,761 (12.69% of RTL) | 5,026 |
| 7 | 32 | 45,248 | 48,702 | 3,454 (7.09% of RTL) | 9,394 |

The seven-CTA gap remains at roughly 3,500–3,800 cycles when the work doubles. The extra 112 transfers take 19,376 cycles in SimX and 19,069 in RTL, a 1.61% difference relative to RTL. This shows that a lone CTA does not reproduce the remaining copy gap and the two measured copy lengths do not support attributing it to sustained line throughput alone. [One CTA RTL](../results/lmpar_cece299d3_h200s_11_rtlsim_n16g1/run.log) · [One CTA SimX](../results/lmpar_capture_v1_h200s_11_simx_g1/run.log) · [Seven CTA RTL](../results/lmpar_cece299d3_h200s_11_rtlsim_n16/run.log) · [Seven CTA SimX](../results/lmpar_capture_v1_h200s_11_simx_n16/run.log)

The h200s `smem_rate -k22 -w64 -i16 -f150` repeat passed after 1,417 seconds. It reproduces 140,238 RTL cycles against 139,931 in both original and fixed SimX, with 147,672 retired instructions in each model. The RTL cycle excess relative to SimX is 0.22%. At 22:37 EST, GEMM had finished uploading its inputs and was running on the same RTL library. [Fresh RTL load/store mix](../results/lmpar_cece299d3_h200s_11_rtlsim_k22/run.log) · [Fixed SimX load/store mix](../results/lmpar_capture_v1_h200s_11_simx_k22/run.log)

## 2026-11-24 23:01 EST · Reduced trace of the remaining copy gap

The instrumented eight-warp variant with VM and 256 slots reproduces the remaining h200s gap with TCU absent. Both lengths passed using one RTL build. At 16 tiles the fixed SimX and RTL take 23,357 and 27,335 cycles with 5,026 retired (14.55% gap relative to RTL). At 32 tiles they take 44,734 and 47,783 with 9,394 retired (6.38% gap). The 32-tile result is 1,172 cycles further apart than the sum of the two isolated VM and slot effects predicts. [16-tile RTL trace](../results/lmpar_w8_vm_slots256_trace_11_rtlsim_n16/run.log) · [32-tile RTL trace](../results/lmpar_w8_vm_slots256_trace_11_rtlsim_n32/run.log) · [32-tile SimX trace](../results/lmpar_w8_vm_slots256_trace_11_simx_n32/run.log)

For both lengths, the first observed difference is upstream of a GMEM request. RTL first admits warp 5 to the DXA request queue. SimX first admits warp 3. Each assigns its first transfer to worker 0. In the first wave of seven transfers, every RTL request takes two cycles from admission to worker allocation and every SimX request takes one. Both take six more cycles to start. The RTL descriptor FSM accounts for the extra register stage. The earlier descriptor probe adds that stage before allocation and changes full h200s copy time by only 131 cycles.

The trace joins all 28,672 or 57,344 lines across models after accounting for the 0x10000000 difference between logged request address bases. At 32 tiles, RTL takes 2,990 cycles longer from the first accepted worker capture to the last across all workers. Physical responses enter the unit MMU on both ports in the same cycle 17,899 times in RTL versus 22,314 in fixed SimX. Among matched lines assigned to the same port, the median RTL latency is 42 cycles shorter from request issue to MMU response and one cycle shorter from that response to capture. The longer burst across all worker captures remains correlated with different arrival schedules. This trace locates the earliest observed divergence at admission order, with its source upstream still unresolved. The single RTL build took 533 seconds. Phoenix job 16114658 took 11m12s including both RTL runs. [16-tile timing summary](../slurm/lmem-dma-rd-parity/w8_vm_slots256_trace/summary_n16.txt) · [32-tile timing summary](../slurm/lmem-dma-rd-parity/w8_vm_slots256_trace/summary_n32.txt) · [Build and account record](../slurm/lmem-dma-rd-parity/w8_vm_slots256_trace/job_runtime.txt)

Slurm accounting recorded 44.34 allocated CPU-hours across the 45 completed Phoenix points for this follow-up, all on `gts-hkim358`. At the quoted CPU-hour rate this is approximately 0.35. The large h200s RTL jobs run on Rover.

## 2026-11-24 23:32 EST · Full h200s remeasurement complete

All three one-SM RV64 h200s workloads passed numerical verification with overlap and LPC explicitly enabled. Each row has identical retired counts across the original SimX, fixed SimX and fresh RTL. The RTL values repeat Stage 2a exactly. The two percentage columns use different denominators. “RTL excess” divides by SimX, as in Stage 2a. The post-fix CI gap divides by RTL.

| Kernel | Retired | Original SimX cycles | Fixed SimX cycles | Fresh RTL cycles | RTL excess before → after | Post-fix CI gap |
|---|---:|---:|---:|---:|---:|---:|
| `bwstream_dxa -e4096 -n32 -d2 -c0` | 9,394 | 42,938 | 45,248 | 48,702 | 13.42% → 7.63% | 7.09% |
| `smem_rate -k22 -w64 -i16 -f150` | 147,672 | 139,931 | 139,931 | 140,238 | 0.22% → 0.22% | 0.22% |
| `gemm_block -m16 -n1024 -k4096 -s1` | 814,144 | 439,399 | 443,681 | 472,826 | 7.61% → 6.57% | 6.16% |

GEMM completed at 23:29:58 EST after 4,591 seconds. Its maximum K-walk time also repeats at 468,209 RTL versus 437,756 SimX. All six Rover points, including the seed and two diagnostics, share the same model key and unchanged library inode, size and mtime from the single build. The staged SimX benchmark source differs from the final commit only in a comment. [Copy RTL](../results/lmpar_cece299d3_h200s_11_rtlsim_n32/run.log) · [Load/store RTL](../results/lmpar_cece299d3_h200s_11_rtlsim_k22/run.log) · [GEMM RTL](../results/lmpar_cece299d3_h200s_11_rtlsim_gemm/run.log) · [Fixed GEMM SimX](../results/lmpar_capture_v1_h200s_11_simx_gemm/run.log) · [Model identity](../slurm/lmem-dma-rd-parity/lmpar_cece299d3_h200s_11_rtlsim_model_identity.txt)

Local commit `7966e9661` on `fix/lmem-dma-rd-parity` corrects the shared DXA response ordering and adds the RV64 regression. It fixes the reduced regression and closes part of the full copy and GEMM gaps. Both full gaps still exceed 5%, and the overlap-only reduced control remains above 5%. The remaining divergence appears before the DXA queue at different warp admission times and persists through different GMEM burst schedules. The precise cause of those earlier differences remains unresolved. The h200s profile still leaves both knobs disabled. [Regression checker replay](../slurm/lmem-dma-rd-parity/model_parity_replay.txt)
