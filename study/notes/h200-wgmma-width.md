# H200 tensor-core throughput vs wgmma tile width and number of issuers (2026-11-22)

Real NVIDIA H200, node atl1-1-02-014-16-0, GPU 00000000:CB:00.0, driver 615.71.09,
CUDA 12.9.1, holder job 15992837 `wgw_h200` (about 11 min, cancelled when done).
TPC map re-probed (`results_gpu/h200_wgw_map.txt`): 66 live bits, 0 mismatches,
identical to `h200_gng_map.txt`. One-SM runs use TPC bit 0 (SMs 0 and 1).
Tags: MEAS = measured here; INF = inference.

Scope was narrowed by the coordinator mid-run to the production arrangement:
one CTA per SM, 1 and 2 consumer warpgroups, SS operands, N = 64 / 128 / 256,
plus the production GEMM reference. The other rows below were already measured
before the narrowing. They are reported but not extended. The issue-interval
sweep (item 3) is incomplete: 3 of 42 points.

## 0. Answers

**FLOPs per SM clock, FP16 in / FP32 accumulate, m64nNk16, ONE CTA on one SM (MEAS).**
Production cadence: per k-tile, `wgmma.fence`, 4 x wgmma (K = 64), `commit_group`,
`wait_group 1` (CUTLASS `K_PIPE_MMAS = 1`). SS = A and B from shared memory.
Results are medians of 5 trials of >= 0.5 s; min = max in every cell shown
(spread < 0.1%). Every cell passed the exact correctness check.

| N | 1 WG, SS | 2 WG, SS | 4 WG, SS | max WG, SS (n) | 1 WG, RS | 2 WG, RS |
|---|---|---|---|---|---|---|
| 8 | 910 | 910 | 910 | 910 (16) | 1,260 | 1,260 |
| 16 | 1,638 | 1,638 | 1,638 | 1,638 (12) | 2,521 | 2,521 |
| 32 | 2,731 | 2,731 | 2,731 | 2,731 (8) | 4,096 | 4,096 |
| **64** | **4,096** | **4,096** | 4,096 | 4,096 (8) | 4,096 | 4,096 |
| **128** | **4,096** | **4,096** | 4,096 | 4,096 (4) | 4,096 | 4,096 |
| **256** | **4,096** | **4,096** | - | 4,096 (3) | 4,096 | 4,096 |

- **One warpgroup saturates the SM's tensor cores** when wgmmas are issued 4 per
  commit group:
  - SS: at N >= 64, 4,096 FLOPs per clock;
  - RS: at N >= 32.
  - Adding warpgroups or CTAs never adds throughput at any N. It only splits the
    same rate.
- **The engine roof per actual SM clock is 4,096 FLOPs (dense FP16/BF16) and 8,192
  (FP8 e4m3), exactly.** It is reached by our kernel and equals Vortex's 4,096.
  - The clock is the actual SM clock: clock64 checked against `%globaltimer`.
  - On tensor-active SMs it is **1.800 GHz while NVML reports 1980 MHz**. This is
    the same offset as in `h200-bystander.md`.
  - The notes' "3,786 per clock" (989.5 TFLOPS / 132 / 1.98 GHz) is therefore the
    wrong normalisation. Use 4,096.
- **What bounds the small-N end: shared-memory read rate for SS, and a fixed
  per-instruction cost for RS.**
  - SS costs exactly (2,048 + 32 N) / 128 cycles per wgmma: 18 / 20 / 24 / 32 cycles
    at N = 8 / 16 / 32 / 64. That is **128 B per clock** of A + B operand reads in
    every SS cell up to N = 64, and the same with 16 warpgroups.
  - RS costs max(13, N/2) cycles per wgmma: **13 cycles minimum per instruction**,
    flat at N = 8 and 16, with B read at only 20-39 B per clock. So it is not
    bandwidth.
  - Above that, the TC needs N/2 cycles per m64nNk16 (4,096 FLOPs per clock).
- **Issue interval with ONE wgmma per commit group** (`commit` + `wait_group 1`
  after each wgmma, G = 1):

  | | N = 8 | 16 | 32 | 64 | 128 | 256 |
  |---|---|---|---|---|---|---|
  | cycles per wgmma, SS | 38.3 | 40.3 | 44.8 | 51.5 | 70.5 | 128 |
  | cycles per wgmma, RS | 33.3 | 35.3 | 39.3 | 46.0 | 64.0 | 128 |

  - The pattern is ~22 cycles + the SS operand time for SS, ~31 + N/4 for RS (INF).
    It is a latency round trip, not issue: adding 4 or 16 independent ALU
    instructions per iteration leaves N = 8 SS unchanged (38.25 / 38.25 / 38.0
    cycles).
  - A one-WG, one-op-per-iteration loop therefore gets 2,545 FLOPs per clock at
    N = 64 and 3,718 at N = 128. Two warpgroups (or >= 2 wgmmas per group)
    restore 4,096.
- **Production GEMM roof, same session (MEAS):**
  - CUTLASS on ONE SM: 3,578 FLOPs per in-kernel clock (6.44 TFLOP/s at 1.799 GHz),
    = 87% of 4,096.
  - cuBLASLt on one TPC: 7.02 TFLOP/s per SM = 3,902 FLOPs per clock at the 1.80 GHz
    in-kernel clock (INF: clock not stamped for cuBLAS; 3,547 at NVML's 1980), = 95%.
  - All 132 SMs: cuBLAS 688-714 TFLOP/s and CUTLASS 620-627, both power-capped
    (reason 0x4, NVML 1.45-1.62 GHz, CUTLASS in-kernel 1.287 GHz). CUTLASS = 3,648-3,691
    FLOPs per in-kernel clock.

## 1. How a production Hopper GEMM sits on an SM (MEAS: ncu LaunchStats, SASS, `%smid`)

| | CUTLASS 3 (v4.5) 128x256x64 warp-specialised cooperative | cuBLASLt 13.4 heuristic choice, 8192^3 |
|---|---|---|
| kernel | `KernelTmaWarpSpecializedCooperative`, cluster 1x1x1 (ours) | `nvjet_{tst,hsh}_320x128_64x3_1x2_h_bz_coopB_TNT`, cluster 2x1 |
| CTAs per SM | 1 (persistent; grid = SMs; `%smid` read back: 1 CTA on SM 0 in the 1-SM run) | 1 (grid 132; occupancy limit 1 by registers and by shared memory) |
| threads per CTA | 384 = 3 warpgroups: 1 producer (TMA) + **2 consumer WGs issuing wgmma** | 384 (3 warpgroups); "coop" = 2 consumer WGs (INF from name and count) |
| registers | 168 at launch; `setmaxnreg` 40 (producer) / 232 (consumers) (SASS `USETMAXREG` 0x28 / 0xe8) | 168 per thread at launch |
| shared memory | 230.4 KB dynamic, 4 stages of 128x64 A + 256x64 B | 188.6 KB dynamic, 3 stages |
| wgmma | `HGMMA.64x256x16.F32.BF16` (SS), 4 per K = 64 k-tile per WG; ncu: 8,388,608 warp-level gmma = 2,097,152 WG-level = exactly 2·8192^3 / (2·64·256·16) | 34,078,720 warp-level gmma = 131,072 FLOPs per WG instruction (with padding to 320-row tiles) = a 64x64x16 equivalent; the shape was not read from SASS |

So the production condition is:
- ONE CTA per SM;
- TWO consumer warpgroups issuing SS wgmma, 4 per commit group (K = 64),
  `wait_group 1`;
- one producer warpgroup issuing no wgmma;
- N = 256 per wgmma for CUTLASS.

In our microbenchmark that condition (2 WG, SS, G = 4) runs at the 4,096 roof for
every N >= 64. The production kernels reach 87-95% of the roof: the rest is
mainloop and epilogue overhead, not the TC.

## 2. Comparison guide for Vortex `tmem_roof` (h200s, 1 SM, one CTA, 64 x N x 16, roof 4,096)

| Vortex mode | H200 row to compare with | H200 FLOPs/clk N = 64 / 128 / 256 | Vortex FLOPs/clk N = 64 / 128 (512) |
|---|---|---|---|
| one warp looping, one op per iteration | 1 WG, G = 1 (commit + wait_group 1 per wgmma), SS | **2,545 / 3,718 / 4,096** (51.5 / 70.5 / 128 cycles per op) | 1,185 / 2,369 (3,704 at N = 512); ~110 / ~111 cycles per op |
| one warp unrolled x4, descriptors in registers | 1 WG, G = 4, SS (descriptors in registers here too) | **4,096 / 4,096 / 4,096** (32 / 64 / 128 cycles per op) | 2,217 / 2,884 |
| every warp of the CTA issuing a share | 2 WG in one CTA, G = 4, SS (= CUTLASS's 2 consumer WGs) | **4,096 / 4,096 / 4,096** | 1,789 / 2,619 |
| several CTAs on the SM (no production kernel does this) | k CTAs x 1 WG, G = 4, SS (`h200_wgw_cta_f16.txt`) | 4,096 for 2-8 CTAs at N = 64; 2-4 at N = 128 | - |

Reading:
- On the GPU, the unrolled and multi-issuer modes are at the engine roof for
  N >= 64, so Vortex's 2,217-2,884 and 1,789-2,619 are 54-70% of what the same
  arrangement gets on H200.
- Vortex's all-warps mode being *lower* than one warp unrolled has no H200
  counterpart: on H200 more issuers never lower the rate.
- Vortex's lone-issuer interval of ~110 cycles per op at N = 64 compares with
  51.5 on H200 when the H200 kernel also waits for the previous op every
  iteration, and with 32 (= the TC's own N/2) when it does not.
- Per-op operand cost on H200 is 2,048 + 32 N bytes at 128 B per clock (SS). At
  N = 64 that takes 32 cycles, the same as the TC time. So 128 B per clock of
  shared memory is exactly enough for 64 x 64 x 16 SS at the 4,096 roof, and
  insufficient below N = 64.

## 3. Luo et al. (arXiv 2501.12084) Table 9, H800, m64nNk16 FP16

Ratio to N = 256 (clock-free):

| N | Luo SS | ours SS | Luo RS | ours RS |
|---|---|---|---|---|
| 8 | 0.217 | 0.222 | 0.297 | 0.308 |
| 16 | 0.394 | 0.400 | 0.596 | 0.615 |
| 32 | 0.655 | 0.667 | 0.975 | 1.000 |
| 64 | 0.988 | 1.000 | 0.988 | 1.000 |

- The shapes agree within 2-3.5% at every point, so the same two bounds apply: SS
  at 128 B per clock, and RS at 13 cycles per instruction.
- INF:
  - 728.5 TFLOPS / 132 SMs = 5.52 TFLOP/s per SM = 4,096 FLOPs x 1.35 GHz. So their
    table is an all-SM, power-capped run (our all-SM CUTLASS: 1.29 GHz in-kernel,
    reason 0x4), not a lower per-clock rate.
  - Issuers per SM cannot be read from the shape if their kernel issued >= 2-4
    wgmma per commit group, because then 1 WG already gives this curve.
  - If it committed every wgmma (G = 1), 1-2 WG would not match (G = 1, 2 WG: RS
    N = 32 = 0.80 vs their 0.975), and >= 4 WGs per SM would.
  - So the table's kernel had either >= 2 wgmmas in flight per warpgroup, or
    >= 4 warpgroups per SM.

## 4. Method

- Kernel `gpu/wgw_lab.cu`, wrappers generated by `gpu/wgw_gen.py` -> `gpu/wgw_mma.h`.
- Each issuing WG loops: `wgmma.fence`; G x `wgmma.mma_async` (k-steps cycle over a
  128-byte K tile); `commit_group`; `wait_group 1` (cad 0).
- Accumulator: FP32 registers, scale-d = 1.
- Operands:
  - A: 64 x 128 B per WG, 128-byte swizzle, K-major (the no-swizzle layout gives
    identical cycles in a smoke run, `h200_wgw_smoke_none.txt`);
  - B: N x 128 B shared by the CTA;
  - RS A: 4 x .b32 per thread per k-step, loaded once.
- No other memory traffic in the loop.
- SASS (`gpu/wgw_sass.py`, 82 kernels, 0 flagged): exactly G HGMMA per
  iteration, one WARPGROUP.ARRIVE and one DEPBAR.
  - G = 4 SS: 26 instructions per iteration (4 HGMMA, 16 UMOV that move
    descriptors into uniform registers, loop control).
  - G = 1: 7-8 per iteration.
  - No ptxas serialisation warnings.
- Placement: sticky TPC mask of bit 0, grid = 2C, the first C CTAs per SM are kept
  (`%smid` claim). Only SMs holding exactly C CTAs are reported (the slower of the
  two SMs).
- Window: >= 0.5 s per trial (iterations sized from a calibration run), 1 warm-up +
  5 trials.
- Clock:
  - FLOPs per clock = FLOPs / (clock64 span of the SM);
  - TFLOP/s by `%globaltimer`;
  - NVML sampled every 10 ms: 1980 MHz, no reason, 125-130 W in every lab run.
- Correctness: every configuration first ran 3 iterations and compared every
  WG's 64 x N accumulator exactly with a host reference. Operands are multiples
  of 1/4, exact in FP16, BF16 and E4M3. All passed (also BF16 and E4M3, SS and RS,
  1 and 4 WG, in `h200_wgw_smoke.txt`).
- Residency is register-limited: SS N = 256 at 160 registers allows 3 WG; RS
  N = 256 at 176 allows 2.
- GEMM driver `gpu/wgw_gemm.cu`:
  - cuBLASLt TN, FP32 compute;
  - CUTLASS `gpu/wgw_cutlass_kind.cu`, cluster 1, with entry/exit clock64 +
    `%globaltimer` per CTA for the in-kernel clock;
  - events over >= 0.5 s, 5 trials, 64 sampled outputs checked (all ok).
- BF16 and E4M3 (smoke, 0.05 s windows, 1 trial, MEAS):
  - BF16 is identical to FP16 cycle for cycle;
  - E4M3 m64nNk32 has the same cycles per instruction, so it gives twice the
    FLOPs: 1,820 / 3,277 / 5,461 / 8,192 SS at N = 8 / 16 / 32 >= 64 with 1 WG.

## 5. Files

Sources (`gpu/`): `wgw_gen.py`, `wgw_mma.h`, `wgw_lab.cu`, `wgw_sass.py`, `wgw_summary.py`,
`wgw_gemm.cu`, `wgw_cutlass_kind.cu`, `wgw_build.sh`, `wgw_holder.sbatch`, `wgw_srun.sh`,
`wgw_run.sh`, `wgw_gemm_runs.sh`. Binaries only in `/tmp/$USER/wgw` on the node.

Raw (`results_gpu/`):
- `h200_wgw_main_f16.txt` (SS/RS issuer sweep, G = 4);
- `h200_wgw_g1_f16.txt` (G = 1);
- `h200_wgw_cta_f16.txt` (k CTAs x 1 WG);
- `h200_wgw_t3.txt` (INCOMPLETE: 3 points, stopped by the scope change);
- `h200_wgw_smoke.txt`, `h200_wgw_smoke_none.txt` (all types, short windows);
- `h200_wgw_gemm.txt`, `h200_wgw_ncu.txt`;
- `h200_wgw_map.txt`, `h200_wgw_probe.txt`, `h200_wgw_holder.out`.
