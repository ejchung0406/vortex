# H200 go/no-go experiments A, B, C (2026-11-21)

Real NVIDIA H200, node atl1-1-02-014-16-0 (driver 615.71.09, toolkit 12.9.1,
PyTorch 2.15.0+cu132 with cuBLAS 13.4.1), holder job 15978502 `gng_h200`
(17:42-20:12). TPC mask map re-probed on this die (`results_gpu/h200_gng_map.txt`,
66 live bits, 0 mismatches; identical to `h200_l2s_map.txt` and `h200_tpc_map.txt`);
GPC layout from `h200_l2s_gpc.txt` (same die): GPC0 = bits 0-3 + 75, 78, 81, 83;
GPC1-5 = 8 TPCs each (bits 9-16, 18-25, 27-34, 36-43, 45-52); GPC6-7 = 9 TPCs
(54-62, 63-71).

Tags: MEAS = measured here (file named); INF = inference; UNV = not verified.

## 0. Answers

PLACEHOLDER_ANSWERS

## 1. Experiment A: SM sharing (X0)

PLACEHOLDER_A

## 2. Experiment B: bypassing shared memory

PLACEHOLDER_B

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

PLACEHOLDER_C_REMEASURE

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

PLACEHOLDER_GAPS

## 6. Corrections to earlier notes

PLACEHOLDER_CORR

## 7. Files

PLACEHOLDER_FILES
