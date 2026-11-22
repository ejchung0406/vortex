# Critique of the research framing (2026-11-21)

Source: an independent reviewer agent acting as a conference and fellowship
reviewer. Items marked [checked] were re-checked against the raw logs by the
main session; the rest are the reviewer's reading.

## Verdict
Revise. The H200 characterisation is solid (placement and GPC numbers match
the raw logs). The hardware thesis is not supported yet.

## What undercuts it
1. [checked] The starvation result (memory job keeps 0.6-22%) was measured only
   beside a synthetic FP32 FMA kernel. `results_gpu/h200_borrow_e3.txt` lines
   14-21 hold the cuBLAS GEMM runs, which our summaries left out:
   - 132 memory blocks beside the GEMM: memory keeps 0.63-0.78, GEMM drops to
     0.25 (757 -> 190 TFLOP/s). `mem_distinct_sms_co` = 43-66, so the two
     alternate on SMs (block-scheduler interleaving), not warp starvation.
   - 16 memory blocks: memory keeps 0.66-0.71, GEMM 0.89-0.99.
2. [checked] The Qwen decode printed `'!!!!…'` in 4 of the MPS runs
   (`h200_qwen_mps.txt`) and 2 co-runs (`h200_models_corun.txt`). Those timings
   come from a run that produced wrong output. Cause unknown. **[corr. 2026-11-21]** Cause found: cuBLAS 13.4
   returns NaN for BF16 GEMMs under an MPS active-thread percentage; every Qwen number at MPS 80/70/40/20% and
   the co-runs at LLM 20/40% are invalid (`h200-go-nogo.md` §3).
3. The launch-order x ILP table in `tpc-placement-and-borrowing.md` §C.2 has no
   `ilp=1` run in the raw files; some entries do not match `h200_tpc_coresid2.txt`.
4. Two packed GPCs give 2.27-2.28 TB/s, not 2 x 1.28: a second limit above the
   GPC that the notes do not mention.
5. "One SM ingests 205-219 GB/s" depends on the kernel: 110, 115, 142, 205-211
   and 239 on the same die; the same kernel gives 140 or 205 by code generation.
6. Bandwidth windows are 3.6-6.7 ms; the clock was read at idle, not during runs.
7. Deployment: HF decode is host-bound (20 of 27 ms per step), so the measured
   user-visible loss under MPS is 2-3%. "DiT uses 1.7% of bandwidth" counts
   weights only (**[corr. 2026-11-21]** measured: 178-209 GB per forward = 1.16-1.36 TB/s, 25-29% of the roof,
   `h200-go-nogo.md` §3). At batch 64 decode needs ~39 SMs of tensor throughput, so it
   is no longer bandwidth-only.
8. Prior art missing from our survey: intra-SM QoS (Wang et al. ISCA'17,
   Simultaneous Multikernel HPCA'16, Warped-Slicer ISCA'16), Mascar HPCA'15,
   Orion, Tally, POD-Attention, NanoFlow, MuxServe, megakernels.
9. Simulator: the per-warp rate (1 instruction per ~11 cycles) and an unmatched
   warp-scheduler policy make the starvation phenomenon impossible to reproduce
   today. The DXA calibration (9 knobs, 16 points) has no held-out data.
10. Profile `h200g2` (2 GPCs, 2 stacks): memory job's SM share becomes 25%
    against 17% on the part, which inflates the cost of disjoint partitioning,
    the baseline an isolation proposal must beat. Use the 64-SM / 4-GPC /
    3-stack shape for ratio-dependent claims; keep `h200g2` for per-SM studies.

## Reviewer's ranking
1. SM-sharing isolation, reframed as "compute and bandwidth cannot be
   allocated separately" (MIG couples them) — only if the go/no-go below passes.
2. Characterisation/placement paper — safest, most data exists.
3. GPC egress. 4. Tensor core without shared memory.

Measured headroom of sharing over disjoint SMs so far: about +9 points
(4.33 TB/s at 0.86 compute against ~0.77 interpolated); ideal ceiling +25-30%.

## First three steps proposed
1. Real-H200 go/no-go: a lane-sized decode kernel (1 warp, <=32 registers,
   <=32 KB shared memory) co-resident with a DiT-shaped production wgmma GEMM,
   both launch orders, 1 s windows, clock/power logged, PC-sampling stall
   reasons. If the lane runs at full rate with no GEMM loss, isolation is dead.
2. Fix the deployment evidence (cause of `!!!!`, GPU-bound runtime, full DRAM
   bytes for DiT via ncu, batch sweep, SM counts read back) and the related work.
3. Make the simulator show the problem before fixing it (per-warp rate, warp
   scheduler policy against the co-residency matrix, held-out points).
