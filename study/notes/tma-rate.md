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
