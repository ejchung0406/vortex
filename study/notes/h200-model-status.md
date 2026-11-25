# H200 model status (2026-11-25)

For someone about to run experiments on the calibrated Vortex model. Work branch `feature/dxa-borrowing`. Numbers name their source; "findings N" means `upstream-pr-candidates.md` item N. Measured and inferred statements are kept apart.

## Profiles and how to run a point
- **`h200s`**: one sixth of an H200 (22 SMs, one HBM3e stack, 800 GB/s, a sixth of the L2). Every per-SM study number below is one SM of this profile.
- **`h200g2`**: 32 SMs in two groups of 16, 32 L2 slices each bound to one channel of two HBM3e stacks (1.6 TB/s nominal, study roof 1,544 GB/s), one L2 shared by both groups, no partitions or fabric (findings 68).
- **Run a point** with the scripts in `vortex-exp/`: `run_point.sbatch` (Phoenix; variables NAME APP ARGS PROFILE CORES XCONF PERF DRIVER), `ice/ice_submit.sh` (same variables, on ICE; `ice/ice_fetch.sh` brings the result back), `rover/rover_start.sh` (the lab server; same variables plus SRC and MODEL; `rover_status.sh` lists points and cycles; full-size rtlsim runs there, inside a 200 GB memory slice, one build at a time). The public copies are `study/scripts/run_point.sbatch` and `study/ice/`.
- **PROFILE must be in the environment before `configure`** (and `make software`); the scripts do this. `blackbox --profile` alone leaves headers and the kernel library at defaults (about 7% different cycles; findings 37, 38).
- **`VORTEX_VM_MEGAPAGE_HEAP=1`** (the scripts set it by default): with 4 KB pages the study's data sets miss the TLB and the run measures page walks.

## Kernel class: model against the real H200, and SimX against rtlsim
SimX numbers at the current head are the lab-server points `hn_*` (profile as committed) read on 2026-11-25. "Gap" is (rtlsim - SimX) / rtlsim unless noted; positive means SimX is faster.

| Kernel class | Model | Real H200 | Model / part | SimX vs rtlsim gap |
|---|---|---|---|---|
| Tensor-core roof (one SM) | 3,684 FLOPs/clock (findings 17; `tmem_roof` N=512 3,704) | 4,096 per actual SM clock (M2) | 90% | within 1% (findings 17) |
| Shared-memory 4-byte loads / stores, 64 warps | 0.98 per clock each; one warp 4.88 / 4.80 cycles (findings 61) | 1.000 per clock; one warp 4.98-5.05 / 4.06-4.30 (M4) | 98% | `smem_rate -k14` 139,078 / 139,069: 0.01%; `-k13` 139,755 / 139,576: 0.13% (rtlsim at d353c1c33) |
| Shared-memory store/load mix | row shared; neither starves (findings 61) | stores + loads sum to 1.000 per clock (M4) | split not compared with the part's mixes (h200s sources, `LMEM_RD_ROW`) | `-k22` 140,238 / 139,931: 0.22% (rtlsim at cece299d3) |
| Plain-load stream, lean (`ldstream -t2048 -b8 -u8 -m16 -a -l`) | 194.4 GB/s on one SM (`noc-step2.md`) | 239 GB/s per SM with 16-byte loads; per-SM ingress 215-230 (`tpc-placement-and-borrowing.md`, findings 21) | 81-90% | 170,629 / 173,098: -1.4% (rtlsim at 00186b8b3; `open-threads.md` 14:10) |
| Plain-load stream, instruction-bound (`bwstream -n2048 -u4 -b32`, 4-byte loads) | 97.7 GB/s on one SM (`h200g2` sweep) | 110 GB/s per SM with 4-byte loads (`tpc-placement-and-borrowing.md` C.1) | 89% | about 395,000 / 394,065: about 0.2% (rtlsim n=2048 at 5f855c2ae; findings 51, 63) |
| TMA copy, several transfers (`bwstream_dxa -e4096 -d2/-d4`, one SM) | 86.95 / 79.70 B/clock (`hn_dx7`, `hn_dx3`); `-n32 -d2` 81.1 | 105-108 B/clock (M4) | 74-83% | `-n32 -d2` 48,702 / 45,248: 7.1% (findings 66; cause open) |
| TMA copy, one transfer | 0.377 lines/clock slope with LPC (`tma-rate.md`) | 1.67 lines/clock (M4) | 23% | one CTA 14,078 / 14,059: 0.13% (`tma-rate.md` 22:26) |
| `gemm_block -m16 -n1024 -k4096 -s1` (DXA-staged decode GEMM) | 443,681 cycles, 18.9 B/clock of weights (`hn_gb`) | no like-for-like measurement in the notes | - | 472,826 / 443,681: 6.2% (findings 66; cause open) |
| Decode kernel `gemm_tmem_decode -m16 -n8192 -k4096` | 1,150,323 cycles (`hn_dec`); 58.9 B/clock/SM in the multiply (`tma-rate.md` B) | the part's tensor bound for this kernel, 58 B/clock (findings 44; derived, not a run of this kernel) | about 100% of that bound | `-k1024` 319,643 / 314,040: 1.8% at d353c1c33, before the overlap knobs; not re-taken |
| Multi-SM bandwidth (`h200g2`, spread) | peak 1,315.6 GB/s lean (10 SMs), 1,359.7 TMA (16); 32 SMs 6.1% / 6.3% lower (findings 69) | 96-97% of nominal from 32 SMs on, flat to 132 (`tpc-placement-and-borrowing.md` C.1) | peak 85-88% of 1,544; roll-off absent on the part | SimX only; small NoC parity cases within 2.3% (findings 65, 68) |
| SM fraction that fills memory (98% of own peak) | lean 8 of 32 (25%); TMA 16 of 32 (50%) (findings 68) | 17% (design §4.2) | 1.5-3x the part's fraction | SimX only |
| Packed against spread, 8 SMs | lean 1,087 / 1,302 GB/s; TMA 1,142 / 1,170 (findings 68) | one GPC 1,274-1,277 against 1,611-1,893 spread (C.1) | packed point 15% below the part's GPC limit; spread not comparable (model at its roof) | SimX only |

## What the model cannot be trusted for
- **Behaviour at the rate limits.** The SM ingress limiter and the group return gate are token buckets set to measured rates; where requests wait and which flow waits behind which at those limits comes from where the gate was placed (findings 71). Interference and fairness results at a limit need an independent check.
- **Partitions and the fabric are absent.** `h200g2` has one L2 shared by both groups, no partition hash, no near/far latency, no replication; L2 per SM is 640 KiB against about 465 KiB.
- **Bandwidth with many memory-bound SMs.** The peak is 85-88% of the target and falls about 6% toward 32 SMs where the part stays flat (findings 69, left as a known limit on 2026-11-25).
- **A single TMA transfer.** One transfer is one worker's; 0.38 lines per clock against 1.67. Latency-bound TMA results (few CTAs, one transfer in flight) are too slow.
- **Tensor-unit latency.** Simulation uses a TCU latency of 4; the synthesis flows use 5 (findings 70).
- **One kernel at a time.** Two co-running workloads are emulated by one kernel whose CTAs choose their role by core id (`gemm_block`: a CTA on a core below `bw_cores` takes the memory-bound role), not by two launches sharing the machine.
- **DXA-heavy kernels in SimX against rtlsim.** SimX is 6-7% faster than rtlsim on the multi-CTA copy and `gemm_block` (findings 66); use rtlsim when that margin matters.

## Open items, in priority order
1. The remaining SimX/rtlsim gap on DXA-heavy kernels (copy 7.1%, `gemm_block` 6.2%); first difference seen at warp admission before the DXA queue (findings 66).
2. One TMA transfer at 0.38 of 1.67 lines per clock, and the multi-transfer copy at 80-87 of 105-108 B/clock: line-granular scheduling of a transfer over both line paths, not started (findings 64, 67).
3. `h200g2` step-2 leftovers: the 48-channel remap, the deeper L2 egress paths, L1 and `h200s` L2 latency outside the profile check's 10% (findings 68).
4. Partitions and fabric (NoC step 3), needed before any claim about cross-partition traffic.
5. Smaller SimX/rtlsim residues: dcache without VM at 16 warps (-12.4%, findings 63), 64-warp stack replay 5.3% (findings 62), the decode-kernel rtlsim reference at the current settings.
6. Logic depth and area of the merged knobs; synthesis timing of the NoC.
