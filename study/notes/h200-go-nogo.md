# H200 go/no-go experiments A, B, C (2026-11-21)

Real NVIDIA H200, node atl1-1-02-014-16-0 (driver 615.71.09, toolkit 12.9.1,
PyTorch 2.15.0+cu132 with cuBLAS 13.4.1), holder jobs 15978502 `gng_h200`
(17:42-20:12) and 15985032 (20:15-, same node). TPC mask map re-probed on this die (`results_gpu/h200_gng_map.txt`,
66 live bits, 0 mismatches; identical to `h200_l2s_map.txt` and `h200_tpc_map.txt`);
GPC layout from `h200_l2s_gpc.txt` (same die): GPC0 = bits 0-3 + 75, 78, 81, 83;
GPC1-5 = 8 TPCs each (bits 9-16, 18-25, 27-34, 36-43, 45-52); GPC6-7 = 9 TPCs
(54-62, 63-71).

Tags: MEAS = measured here (file named); INF = inference; UNV = not verified.

## 0. Answers

**A (SM sharing, X0): GO, residency direction.** Kill is ruled out.
- A lane-sized decode kernel (1 warp, 32 registers, 33 KB) **cannot become resident
  beside any of the production tensor-core GEMMs tested**: cuBLASLt BF16 and FP8,
  CUTLASS warp-specialised BF16 and FP8, M = 4096, K = 3840, N = 3840 and 15360.
  - With the lane on all 132 SMs first, each GEMM made no progress (0.1-0.3
    TFLOP/s against 600-1220 alone), in the same process and under MPS.
  - With the GEMM first, lane CTAs take whole SMs in the gaps between GEMM
    launches: lane 0.09-0.34 of solo on 12-48 SMs, GEMM 0.59-0.79. That is
    block-level interleaving, never a shared SM.
  - Removing one CUTLASS stage (230 -> 181 KB of shared memory, costing the GEMM
    3.5-9.7% alone) and forcing the 228 KB carveout makes the pair fit by every
    per-SM total, and it still does not co-reside. INF: the registers, 384 x 168
    per CTA, leave 256 per SM sub-partition, which is not enough for a 32-register
    warp.
- Where the lane does share SMs, with non-tensor-core lenders, it starves:
  - beside the DiT-style elementwise kernel: 0.05 of solo (TMA-only lane 0.28),
    elementwise 0.96-0.99, and the lane's `not_selected` share is ~0;
  - beside FMA ILP 4: 0.002, with 90% `not_selected`, FMA 0.97;
  - beside FMA ILP 2 with the lane resident first: lane 0.68-0.87 and the FMA job
    loses 20-24% (> 5%).
- So all three "go" conditions of the rule are met somewhere. The one that decides
  for production lenders is residency. The issue and isolation signatures appear
  only beside issue-heavy CUDA-core lenders.
- Disjoint baseline: a TMA lane on 24 spread SMs gets 1.36-1.42 TB/s while the
  GEMM keeps 0.73-0.77 of its all-SM rate. With the GEMM first, the "sharing"
  attempt gives the lane 0.33-1.25 TB/s at GEMM 0.68-0.73 (cuBLAS BF16, N = 15360).
- Caveat: the lane reached only 8.7-10.2 GB/s per SM with its mma consumer and
  30.5 with TMA only, not the 35 GB/s fair share. The residency result does not
  depend on its rate.

**B (bypassing shared memory): not decided.**
- My swap-AB wgmma kernels reached 66 GB/s per SM for (1) SS and (2) RS-from-shared,
  and 52.6 for (3) RS-from-global, on 1, 2 and 4 SMs of one GPC; outputs correct.
- That is below cuBLAS's padded-M BF16 kernel on the same SMs (119-125 GB/s per
  SM) and far below both thresholds of the rule (~200 for "software bypasses";
  157-170 for "bank sharing binds"). So the experiment does not test the hardware.
- One supported observation: register-A wgmma fed from global ran at half the rate
  of the same loads alone (52.6 vs 108.6 GB/s per SM).

**C (wrong LLM output): cause found, output fixed; kernel GB/s not re-measured.**
- cuBLAS 13.4 (PyTorch 2.15+cu132) returns NaN or wrong values for ordinary BF16
  GEMMs when the process runs under an MPS active-thread percentage:
  - 18 of 30 shapes bad at 80% (104 SMs), 7 at 60%, 25 at 40%, 0 at 100%;
  - first failing module in Qwen: `layers.0.mlp.gate_proj`.
- It is not the script, not uninitialised memory, not FLA/Triton, and not the
  co-runner.
- With TPC masks instead of percentages the output is correct at 26-132 SMs (SM
  count read back). But a masked process runs 2.5x slower on the host, and
  CUPTI-based profiling hangs, so clean kernel times under a reduced SM count
  were not obtained.
- DiT full DRAM traffic per forward: **178-209 GB per forward (ncu, 2,615 kernels), against 12.3 GB of weights. Over the 153.6 ms solo forward that is 1.16-1.36 TB/s = 25-29% of the 4.63 TB/s roof, not 1.7%**.

## 1. Experiment A: SM sharing (X0)

Raw: `results_gpu/h200_gng_a_same.txt` (one process, two streams, 545 slots),
`h200_gng_a_mps.txt` (two MPS clients, no percentage, 135 slots), smoke run
`h200_gng_a_smoke.txt` (default CUTLASS carveout). Summaries:
`h200_gng_a_same_summary.md`, `h200_gng_a_mps_summary.md` (`gpu/gng_analyze_a.py`).
All values are medians of 5 trials, ranges are min-max; windows 1.4 s.

### 1.1 The lane as built, alone (MEAS)

Lane = `gpu/gng_corun.cu` `k_lane`: 1 warp per SM, 32 registers
(`__maxnreg__(32)`, `cudaFuncGetAttributes` = 32; 56-64 B local spill), 2 x 16 KB
ring + 1 KB static = 33 KB, lane 0 issues `cp.async.bulk`, all 32 lanes consume
with `ldmatrix.x4` + `mma.sync.m16n8k16` (BF16, FP32 accumulate), activations from
L1, output per 1 MB of weights checked against a host reference (all checks ok).

| lane (all 132 SMs unless stated) | GB/s chip | GB/s per SM | B/clk per SM |
|---|---|---|---|
| M = 16, unpaced (`L16u`) | 1142 [1133-1143] | 8.65 | 4.4 |
| M = 16, paced at 8 GB/s per SM (`L16p`) | 1056 | 8.00 | 4.0 |
| M = 1 / M = 4, unpaced | 1351 / 1350 | 10.2 | 5.2 |
| M = 16 on 1 SM / 16 spread SMs | 8.7 / 138 | 8.7 | 4.4 |
| TMA only, no consumer (`Lt`, same 2 x 16 KB ring) | 4022 [4019-4024] | 30.5 | 15.4 |
| TMA only on 1 SM | 40.5 | 40.5 | 20.5 |

**The lane does not reach the 35 GB/s per SM fair share.** The consumer is
latency-bound: one warp at 32 registers cannot keep enough `ldmatrix`/L1 loads
and `mma` chains in flight (~75 cycles per 16-row k-step even with two
accumulator chains). The TMA-only form shows the ring alone allows 30-40 GB/s.
The paced variant was therefore set to 8 GB/s (below the lane's own ceiling)
instead of 35. Both forms are reported; neither changes the verdict below.

### 1.2 Residency: can the lane share an SM with a production GEMM? (MEAS + INF)

| lender kernel | threads x regs | regs per SM | dynamic smem | fits beside a 33 KB, 32 x 32-reg lane by SM totals? | co-resident in practice? |
|---|---|---|---|---|---|
| cuBLASLt BF16/FP8, N = 3840/15360 (nvjet) | not instrumented (the borrow run read 384 x 168 for an nvjet BF16 kernel) | | | (by that count: registers exactly full) | **no**: 0.2 TFLOP/s while the lane holds all SMs |
| CUTLASS BF16 128x256x64, 4 stages (auto) | 384 x 168 | 64,512 | 230,400 B | no (smem) | **no** |
| CUTLASS FP8 128x256x128, 4 stages | 384 x 168 | 64,512 | 230,400 B | no (smem) | **no** |
| CUTLASS BF16/FP8, 3 stages ("m1"), 228 KB carveout | 384 x 168 | 64,512 | 181,248 B | **yes**: 215 KB of 228 KB smem, 65,536 of 65,536 registers, 416 threads | **no** (0.3 TFLOP/s, 5 of 5 trials lane-first) |
| elementwise `ew` (256-thread CTAs) | | | 0 | yes | yes (lane on all 132 SMs, ew at 0.99) |
| FMA ILP 2/4 (1 CTA x 1024 threads) | | | 0 | yes | yes |

- With the lane resident first on all 132 SMs, **every tensor-core GEMM stops**
  (0.1-0.3 TFLOP/s, i.e. only the launches after the lane's window), in both the
  same process and under MPS. With the GEMM launched first, the lane's CTAs take
  whole SMs in the gaps between GEMM launches and hold them (12-48 SMs, phenomenon
  A); they never share an SM with a GEMM CTA.
- The 3-stage CUTLASS kernel fits by every per-SM total and still does not co-reside.
  INF: the register file is allocated per SM sub-partition (4 x 16,384). 12 GEMM
  warps x 5,376 registers = 3 warps x 5,376 = 16,128 per sub-partition, leaving 256,
  i.e. 8 registers per thread for a 32-thread lane warp. The lane would need the
  GEMM to give up registers, not shared memory. This is not verified (no
  register-reduced CUTLASS build was run).
- Cost of the one shared-memory carve-out that was measured (one fewer stage),
  GEMM alone: BF16 573 vs 634 TFLOP/s (-9.7%), FP8 1107 vs 1147 (-3.5%), N = 15360.
  It buys nothing, because the registers still do not fit.

### 1.3 Co-run ratios (MEAS, `h200_gng_a_same_summary.md`; MPS rows agree within 1%)

Lane ratio = lane GB/s / its solo; lender ratio = lender rate / its solo. "first" =
which job became resident first.

| lane | lender | first | lane ratio [range] | lender ratio [range] | lane SMs | what happens |
|---|---|---|---|---|---|---|
| L16u | cuBLAS BF16 N=15360 | lane | 1.000 [0.992-1.001] | **0.000** | 132 | GEMM cannot become resident |
| L16u | cuBLAS BF16 N=15360 | GEMM | 0.289 [0.216-0.314] | 0.730 [0.726-0.791] | 44 | SM interleaving (A) |
| L16u | cuBLAS FP8 N=15360 | GEMM | 0.289 [0.254-0.303] | 0.736 [0.731-0.764] | 46 | (A) |
| L16u | cuBLAS BF16 N=3840 | GEMM | 0.085 | 0.725 | 12 | (A) |
| L16u | CUTLASS BF16 N=15360 | GEMM | 0.171 [0.156-0.320] | 0.667 [0.631-0.668] | 23 | (A) |
| L16u | CUTLASS FP8 N=15360 | GEMM | 0.188 [0.139-0.315] | 0.666 [0.642-0.674] | 26 | (A) |
| L16u | CUTLASS BF16 3-stage | either | 0.994-1.000 | **0.000** [0-0.559] | 132 | GEMM excluded |
| Lt | cuBLAS BF16 N=15360 | GEMM | 0.311 | 0.682 | 44 | (A) |
| L16u | elementwise | lane | **0.051** [0.051-0.051] | 0.986 | 132 | **co-resident, lane starved** |
| L16u | elementwise | ew | 0.035 | 0.979 | 92 | co-resident, starved |
| Lt | elementwise | lane | **0.283** [0.281-0.284] | 0.956 | 132 | co-resident, starved |
| L16u | FMA ILP 4 | either | **0.002** | 0.968-0.970 | 132 | co-resident, starved |
| Lt | FMA ILP 4 | lane | 0.214 | 1.000 | 132 | co-resident, starved |
| L16u | FMA ILP 2 | lane | 0.678 | **0.762** | 132 | both lose |
| L16u | FMA ILP 2 | FMA | 0.011 | 0.952 | 132 | starved |
| Lt | FMA ILP 2 | lane | 0.865 | **0.805** | 132 | both lose |
| L1u / L4u | elementwise, FMA 4, GEMMs | | same pattern (0.044, 0.002, 0.29 / GEMM 0.73) | | | |

- Elementwise: the elementwise kernel moves 3.1 TB/s and the lane 1.14 TB/s alone;
  together that is 4.2 TB/s, under the 4.63 TB/s roof. So the lane's fall to 58 GB/s
  is not DRAM sharing. Clock: 1515-1545 MHz in those slots (power cap, reason 0x4)
  vs 1980 for the lane alone, which explains at most a 22% loss, not 95%.
- Clocks: lane and FMA slots ran at 1980 MHz with no reason set; every GEMM and
  elementwise slot was power-capped (0x4), GEMMs at a median 1.49-1.82 GHz.
- Every lane/GEMM/elementwise output check passed in every slot.

### 1.4 Disjoint baseline (MEAS)

Lane in its disjoint form (TMA-only, 6 x 32 KB ring, unpaced) on 24 or 32 SMs
spread over all 8 GPCs; GEMM masked to the other TPCs and told the SM count
(CUTLASS `sm_count`, cuBLASLt `SM_COUNT_TARGET`).

| lane SMs | GEMM | lane TB/s (alone) | GEMM / its all-132 solo [range] | GEMM / its masked solo |
|---|---|---|---|---|
| 24 | cuBLAS BF16 N=15360 | 1.42 (2.04) | 0.729 [0.727-0.730] | 0.853 |
| 24 | CUTLASS BF16 N=15360 | 1.36 (2.04) | 0.766 [0.763-0.768] | 0.861 |
| 32 | cuBLAS BF16 N=15360 | 2.16 (2.72) | 0.483 [0.483-0.484] | 0.853 |
| 32 | CUTLASS BF16 N=15360 | 1.79 (2.72) | 0.684 [0.680-0.686] | 0.816 |

Caveats: this disjoint lane reached only 85 GB/s per SM alone (the 6 x 32 KB TMA
loop should give ~200; the same try_wait-loop code-generation effect as in
`tpc-placement-and-borrowing.md` B.4 is the likely cause, UNV), and cuBLAS with
`SM_COUNT_TARGET = 100` picked a poor kernel (0.48). Still, at a similar GEMM share
(0.68-0.77) the disjoint split delivers 1.4-1.8 TB/s to the lane, against 0.33
(L16u) to 1.25 TB/s (Lt) when the two are left to share SMs, because sharing never
happens with a tensor-core GEMM.

### 1.5 Stall evidence

CUPTI continuous PC sampling (the CUDA 12.9 toolkit sample injection library
`pc_sampling_continuous`, collection mode 1 = concurrent kernels allowed), one
process per configuration, same process, unmasked (the mask callback and CUPTI
cannot both subscribe; the binary installs the mask callback only when a mask is
used). Raw: `results_gpu/h200_gng_a_stall2.txt`, `h200_gng_a_stall3.txt` and
their `.util` dumps; `gpu/gng_pcsum.py`. Shares of the lane kernel's samples:

| configuration (all 132 SMs) | lane rate this run | lane: top stall reasons | lender: top stall reasons |
|---|---|---|---|
| L16u alone | 1133 GB/s | wait 45%, selected 22%, short_scoreboard 18%, long_scoreboard 10%; not_selected ~0 | - |
| L16u beside FMA ILP 4 (FMA first) | 1.5 GB/s | **not_selected 90.0%**, math_pipe_throttle 9.8% | FMA: not_selected 70%, selected 12.5% |
| Lt beside FMA ILP 4 (lane first) | 865 GB/s | long_scoreboard 38%, wait 25%, branch 11%, selected 10%, not_selected 6% | as above |
| L16u beside FMA ILP 2 (lane first) | 768 GB/s | as alone (not_selected 1.7%) | FMA: not_selected 54%, barrier 15%, dispatch 11% |
| L16u beside elementwise (lane first) | 58 GB/s | as alone (wait 43%, selected 21%, short_scoreboard 17%, long_scoreboard 14%); **not_selected ~0** | ew: long_scoreboard 50%, drain 14%, not_selected 7.5% |
| Lt beside elementwise (lane first) | 1124 GB/s | long_scoreboard 41%, wait 26%, branch 12% | as above |

- **Beside FMA ILP 4 the lane is starved of issue** (90% `not_selected`). This is the
  issue-priority signature of the decision rule.
- **Beside the elementwise kernel it is not.** The lane's per-PC histogram is the
  same as when it runs alone (top PC offsets 0x1900, 0x1b60, 0x1b90 hold the same
  shares). It collects ~1/3 of the samples while doing 1/20 of the work.
  INF: the warp spends most of its time suspended in `mbarrier.try_wait`, waiting
  for its TMA data. Suspended warps are not sampled, so the starvation is on the
  memory path (the lane's bulk copies queue behind the elementwise kernel's
  loads), not in issue. UNV: confirming this needs the TMA transfer latency
  stamped per tile under the elementwise lender, which was not run.
- No PC sampling was taken beside the GEMMs: they never share an SM with the lane.



## 2. Experiment B: bypassing shared memory

Kernel `gpu/gng_swapab.cu`: swap-AB `wgmma.m64n16k16` BF16 -> FP32, weights =
64-row A operand, 16 tokens = N, K = 1024 per 128 KB row block, X (16 x 1024) in
shared memory as the B operand; 1-3 consumer warpgroups plus one producer
warpgroup; 128 registers per thread, 1 CTA per SM; 256 MB of weights per SM,
wrapped. Every compute variant's output was checked (3 row blocks per CTA,
64 x 16 values, exact) and passed. Raw: `results_gpu/h200_gng_b2.txt` (second
holder; the first sweep `h200_gng_b.txt` is invalid, see below). SM clock 1980 MHz,
no reason set, in every B run.

| variant (1 SM = SM 12, GPC 6) | GB/s per SM [range, 5 trials] | B/clk | useful TFLOP/s per SM |
|---|---|---|---|
| (1) SS: A and B from shared memory, A staged by TMA (10 x 16 KB ring), 2 WG | 66.0 [66.0-66.0] | 33.3 | 1.06 |
| (2) RS: A in registers from the TMA ring with `ld.shared.v4`, 2 WG | 66.2 [66.1-66.2] | 33.4 | 1.06 |
| (3) RS: A in registers straight from global, 16 B `ld.global.nc`, 16 k-steps in flight, 3 WG | **52.6** [52.4-52.6] | 26.6 | 0.84 |
| (3), 2 WG | 38.0 | 19.2 | 0.61 |
| control: (3)'s loads without wgmma, 3 WG | 108.6 [108.6-108.7] | 54.8 | - |
| control: TMA ring only, no consumer reads (10 x 16 KB) | 92.1 | 46.5 | - |
| (3) on 2 SMs of one TPC / 4 SMs of GPC 6 | 52.6 / 52.6 | 26.6 | 0.84 |
| (1) on 4 SMs of GPC 6 | 66.0 | 33.3 | 1.06 |
| cuBLASLt BF16 padded M, M = 1 / 16 (D = X W^T, K = 4096, N = 28672), 2 SMs (1 TPC) | 123.9 / 124.9 | 62.6 / 63.1 | 0.12 / 2.0 |
| same, 4 SMs (2 TPCs of GPC 6) | 118.6 / 118.7 | 59.9 | 0.12 / 1.9 |

(cuBLAS rows: from the first sweep `h200_gng_b.txt`, whose cuBLAS runs are not
affected by the defect below. cuBLAS cannot be placed on 1 SM with a TPC mask.)

Reading against the rule:
- (3) reaches 52.6 GB/s per SM, not ~200. (1)/(2) reach 66, not 157-170. **Neither
  branch of the decision rule applies: my kernels are slower than cuBLAS's padded
  kernel (124 GB/s per SM) and therefore do not probe the hardware limit.** B is
  not decided.
- Two things the data does support:
  - Feeding the register operand straight from global halves the load rate: the
    same loads reach 108.6 GB/s per SM without the wgmma and 52.6 with it. INF:
    the `wgmma.fence` / `WARPGROUP.ARRIVE` before each register-A wgmma waits for
    the warp's outstanding loads, so register-destination prefetch does not
    overlap across k-steps (UNV; not checked in SASS stall data).
  - The staged variants are capped by my producer, not by bank sharing: the TMA
    ring alone reaches only 92 GB/s (tpc_lab's one-thread 32 KB x 6 loop gets 203).
- The rates are identical on 1, 2 and 4 SMs of one GPC, so nothing here is a
  GPC limit.

The first B sweep (`h200_gng_b.txt`, 20-26 GB/s for every variant, including the
no-wgmma controls) was limited by 64-bit runtime `%` and `/` in the address
arithmetic: one software division sequence per load or per stage made every
loop instruction-bound (rate proportional to stage size, flat in ring depth).
Replaced by incremental counters for the second sweep.


## 3. Experiment C: the wrong LLM output

**Cause (MEAS): cuBLAS returns NaN or wrong values for ordinary BF16 GEMMs when
the process runs under an MPS active-thread percentage.** It is not the script,
not uninitialised memory in PyTorch, not a Triton/FLA grid choice, and not a race
with the co-runner (it happens with the model alone).

Evidence, in order (`results_gpu/h200_gng_c_diag.txt`, `h200_gng_c_gemm.txt`):

1. Reproduced, deterministic. Qwen3.5-9B prefill + 32 decode steps, batch 1,
   alone in an MPS client. The SM count was read back in every run three ways
   (torch device property, Triton driver property, distinct `%smid` of a probe
   kernel launched from the same process, `libgng_smcount.so`):

   | MPS % | SMs (attr = torch = Triton = distinct %smid) | prefill logits finite | decode steps non-finite | sample |
   |---|---|---|---|---|
   | none | 132 | yes | 0/31 | correct |
   | 100 | 132 | yes | 0/31 | correct |
   | 80 (twice) | 104 | **no** | 31/31 | `!!!!` |
   | 70 | 92 | **no** | 31/31 | `!!!!` |
   | 60 | 78 | yes | 0/31 | correct |
   | 40 | 52 | **no** | 31/31 | `!!!!` |
   | 20 | 26 | **no** | 31/31 | `!!!!` |
   | 10 | 12 | yes | 0/31 | correct |

   This matches the old pattern exactly (80/70/40/20 wrong, 100/60/10/5 right;
   the co-runs at LLM 20% and 40% wrong, at 60% right). `!` is token 0: argmax of
   all-NaN logits.
2. Not the Python-level SM count: with no MPS, making torch, Triton and FLA report
   104 or 78 SMs gives correct output.
3. Not uninitialised reads: with no MPS, filling PyTorch's caching allocator with
   NaN before loading the model gives correct output.
4. First non-finite module (forward hooks, MPS 80%): `model.layers.0.mlp.gate_proj`,
   a plain `nn.Linear` in BF16 whose inputs are finite.
5. Standalone check, `torch.nn.functional.linear` in BF16 against an FP32
   reference, Qwen's Linear shapes (in -> out = 4096 -> 12288, 12288 -> 4096,
   4096 -> 4096/1024/8192/31040) at M = 1, 4, 16, 26, 64, both torch BLAS back
   ends (cuBLASLt and cuBLAS):

   | MPS % (SMs) | cuBLASLt: bad / non-finite of 30 shapes | cuBLAS: bad / non-finite |
   |---|---|---|
   | 100 (132) | 0 / 0 | 0 / 0 |
   | 80 (104) | 18 / 11 | 18 / 9 |
   | 60 (78) | 7 / 6 | 5 / 4 |
   | 40 (52) | 25 / 14 | 25 / 14 |

   "Bad" = any non-finite value or max relative error > 5%; at 40% even M = 1
   gives 81-84% error. So 60% was correct for the model only because its shapes
   at M = 1 and 26 happened to be among the good ones.
6. With the SM allotment set by a TPC mask instead (LD_PRELOAD `tpc_preload.so`,
   inside an MPS client without a percentage, 13 TPCs = 26 SMs read back), the
   output is correct.

INF: the defect is in cuBLAS 13.4's kernels (or the driver) under an MPS
percentage, where the context sees a reduced SM count and virtual `%smid`
0..n-1; a reduced SM count by itself (TPC mask, attribute still 132) is fine.
Whether CUDA 12.9's cuBLAS has the same defect was not tested (UNV).

**Consequence for earlier notes.** Every Qwen number taken under MPS 80/70/40/20%
(`h200_qwen_mps.txt`) and in the co-runs at LLM 20% and 40%
(`h200_models_corun.txt`) came from a run computing NaN. Their kernel times may
or may not be representative (NaN does not slow a GEMM, but the defective kernels
are the ones that ran). The Z-Image side of those co-runs was not checked for
correctness (its MPS shares 80/60/40% are also in the affected range; UNV).

**Re-measurement with correct output** (`results_gpu/h200_gng_c_qwen_mask.txt`,
second holder, no MPS, SM allotment by a packed TPC mask through LD_PRELOAD
`tpc_preload.so`, 5 s windows, batch 1 and 16):

| TPCs | SMs read back (distinct %smid; attribute stays 132) | sample | step ms (b=1 / b=16) | Linear time per step by CUDA events (b=1) |
|---|---|---|---|---|
| 13 | 26 | correct | 68.7 / 69.6 | 14.2 ms |
| 26 | 52 | correct | 68.8 / 70.4 | 11.5 ms |
| 39 | 78 | correct | 68.3 / 70.1 | 11.2 ms |
| 52 | 104 | correct | 69.1 / 70.4 | 10.7 ms |
| 66 | 132 | correct | 68.5 / 70.5 | 10.8 ms |

- The output is correct at every SM count, so the earlier "MPS share" points can be
  re-taken with masks instead of percentages.
- **The kernel times could not be measured cleanly under a mask.**
  - `torch.profiler` hangs as soon as it starts in a masked process, with or
    without MPS (13 and 26 TPCs timed out at 200-240 s). INF: CUPTI and the
    mask both subscribe to the driver's launch callback.
  - With the mask installed, every decode step takes ~68-70 ms even on all 132 SMs
    (26.8 ms without it, `h200_qwen_solo.txt`). The per-launch callback makes the
    host-bound decode 2.5x slower. The CUDA-event sums around each `nn.Linear`
    then include host gaps: 10.8 ms at 132 SMs, against 4.99 ms from the profiler
    in the old unmasked run. They are upper bounds, not kernel times, and are not
    converted to GB/s per SM here.
- So the honest statement is: correct output is restored, the SM count is now
  read back, and kernel-level GB/s per SM under a reduced SM count still needs a
  method that does not use CUPTI (for example in-kernel `%globaltimer` stamps in
  a C++ harness around the same cuBLAS calls).

**DiT full DRAM traffic** (`results_gpu/h200_gng_dit_dram.csv`,
`h200_gng_dit_dram_nocacheflush.csv`; `gpu/gng_dit_dram.py` under `ncu
--profile-from-start off`, one transformer forward, 1024 x 1024, BF16, after one
warm forward; `dram__bytes_read.sum`, `dram__bytes_write.sum`,
`gpu__time_duration.sum` per kernel, summed over 2,615 kernels):

| ncu cache control | DRAM read | DRAM write | total | serialized kernel time |
|---|---|---|---|---|
| `all` (default: caches flushed before every kernel) | 131.3 GB | 47.0 GB | 178.2 GB | 134.6 ms |
| `none` | 127.6 GB | 81.5 GB | 209.2 GB | 137.7 ms |

- The two modes bracket the real value. Flushing makes every kernel re-read its inputs.
  Without flushing, write-backs of earlier kernels' dirty lines are charged to later
  kernels.
- Against the 153.6 ms solo forward (`h200_zimage_solo.txt`): **1.16-1.36 TB/s on
  average, 25-29% of the 4.63 TB/s roof.** The weights are 12.3 GB (1.7%).
- Largest contributors (flushed run):
  - the main nvjet GEMM: 42.5 GB read in 58.7 ms;
  - PyTorch vectorised elementwise kernels: about 28 + 20 + 16 GB read and
    8 + 16 + 11 GB written;
  - a reduction kernel: 12 GB.
- Kernel times under ncu are serialized and only indicative.



## 4. Method

Common to A and B (sources `gpu/gng_*`, built into `/tmp/$USER/gng` by `gpu/gng_build.sh`):

- **Placement.** Per-launch TPC masks through a sticky per-thread version of the
  libsmctrl TMD callback (`gpu/gng_common.h`; it stays set for every launch from
  that host thread, so back-to-back cuBLASLt/CUTLASS launches stay masked), plus an
  SM filter in our own kernels (each CTA reads `%smid`, claims its SM once, extra
  CTAs exit). Every lane, FMA and swap-AB CTA's `%smid` is recorded and printed as
  a bitmap; CUTLASS GEMM CTAs record `%smid` through a copy of CUTLASS's
  `device_kernel` (`k_cutlass_smid`, last 8 launches). cuBLAS CTAs cannot be
  instrumented; their placement is inferred from rates (below).
- **Windows.** A: a fixed schedule of 2.6 s slots shared by all processes of a run
  (same epoch, so the same binary does two streams in one process and two MPS
  clients). First job active 0-2.3 s, second 0.3-2.0 s, measurement window
  0.45-1.85 s = **1.4 s**, identical for solo slots. Lane and FMA rates come from
  `%globaltimer` stamps inside the window (lane: every 64 tiles per CTA; FMA: every
  ~260 us chunk per CTA), lender launch rates from a CUDA event after every launch,
  mapped to `%globaltimer` by a stamp kernel. B: each trial runs >= 1.2 s, sized
  from a calibration run; rate = bytes / (t_end - t_start) per CTA.
- **Clock and power.** NVML sampled every 10 ms in every process during the run
  (SM clock, memory clock, power, clock-event reasons); each slot reports the
  minimum and median SM clock and median/max power inside its window and the OR
  of the reasons (0x4 = software power cap). GEMMs run power-capped (0x4) at
  1.2-1.8 GHz; the lane, FMA and swap-AB kernels at 1980 MHz with no reason set.
- **Trials.** 5 per configuration, interleaved (trial loop outermost); medians and
  min-max over trials. Ratios are median(co-run) / median(solo, same mode).
- **Buffers.** cudaMalloc only. Lane: 132 disjoint 128 MB slices (17 GB, far
  beyond the 60 MB L2), wrapped. Swap-AB: 256 MB per SM.
- **Correctness.** Lane: 2 random 1 MB output blocks per slot against a host FP64
  reference (values are multiples of 1/32, exact in FP32). GEMMs: 32 sampled
  outputs against a host reference per slot. Elementwise: 64 samples. Swap-AB:
  3 row blocks per CTA (64 x 16 outputs each). cuBLAS M <= 16 baseline: 64
  samples. FMA: not checked (synthetic).
- **Node / die:** see the header. MPS runs: `nvidia-cuda-mps-control -d` as the
  user, no active-thread percentage in A.


## 5. What could not be measured, and why

- **Lane at the 35 GB/s fair share.** With 1 warp and 32 registers the mma consumer
  reached 8.7-10.2 GB/s per SM. The TMA-only lane reached 30.5 (40.5 on one SM).
  Pacing at 35 was therefore meaningless and was run at 8 GB/s.
- **cuBLAS kernel resources and %smid.** cuBLAS CTAs cannot be instrumented. Their
  non-residency is shown by rates (0 progress while the lane holds every SM), not by
  `%smid`. Registers and shared memory per nvjet kernel were not read in this run.
  ncu works on this node, but the launch metrics were not collected for lack of
  holder time.
- **What the GEMM must give up in registers.** The likely blocker (registers per
  sub-partition) was not tested. That would need a CUTLASS build with a lower
  launch register count, which requires changing its `setmaxnreg` 40/232 split.
- **Stall reasons beside the GEMMs:** not applicable, they never share an SM.
  Beside the elementwise kernel the lane's waiting time is attributed by
  inference (suspended `try_wait` is not sampled).
- **Disjoint lane rate.** The 6 x 32 KB TMA lane reached 85 GB/s per SM alone,
  not ~200 (tight `try_wait` loop, UNV). cuBLAS with `SM_COUNT_TARGET = 100` picked
  a poor kernel (0.48 of all-SM).
- **B at the hardware limit.** My kernels stayed below cuBLAS, so the rule's two
  outcomes were not reached. The staged variants are capped by my producer (TMA
  ring alone 92 GB/s).
- **C kernel times under a reduced SM count.** `torch.profiler` hangs with the TPC
  mask (with and without MPS). The mask's per-launch callback slows the host-bound
  decode 2.5x, so CUDA-event timings include host gaps.
- **Whether CUDA 12.9's cuBLAS has the MPS defect, and whether the Z-Image outputs
  under MPS 80/60/40% were wrong.** Not checked.


## 6. Corrections to earlier notes

- `sm-sharing-design-space.md` §0 item 3 and §1.3 item 11, "a one-warp lane with a
  ring of 32 KB or less would fit *today*" beside the cuBLAS GEMM (1,024 registers
  and ~38 KB left): **wrong in practice**. Totals are not the constraint. No
  1-warp, 32-register, 33 KB lane co-resided with any cuBLAS or CUTLASS Hopper
  GEMM, including a 181 KB CUTLASS variant under the 228 KB carveout. INF:
  per-sub-partition registers.
- `framing-critique.md` item 2 / `corun-workloads.md` (Qwen under MPS): the `!!!!`
  runs are cuBLAS computing NaN under MPS percentages. The 60% run was correct by
  luck of shapes. Do not use MPS active-thread percentages with this PyTorch/cuBLAS
  for correctness-sensitive runs. Every MPS-percentage Qwen timing at 80/70/40/20%
  and the co-runs at LLM 20/40% are from runs that computed NaN.
- "DiT uses 1.7% of memory bandwidth" (weights only, 12.3 GB per 153.6 ms forward):
  wrong by an order of magnitude. Full DRAM traffic is 178-209 GB per forward (14-17x the weights), 25-29% of the roof on average (§3). The DiT is therefore not a bandwidth-free lender: its elementwise kernels alone move ~100 GB per forward.
- `sm-sharing-design-space.md` §0 item 1: starvation beside a production-style
  kernel is now measured for a DiT-style elementwise kernel (lane 0.04-0.05). Its
  signature is not issue (`not_selected` ~0) but waiting for the lane's own
  memory. The FMA-lender starvation is confirmed as issue (90% `not_selected`).


## 7. Files

Sources (`gpu/`): `gng_common.h` (sticky TPC mask, NVML sampler, timing),
`gng_corun.cu` (A: lane, FMA, elementwise, cuBLASLt, CUTLASS through
`gng_cutlass_api.h` / `gng_cutlass.cuh` / `gng_cutlass_kind.cu`), `gng_swapab.cu`
(B), `gng_smcount.cu` (SM read-back library), `gng_llm_diag.py`, `gng_c_gemm.py`,
`gng_dit_dram.py` (C), `gng_sched.py`, `gng_analyze_a.py`, `gng_analyze_b.py`,
`gng_pcsum.py`, scripts `gng_build.sh`, `gng_holder.sbatch`, `gng_srun.sh`,
`gng_run_a.sh`, `gng_run_b.sh`, `gng_run_b2.sh`, `gng_after_a.sh`,
`gng_holder2_work.sh`, `gng_stall*.sh`, `gng_c1.sh`-`gng_c6.sh`. Edited:
`llm_decode.py` (SM read-back when `GNG_SMCOUNT` is set; `--no-profile`,
`--event-timing`). Binaries only under `/tmp/$USER/gng`.

Raw (`results_gpu/`): `h200_gng_env.txt`, `h200_gng_env2.txt`, `h200_gng_map.txt`,
`h200_gng_probe*.txt`, `h200_gng_a_smoke.txt`, `h200_gng_a_same.txt` (+
`_summary.md`), `h200_gng_a_mps.txt(.lane/.lender)` (+ `_summary.md`),
`h200_gng_a_stall2.txt`, `h200_gng_a_stall3.txt` (+ `.util`), `h200_gng_b.txt`
(invalid first sweep), `h200_gng_b2.txt`, `h200_gng_c_diag.txt`,
`h200_gng_c_gemm.txt`, `h200_gng_c_qwen_mask.txt`, `h200_gng_dit_dram*.csv`,
holders `h200_gng_holder.out` (job 15978502) and `h200_gng_holder2.out` (job
15985032, same node).


Side measurement in the second holder (for the coordinator): one-warp branch costs,
`results_gpu/h200_warprate_nottaken.txt`, generator `gpu/warprate_nt_gen.py`.
