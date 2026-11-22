# Sharing an SM between a memory-bound and a compute-bound job without starving the memory job

Study written 2026-11-21. Read-only: no code edits, no simulations and no GPU jobs were run for it.
Inputs:
- our real-H200 runs: `results_gpu/h200_borrow_e3*.txt`, `h200_borrow_e4_*.txt`,
  `h200_tpc_coresid*.txt`, `h200_tpc_chip.txt`, `h200_warprate_summary.txt`;
- notes `tpc-placement-and-borrowing.md`, `related-work-placement.md`, `open-threads.md`,
  `corun-workloads.md`, `noc-and-design-space.md`;
- a code survey of the Vortex RTL and SimX (branch `feature/dxa-borrowing`, file:line cited);
- a literature survey (§2, URLs given).

Tags:
- **MEAS**: measured by us on the H200 (file named);
- **LIT**: published (URL);
- **CODE**: read in the Vortex source (file:line, paths relative to the repo root);
- **INF**: my inference or arithmetic, with what it rests on;
- **UNV**: unverified.

Companion file: `sm-sharing-figures.md`, one diagram specification per mechanism plus the starvation anatomy.

---

## 0. Answer first

**Scope and status.**
- Everything below concerns two *distinct* phenomena. Earlier notes merged them. A critique (`framing-critique.md`) found the mixing in the raw logs.
  - **(A) Block-scheduler interleaving.** The two jobs cannot fit on one SM, so whole SMs alternate between them.
    - Beside the cuBLAS GEMM, the 65 KB TMA blocks never share an SM with a GEMM CTA (a 189 KB GEMM CTA plus 65 KB > 228 KB).
    - Memory keeps 0.63-0.78. The GEMM falls to 0.25 at 132 memory blocks (757 -> 190 TFLOP/s, `mem_distinct_sms_co` 43-66), and keeps 0.89-0.99 at 16 blocks (memory 0.66-0.71) (MEAS `h200_borrow_e3.txt`, GEMM rows).
    - This is a *residency and placement* effect, not starvation.
  - **(B) Warp-level starvation on a shared SM.** Measured **only beside a synthetic FP32 FMA kernel**.
- **No co-resident run beside a production tensor-core kernel exists.** Whether (B) happens beside one is the open go/no-go question, **X0** in §1.4.

**1. Phenomenon (B) beside the FMA kernel is an issue-arbitration effect.** Five signatures point at the warp scheduler; none fits a bandwidth or capacity resource.
- MEM falls to 0.6-6.7% while CMP keeps 0.985-1.000 (MEAS, coresid). CMP is register-only and saturates nothing but issue.
- MEM is *stopped*, not slowed. With a fixed-length CMP, MEM made almost no progress until CMP ended.
- The result depends on launch order and on compute ILP.
- A one-thread TMA issuer runs at 11.1 GB/s per SM whatever its depth (D4 = D8, e3b). That is one 16 KB transfer per ~2,900 cycles: bound by how often it gets to issue.
- With CMP first at ILP 4, MEM's rate × the number of FMA warps is roughly constant (487-605) (INF: a per-warp share of switch events).
- Caveat: the order dependence we measured (MEM wins when launched *first*) contradicts the published Ampere/Turing policy, "greedy then **youngest**" (Huerta et al., §2.1). The policy is not yet understood (X1).

**2. Shared memory and the LSU are a second contention, measured in the other direction.**
- Neighbours streaming plain loads slow a shared-memory probe ~12× (121 -> ~1,500 cycles per iteration). A barrier-only probe barely moves (71 -> 79) (e4_probe2).
- A plain-load stream costs a co-resident FMA kernel 20-37%; a TMA stream costs it 3-6% (e3).
- Whether TMA landing writes slow shared memory is untested (X5).

**3. Residency (A) is the precondition, and production GEMMs leave almost none.**
- The cuBLAS FP16 GEMM uses 64,512 of 65,536 registers and 189 KB of shared memory. That leaves 1,024 registers (one warp at 32 registers) and ~38 KB.
- A one-warp lane with a ring of 32 KB or less would fit *today*. Nobody has run it.

**4. Little's law sets what the memory job needs per SM.** A memory job on every SM needs 4.63 TB/s / 132 = **35.1 GB/s = 17.7 B/clock per SM**. Per SM (INF, §3.0):
- issue: ~0.7% (TMA producer, 32 KB transfers), ~3% with a decode consumer;
- tensor core: 4-8% of the time (FP16, M = 8-16);
- shared-memory port: 14% (+14% if staged and re-read);
- **buffer: 35-70 KB in flight**, the one large item.

So the tenant is asymmetric: almost no issue, but a *timely* guaranteed share and a latency-sized buffer.

**This holds only for small decode batches.**
- Tensor-core need ≈ 0.62·B SM-equivalents at 4.63 TB/s (B = batch, FP16): 4.63 TB/s × B FLOP per byte / 7.5 TFLOP/s per SM.
- B = 16: ~10 SM-equivalents = 7.5% of every SM. B = 32: 15%. **B = 64: ~39 SMs**, so decode is then itself a compute tenant.
- **A bandwidth-only tenant exists for B ≲ 16** (≲ 32 if the lender's tensor core has idle time).

**5. Honest headroom.** "Compute share" below is the compute job's throughput relative to its solo rate on all 132 SMs.
- **Measured so far:** sharing beats disjoint by about **+8 to +9 points** of compute share at equal memory rate.
  - Sharing: 4.33 TB/s at 0.86 (FMA lender, MEM first, ILP 2: luck-dependent).
  - Disjoint at 4.33 TB/s: ~0.77-0.78, interpolated between 24 SMs (4.03 / 0.82) and 32 (4.50 / 0.76).
- **Ideal ceiling** (lane free, lender loses only R8): ~0.97 at 4.5 TB/s against 0.76, i.e. +21 points (+28% relative).
- **But for an SM-insensitive lender the baseline itself costs little.** The real Z-Image DiT ran at **1.01 of its speed on "80%" and 0.90 on "70%" of the SMs** (MPS, `corun-workloads.md`; MPS percentages map to SM counts only approximately).
  - If that holds, giving the decode job 24-32 disjoint SMs costs the DiT ~0-10%, not 18-24%.
  - **The measured headroom for that pair could be ≤ 10 points.**
  - The headroom is real only for lenders whose throughput is proportional to SMs (large dense GEMMs, training). §3 gives each mechanism's headroom on this basis.

**6. Recommended mechanism, if X0 says go.** A "memory lane" of three parts:
- **(a)** a bounded issue-share floor for lane warps (a per-scheduler token bucket);
- **(c)** an autonomous per-SM stream engine driven by a descriptor ring with credits, so the fetch needs no issue;
- **(g1)** two-stage fetch: L2 prefetch far ahead, then a just-in-time pull. This cuts the SM-side ring to ~27-34 KB, which fits beside a production GEMM.

Notes on the rest:
- Shared-memory and LSU isolation (f) protects the lender.
- Remote fetch (d) is ingest-bound (saves 3-9 SMs).
- Context-switched slices (e1) are ruled out by arithmetic.
- Resident micro-slices (e2) are a cheap form of (a).

**7. Go/no-go first (X0, §1.4).** A lane-sized decode kernel co-resident with a DiT-shaped production wgmma GEMM, both launch orders, same process and MPS. Outcomes:
- **Kill:** if the lane keeps ≥ 95% of its solo rate and the GEMM ≥ 0.97 in all four configurations, hardware isolation is not needed. The work becomes a software and placement paper.
- **Go for (a):** if the lane starves with "not selected" as its dominant stall reason.
- **Go for (b)/(g1):** if it cannot become resident.
- **Go for (f):** if it runs but the GEMM loses > 5%.

Only after a "go": calibrate SimX until it *reproduces* the H200 fingerprint (Vortex cannot today, §4.1). Then prototype (a) and (c)+(g1) on `h200s`, 22 cores (fair share 32.5 GB/s per core, the same 17.7 B/clock as the part).

**8. Framing (§5).** "Compute and memory bandwidth cannot be allocated separately on today's GPUs."
- MIG ties memory slices to GPC slices.
- MPS and Green Contexts allocate SMs, and bandwidth then follows the SM count (per-SM ingest, per-GPC egress).
- A memory lane is the missing knob that lets a scheduler give one tenant the bandwidth and the other the SMs.
- Deployment cases a reviewer accepts:
  - prefill + decode in one serving engine (decode B ≤ 16 per model replica);
  - RL rollout (decode) beside training;
  - multi-model serving with a cold, small-batch model beside a busy one.

---

## 1. Anatomy of the starvation

### 1.1 Numbers used throughout

| quantity | value | tag |
|---|---|---|
| SM clock | 1.98 GHz (no throttle reasons active in the chip runs) | MEAS |
| issue | 4 schedulers (SMSPs) per SM, 1 warp-instruction per clock each, so 7.92 G/s per SM | MEAS, `h200_warprate_summary.txt`: one FMA warp with ILP 4 gives 0.94 IPC; 4 warps give 3.74; SM peak 4.00 |
| one warp's own rate | FMA ILP 4: 1.07 cycles/instr; dependent FMA: 3.96; dependent integer: 4.84; one TMA producer loop of 49 SASS instructions: 5.91 cycles/instr (289.5 cycles/iteration) | MEAS warprate |
| shared-memory read bandwidth | 251-253 GB/s = 127 B/clock per SM | MEAS E2 |
| shared-memory writes and reads | partly shared (staged-then-consumed reaches 157-170 GB/s) | MEAS E2 |
| SM ingest ceiling | 205-219 GB/s TMA (32 KB × 6, one thread, 100 ns backoff), 239 GB/s plain 16 B loads | MEAS |
| GPC egress | ~1.28 TB/s from DRAM | MEAS |
| chip roof | 4.63 TB/s | MEAS |
| fair share with all 132 SMs | 35.1 GB/s = 17.7 B/clock per SM | INF |
| register file | 65,536 × 32-bit per SM; max 64 warps, 228 KB shared memory per SM (227 KB per block) | LIT (CUDA guide), consistent with MEAS O7 |
| cuBLAS FP16 GEMM `nvjet_hsh_320x128_64x3_1x2_h_bz_coopB_NNT` | 384 threads × 168 registers = 64,512; 0.189 MB shared memory; grid 2×66 | MEAS `h200_borrow_e3_gemm_kernel.txt` |
| tensor core per SM | 989.5 TF / 132 = 7.50 TFLOP/s FP16 dense; FP8 2× | LIT (spec) |

### 1.2 Candidate contended resources

| # | resource inside the SM | how it would hurt the memory job (MEM) | signature it predicts |
|---|---|---|---|
| R1 | **issue slot and warp-scheduler policy** (per SMSP: greedy, oldest/youngest fallback, yield) | MEM's producer or consumer warp is rarely selected | rate set by how often MEM is picked: independent of TMA depth and transfer count in flight, sensitive to compute ILP, warp count and launch order (age); compute unaffected |
| R2 | **MIO / LSU input queues** (LDS/LDSM/STS/bulk-copy/mbarrier through MIO; LDG through the LG queue) | MEM's mbarrier, try_wait or TMA instructions wait behind the lender's queued memory instructions | lender with many memory instructions in flight hurts; a register-only lender does not; stall reason `mio_throttle` / `lg_throttle` |
| R3 | **L1 / shared-memory data path and banks** (128 B/clock) | landing writes or consumer reads compete with the lender's LDS or wgmma operand reads | slowdown proportional to the lender's shared-memory traffic; a register-only lender does not hurt |
| R4 | **the SM's L2 ingress** (~110 B/clock) | MEM shares the return path with the lender's own loads | bandwidth shared proportionally, never to ~0; independent of compute ILP |
| R5 | **the TMA engine's issue path** (one engine per SM; per-descriptor setup) | the lender's own TMA descriptors queue ahead of MEM's | matters only beside a TMA-using lender; rate per transfer, not per byte |
| R6 | **residency**: registers, shared memory, warp and CTA slots | MEM cannot be resident at all | 0 CTAs placed (`%smid`), not a rate effect |
| R7 | **barrier / mbarrier / polling units** | MEM's try_wait loop is slow, or its polling slows the lender | depends on polling style (tight vs nanosleep), MEAS 205 vs 140 GB/s from a code-generation difference |
| R8 | outside the SM: GPC egress, L2, DRAM, and the lender's own traffic | shared memory-system bandwidth | proportional sharing; seen beside CUTLASS (4.49 -> 3.7 TB/s) |

### 1.3 What the existing measurements already discriminate

The lender in all of the following is the FP32 FMA kernel: register-only, 1024 threads, 4 independent chains unless stated.

1. **The lender keeps its rate while MEM goes to ~0** (coresid, CMP first, ILP 4: CMP 0.985-1.000, MEM 0.6-6.7%).
   - The FMA kernel uses no shared memory, no LSU, no L2 and no TMA, so R2-R5 and R8 cannot be what it saturates.
   - The only resource the FMA kernel saturates is issue: SM IPC 4.00 at 8+ warps (warprate).
   - **R1 is the cause in the FMA case.** Strong.
2. **"Starved, not slowed."** With a fixed-length CMP, MEM showed 1.1 GB/s per SM over a 470 ms window equal to CMP's duration (coresid).
   - Shared-bandwidth resources (R3, R4, R8) give proportional sharing, not ~0. This is consistent with a priority-based selector.
3. **Launch order and ILP.** Corrected from the raw file `h200_tpc_coresid2.txt`. Cells give MEM GB/s on 16 SMs / CMP ratio; the GPC1 cap is ~1,284.
   - The table in `tpc-placement-and-borrowing.md` §C.2 has an ILP-1 column with **no raw run behind it**, and its 32-warp ILP-4 MEM-first and ILP-2 CMP-first cells (20.5, 38.9) do not match the raw file (12.1, 25.0). Do not cite it.
   - The ILP-4 CMP-first row is from `h200_tpc_coresid.txt`, whose runs carry no order or ILP fields. The notes call it CMP first, ILP 4.

   | FMA warps | ILP 4, CMP first | ILP 4, MEM first | ILP 2, CMP first | ILP 2, MEM first |
   |---|---|---|---|---|
   | 8 | 60.9 / 0.999 | 40.8 / 1.000 | **1283 / 0.920** | 1283 / 0.884 |
   | 16 | 37.8 / 0.998 | 60.0 / 0.990 | 22.7 / 0.999 | 1283 / 0.878 |
   | 24 | 24.5 / 0.999 | 42.6 / 1.002 | 35.1 / 1.000 | 1283 / 0.891 |
   | 32 | 16.4 / 1.000 | 12.1 / 1.000 | 25.0 / 1.003 | 1284 / 0.890 |

   - ILP 4 starves MEM in *both* orders.
   - At ILP 2 the order decides it, except at 8 FMA warps (2 per scheduler), where MEM survives even when launched last.
   - INF: two kinds of switch event.
     - (i) The greedy warp is *not ready* (dependency stall). Some age-like order picks the next warp, and MEM wins when launched first.
     - (ii) At ILP 4 the greedy warp is almost always ready, so only forced switches (branch, Yield bit, instruction-cache miss) occur, and they do not favour MEM in either order.
   - Textbook greedy-then-oldest predicts that MEM first at ILP 4 gets the ~6% of cycles in which an FMA warp is not ready (0.94 IPC), ~119 M slots/s, far more than it needs. It does not.
   - The published CGGTY policy (§2.1, "greedy then youngest", measured on Turing and Ampere) predicts the *opposite* order dependence.
   - **So the H200 policy is unexplained.** X1 is designed to separate launch age from warp-slot order.
4. **Rate ∝ 1 / (number of compute warps) at ILP 4** (coresid W, CMP first): 60.9, 37.8, 24.5, 16.4 GB/s at 8, 16, 24, 32 FMA warps. Rate × warps is 487, 605, 588, 525 (±12%).
   - INF: MEM receives about 1/(n+1) of some fixed number of switch events per second. That is a rotation (round-robin) among warps at forced switches.
   - The plain-load (L16) and staged (S12) columns do not follow the rule. They have many MEM warps and their own LSU traffic.
5. **Depth independence** (e3b, `tmafused`, one thread, 16 KB tiles, FMA lender):
   - MEM co-run = 178.0 GB/s on 16 SMs at D = 4 *and* D = 8; 89.0 on 8 SMs at both depths. That is 11.1 GB/s per SM, while solo rates are 101 (D4) and 142 (D8).
   - Bytes in flight do not matter, so the per-transfer serial cost is what binds.
   - 16 KB / 11.1 GB/s = 1.48 µs = **2,920 cycles per transfer**.
   - With the loop at roughly 49 SASS instructions (the warprate `tma_producer` loop; the fused kernel's exact count is UNV), MEM is issued about **once per 60 cycles**, i.e. ~1.7% of its SMSP's slots.
6. **More instructions per transfer are slower in proportion.** The `test_wait` variant runs at 41.2 vs 60.9 GB/s (0.68×) at 8 warps.
   - That fits an instruction-count bound (R1); an engine or bandwidth bound would not change.
7. **Plain-load MEM hurts the lender, TMA MEM does not** (e3, all 132 SMs):
   - TMA: compute 0.94-0.97;
   - plain loads: 0.63-0.80.
   - A plain-load stream needs ~200 warp-instructions per KB versus ~3 per KB for TMA (INF from 4 × 4 B loads per thread per instruction vs 49 instructions per 16 KB). So it takes real issue capacity, plus LSU and L1 time.
8. **Shared memory slowed ~12× by neighbours' streaming plain loads** (e4_probe2, 16 CTAs per SM):

   | probe | idle neighbours | neighbours streaming loads | neighbours streaming loads with barriers |
   |---|---|---|---|
   | `smem_only` (cycles per iteration) | 121 | ~1,500 | ~600 |
   | `bar_only` (cycles per iteration) | 71 | 79 | 93 |

   - This is not a barrier effect.
   - INF: the probe's LDS/STS sit behind neighbours' LDG in a shared MIO/L1 data-path queue (R2/R3). Whether this happens with TMA neighbours is unknown (X5).
9. **MEM first at ILP 2: MEM keeps 100%, compute loses 9-14% (FMA), 23-25% beside a 12-warp staged consumer.**
   - A 500 ns poll backoff only reduces the loss to ~9%, and idle MEM warps are not the cause.
   - MEM needs ~1% of issue, so a 10% compute loss is ~10× too large to be MEM's issue share.
   - Candidates (all UNV): an oldest MEM warp grabbing *every* fallback slot while it spins; a scheduler-switch penalty each time it is picked; register-file bank or operand-collector conflicts.
   - This must be explained before claiming that a lane costs the lender only its share (X6).
10. **Block-scheduler interleaving beside cuBLAS (phenomenon A, not B)** (`h200_borrow_e3.txt`, GEMM rows; 8192³ FP16, ~80-90 back-to-back GEMMs).
    - The memory blocks (TMA D2/D4 = 33-65 KB plus 4 consumer warps; or 1024-thread load blocks) never fit beside a GEMM CTA (189 KB, 64,512 registers). The two kernels *alternate on SMs*: a memory block that grabs an SM between GEMM launches holds it, so the next GEMM's 132-CTA wave runs short of SMs.

    | memory blocks | mode | SMs MEM used | MEM ratio | GEMM ratio |
    |---|---|---|---|---|
    | 132 | TMA D4 | 50 | 0.63 | 0.25 |
    | 132 | TMA D2 | 43 | 0.70 | 0.26 |
    | 132 | loads | 66 | 0.78 | 0.25 |
    | 16 | TMA D4 | 7 | 0.71 | 0.98 |
    | 16 | TMA D2 | 6 | 0.66 | 0.99 |
    | 16 | loads | 10 | 0.70 | 0.89 |

    - These rows say **nothing** about warp starvation beside a tensor-core kernel: there was never a shared SM.
    - They do show that today's block scheduler, given two kernels that cannot co-reside, makes the *compute* job the victim (0.25). That is a placement or residency failure in the other direction, and a carve-out (b) or masks avoid it.
11. **Hardware already admits a stream of the right size into a production GEMM's leftovers** (INF): 1,024 registers = 1 warp × 32 registers; ~38 KB shared memory ≥ a 2 × 16 KB ring.

**Verdict.**
- R1 is established for issue-saturating (FMA) lenders only.
- Beside a production tensor-core lender, warp-level starvation is **unmeasured** (X0).
- R2/R3 are established in the reverse direction (plain-load streams hurt shared-memory users) and untested for TMA.
- R6 is the gate for production kernels.
- R4/R5/R8 are not implicated on one SM. R8 is the GEMM's own traffic, seen chip-wide.
- The exact policy behind R1 (which warp gets forced switches) is unknown. It decides which software workarounds exist.

### 1.4 Real-GPU experiments to pin it down

To be run later with per-launch TPC masks (`gpu/tpc_smctrl.h`), `%smid` and `%warpid` read back, and `%globaltimer` / `clock64` timers.

Common rules, tightened after the critique:
- measurement windows of **≥ 1 s**. Earlier bandwidth windows were 3.6-6.7 ms;
- **NVML SM clock, memory clock and power sampled every 10-20 ms during the run**, not read at idle;
- the `%warpid` of every warp is recorded. Its SMSP is taken as `%warpid % 4`, which Huerta et al. read on Turing and Ampere; verify it on the H200;
- use cudaMalloc only, and poll with ≥ 100 ns backoff;
- report both launch orders;
- report the lender's rate and MEM's rate, each against its matched solo on the same SMs;
- for every MEM CTA, record which SM it landed on and whether a lender CTA was resident on that SM. This separates phenomenon A from B.

**X0. GO / NO-GO: a lane-sized decode kernel beside a DiT-shaped production wgmma GEMM.** Run this first; it decides whether the hardware direction exists.
- **Lane kernel** (new, `gpu/lane_decode.cu`):
  - 1 CTA per SM of **1 warp, `__launch_bounds__(32)`, `-maxrregcount 32`** (check with `cuobjdump -res-usage`), **≤ 32 KB dynamic shared memory** (2 × 16 KB ring) plus 2 mbarriers;
  - persistent, launched with 132 CTAs;
  - lane 0 issues `cp.async.bulk` 16 KB weight tiles (BF16, its own disjoint 1 GiB slice per SM; 18 GB in all, Qwen-sized);
  - all 32 lanes then do a real **GEMV/skinny GEMM, M = 1, 4 and 16**, on each tile: `ldmatrix` + `mma.sync.m16n8k16` with activations held in registers (M ≤ 8 uses N = 8 padding), accumulating 16 output rows, written out per 1 MB of weights;
  - checked against a host reference on one slice;
  - `%globaltimer` stamped every 64 tiles, so the rate per 1 ms bin is known.
- **Solo targets:**
  - per-SM rate on 1 SM, on 16 SMs spread, and on 132;
  - the lane must reach ≥ 35 GB/s per SM alone. If it cannot, fix the lane before co-running: ring 2 × 16 KB at one-stage latency may cap it near 32 KB / L, so also try `cp.async.bulk.prefetch.L2` 8-16 tiles ahead (g1).
- **Lender:** a DiT-shaped GEMM of M = 4096 tokens, K = 3840, N = 3840 and 15,360 (QKV/MLP shapes of Z-Image), BF16. Two kernels:
  - (i) cuBLASLt's default `nvjet` kernel;
  - (ii) a CUTLASS 3.x Hopper warp-specialised persistent GEMM (128×256×64, its default stages).
  - Read each kernel's registers per thread × threads and its shared memory (nsys/`cuobjdump`).
  - Whether a 1-warp, 32-register, 33 KB lane fits beside it must be *computed and stated per kernel* before running. If (i) leaves < 33 KB, use (ii) with stages reduced by 1 and record that as the carve-out cost (X4).
  - Also, as an issue-heavy lender, the DiT's own LayerNorm/GELU/modulation kernels (from `gpu/dit_step.py`).
- **Configurations:**
  - lane first / GEMM first × same process (two streams) / two MPS clients (no percentage limit);
  - the GEMM looped for ≥ 1 s of back-to-back launches;
  - TPC masks unset (whole chip) and, as a control, both restricted to the same 16 SMs of one GPC.
- **Readouts:**
  - lane GB/s per SM per 1 ms bin, and its co/solo ratio;
  - the GEMM's TFLOP/s co/solo;
  - the fraction of lane CTAs that share an SM with a GEMM CTA (from `%smid` + GEMM CTA `%smid` written by a CUTLASS epilogue hook, or inferred from timing);
  - NVML clock and power.
- **Stall reasons by PC.**
  - **CUPTI PC sampling** (the `cupti_pcsampling` API, continuous collection; concurrent kernels allowed) during the co-run.
  - Lane PCs: `not_selected` / `selected` vs `mio_throttle` / `lg_throttle` vs `long_scoreboard` / `barrier` / `sleeping`.
  - Backup if CUPTI continuous mode fails under MPS: a **fused** variant (lane and GEMM roles in one kernel by block index, POD-Attention style) under `ncu --set full` with source-level sampling.
- **Decision:**
  - **KILL the hardware-isolation direction** if, in all four order × process configurations with the lane resident beside a GEMM CTA, the lane keeps **≥ 0.95 of its solo rate** and the GEMM keeps **≥ 0.97**. Then sharing already works with a carefully sized lane; the contribution becomes software (a lane-sized kernel plus placement), and that paper should be written instead.
  - **GO, issue (a)/(c)** if the lane falls below 0.5 in any configuration with `not_selected` + `wait` dominating its samples.
  - **GO, residency (b)/(g1)** if the lane cannot be resident beside (i) or (ii) at all, or only with the GEMM losing > 5% from a reduced stage count.
  - **GO, lender protection (f)** if the lane runs at rate but the GEMM loses > 5% with `mio_throttle` or shared-memory wait rising in the GEMM's samples.
  - **Elementwise lenders.** If only the DiT elementwise kernels starve the lane, the hardware case shrinks to "issue-heavy phases". Weigh it by their share of DiT time (26% of CUDA time) and say so.

**X1. Scheduler fingerprint, no memory.** Separates the policy hypotheses.
- Victim: one warp per SM running a heartbeat. 32 independent IADDs then `clock64` into registers; every 256 heartbeats one timestamp goes to global memory, so the victim does almost no memory traffic.
- Aggressor: FMA warps. Knobs:
  - ILP ∈ {1, 2, 4, 8};
  - aggressor warps on the *victim's* SMSP ∈ {0, 1, 2, 4, 8, 15} (place warps by `%warpid`, early-exit the others);
  - instructions per backward branch U ∈ {16, 64, 256, 1024} (unroll);
  - order: victim launched first or last;
  - victim as warp 0 or warp 31 of the *same* CTA as the aggressor. This separates age by launch time from priority by warp slot.
- Measure the victim's instructions per cycle.
- Expected signatures:
  - (a) strict oldest-first fallback + greedy: victim rate ≈ 0 when younger and ≈ (1 - aggressor IPC) when older;
  - (b) loose round-robin: ≈ 1/(n+1) of the SMSP;
  - (c) rotation at forced switches only: ∝ (branches per second) / (n+1), independent of order at ILP 4;
  - (d) a youngest-first fallback (one published reading, §2): order reversed relative to (a).
- **Control:** aggressor warps only on the other 3 SMSPs. The victim must run at its solo rate; otherwise contention is SM-wide (dispatch or MIO), not per scheduler.

**X2. Memory-lane rate model.** Tests whether MEM's rate is "switch events ÷ instructions per transfer".
- MEM: one thread issuing write-only TMA, 16 and 32 KB, D ∈ {2, 4, 8}.
- MEM's loop padded with P ∈ {0, 16, 64} extra independent IADDs.
- Lender: FMA at ILP 4 with U ∈ {16, 64, 256, 1024} and 8/16/32 warps.
- Prediction if R1 with rotation holds: MEM GB/s ≈ k × (lender branch rate per SMSP) / (n_SMSP + 1) × 16 KB / (49 + P).
- Prediction if R5 or R7 binds: independent of U and of P.

**X3. Lender sweep beyond X0** (after a GO).
- The X0 lane beside FlashAttention-3 forward (4096 tokens, head dim 128), the CUTLASS FP8 GEMM, and a training step (forward + backward GEMMs).
- Gives the per-kernel starvation and lender-loss map that the evaluation needs.

**X4. Cost of a residency carve-out to the lender** (no co-run needed).
- CUTLASS Hopper GEMM (FP16, FP8; 128×256 and 256×128 tiles) at stages S and S-1. Also register caps via `setmaxnreg` for the consumer warpgroups, -8 / -16 / -32 registers per thread.
- FA3 with its shared-memory budget reduced by 16/32/64 KB.
- Output: the lender's TFLOP/s as a function of (KB of shared memory, registers) withheld. This is the price list for mechanism (b).

**X5. Shared-memory / LSU path anatomy.** Extends E4 with one kernel and roles chosen by block index.
- The probe (`smem_only`, `lds_own`) runs beside neighbours doing, in turn:
  - (i) plain LDG.128;
  - (ii) TMA bulk copies into their own shared memory at matched GB/s;
  - (iii) `cp.async` (LDGSTS);
  - (iv) L2-resident plain loads;
  - (v) loads with 1/4 of the warps but the same bytes in flight.
- Collect **Nsight Compute PC sampling** on that single kernel (no concurrent-kernel serialisation problem). Stall reasons are attributed per PC, so the probe's PCs separate from the neighbours'.
  - `mio_throttle` points at the MIO queue (R2);
  - `short_scoreboard` with long latency points at the data path or banks (R3);
  - `lg_throttle` is attributed to the neighbours.
- Reverse direction: a TMA stream's rate beside an LDS kernel at 127 B/clock. Does a landing write starve when reads saturate the port?
- Expected: if (ii) does not slow the probe, a TMA-fed lane leaves the lender's shared memory alone and (f) is needed only for the consumer's reads.

**X6. Why MEM-first ILP 2 costs the compute job ~10%.**
- MEM variants:
  - tight try_wait;
  - `__nanosleep` 20 / 100 / 500 / 2000 ns;
  - no polling (issue D, then one final wait);
  - MEM block of 1 thread vs 32 active threads;
  - a **null MEM CTA**: resident, holding the same shared memory, sleeping.
- The lender's rate as a function of each variant separates residency effects from issue effects.
- Also run with MEM pinned to one SMSP (`%warpid`), measuring the compute rate per SMSP from FMA timestamps per warp.

**X7. Loaded latency and two-stage fetch, to size the buffer.**
- One SM's per-transfer TMA latency: `%globaltimer` at issue and at mbarrier completion, for 16/32 KB transfers. The other 131 SMs stream at a controlled total of 0, 1, 2, 3, 4, 4.6 TB/s.
- Then add `cp.async.bulk.prefetch.L2` d tiles ahead (d ∈ {0, 4, 16, 64}) and measure the L2 -> SM pull latency and the ring size needed for 35 GB/s.
- Check prefetched-line survival beside the cuBLAS GEMM: L2 hit rate of the pull from ncu, or `ld` latency classes.
- Output: L(load), the buffer needed for one- and two-stage fetch, and the L2 capacity cost.

**X8. What the existing controls do inside an SM.**
- `cudaStreamCreateWithPriority` (highest vs lowest) for MEM, both orders, ILP 4 and 2. Expected: no change in issue once both are resident; priority affects only which pending CTAs are dispatched (§2).
- Green Contexts: can two contexts be given overlapping SMs? If not, record that they only partition.
- MPS clients without percentage limits (shared SMs): repeat the coresid ILP table.
- Persistent MEM launched first, as the "software lane" baseline. Its failure at ILP 4 is the motivation row.

Cost:
- X0 needs the lane kernel (about a day to write and check) plus cuBLASLt/CUTLASS; it runs in ~1 hour.
- X1, X2, X5, X6 and X8 fit in one 2-3 hour holder job (small extensions of `tpc_lab.cu` and `borrow_e4.cu`).
- X3, X4 and X7 need CUTLASS/FA3 builds (half a day of setup).

---

## 2. What is publicly known (literature)

Verification tags:
- **read**: the text was checked on the page or in the PDF;
- **abstract**: only the abstract, an index entry or a summary was seen;
- **memory**: not confirmed online, so treat it as UNV.

### 2.1 NVIDIA's warp scheduling and arbitration inside an SM

**Huerta, Abaie Shoushtary, Cruz, González: the issue policy reverse-engineered on Turing and Ampere.**
- Sources: "Analyzing Modern NVIDIA GPU cores", https://arxiv.org/abs/2503.20481; the same work at MICRO 2025 as "Dissecting and Modeling the Architecture of Modern GPU Cores", https://researchr.org/publication/HuertaSC025.
- The policy is **"Compiler Guided Greedy Then Youngest (CGGTY)"** (read).
  - The scheduler keeps issuing from one warp until that warp misses in the instruction cache, or until its **Yield** control bit is set.
  - It then moves to the *youngest* of the remaining ready warps.
  - A constant-cache miss still pending after 4 cycles also causes a switch.
- Per-warp control state set by the compiler in the SASS control codes (read):
  - a **stall counter** for fixed-latency dependences;
  - the **Yield bit**;
  - six dependence counters SB0-SB5.
- Warps go to the four sub-cores by warp ID mod 4 (read).
- "Youngest" is never defined. The paper's examples suggest launch order.

**This contradicts our H200 reading (§1.3 item 3).** With CMP launched first, MEM is the youngest warp, so CGGTY predicts MEM wins the fallback. We measure the opposite: MEM wins only when it is launched *first*. Three ways out:
- (i) Hopper differs from Turing and Ampere;
- (ii) "age" in the hardware is a warp-slot order that runs against launch order (MEM launched first might get high slots);
- (iii) at ILP 4 the switches come from Yield bits and instruction-cache misses, which favour neither tenant.

X1 is built to separate these. It runs the victim in the same CTA as warp 0 or warp 31, and launches it first or last. This contradiction is a result the paper can report.

**NVIDIA patents.**
- **US9189242B2 "Credit-based streaming multiprocessor warp scheduling"**, priority date 2009, https://patents.google.com/patent/US9189242B2/en (read).
  - Each warp has a credit. Issuing lowers it; being passed over raises it.
  - The credit, weighted with other factors, picks the warp. This is a fairness mechanism in NVIDIA's own design space, close to our (a).
- US10552202 "Software-assisted instruction level execution preemption" (abstract).
- No NVIDIA patent names GTO.

**Fermi whitepaper ("Dual Warp Scheduler").** Two schedulers per SM, each issuing one warp-instruction to a group of 16 cores, 16 LD/ST units or 4 SFUs (abstract). https://www.ece.lsu.edu/koppel/gp/refs/NVIDIA_Fermi_Compute_Architecture_Whitepaper.pdf

**Simulators.**
- Accel-Sim (Khairy et al., ISCA 2020), https://engineering.purdue.edu/tgrogers/papers/khairy.isca2020.pdf.
- The public SM7_QV100 and SM86_RTX3070 configs set `-gpgpu_scheduler lrr`, although the comment above that line says "Greedy then oldest scheduler". They also set 4 schedulers per core and 64 warps (read, GitHub `tested-cfgs`).
- So the most widely used validated simulator models loose round-robin. Loose round-robin would never reproduce our starvation. Any simulation result on co-residency inherits this assumption, and that is a reviewer point in our favour.

**Our own measurement of the policy on the H200** (MEAS, `h200_warprate_summary.txt`):
- one FMA warp with ILP 4 issues 0.94 instructions per cycle;
- 4 warps reach 3.74, and 8 or more reach 4.00.
- So one warp can hold its scheduler almost every cycle, which is the precondition for greedy starvation.

### 2.2 CUDA-level controls (none of them acts on issue inside an SM)

**`cudaStreamCreateWithPriority`.** https://docs.nvidia.com/cuda/cuda-runtime-api/group__CUDART__STREAM.html (read)
- The docs say priorities "provide a hint to preferentially run work with higher priority when possible, but do not preempt already-running work or provide any other functional guarantee on execution order".
- Only compute kernels launched in priority streams are affected.
- Nothing is said about arbitration inside an SM. INF: priority acts when the work distributor dispatches CTAs. X8 checks this.

**Green Contexts.** https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__GREEN__CONTEXTS.html (read)
- They partition SMs and work queues.
- SM-count granularity is 8 on CC 9.0 and later.
- Concurrency and forward progress are "not guaranteed".
- There is no control of memory bandwidth and nothing inside an SM.

**MPS.**
- `CUDA_MPS_ACTIVE_THREAD_PERCENTAGE` "sets the portion of the available threads that can be used by the client contexts", https://docs.nvidia.com/deploy/mps/latest/appendix-environment-variables.html (read). There is no bandwidth control.
- Elvinger et al. (below): MPS does not make clients' SMs mutually exclusive.
- Our measurement: MPS packs a client's SMs into few GPCs (inferred from bandwidth).

**Compute preemption.**
- Pascal and later can preempt at instruction granularity, swapping registers and shared memory to DRAM. Pascal Tuning Guide §1.4.7, https://docs.nvidia.com/cuda/pascal-tuning-guide/index.html (read).
- That is the expensive context switch of (e1).

### 2.3 Warp-scheduling research that transfers

- **Two-level scheduling.**
  - Gebhart et al., ISCA 2011 (abstract), https://research.nvidia.com/sites/default/files/pubs/2011-06_Energy-efficient-Mechanisms-for/2011_06_NVIDIA_ISCA.pdf: an active set of warps hides short latencies and a pending set hides DRAM latency.
  - Narasiman et al., MICRO 2011 (abstract), https://utw10235.utweb.utexas.edu/pub/TR-HPS-2010-006.pdf: fetch groups that are prioritised in turn.
  - Transfer: a lane warp stays in the active set permanently, as a reserved active-set entry.
- **CCWS, which uses GTO as its baseline** (Rogers, O'Connor, Aamodt, MICRO 2012; read for the GTO baseline), https://people.ece.ubc.ca/~aamodt/papers/tgrogers.micro2012.pdf.
- **DAWS** (MICRO 2013; abstract), https://people.ece.ubc.ca/~aamodt/papers/tgrogers.micro2013.pdf.
  - Both throttle warps to protect L1 locality. That is the opposite aim: they limit memory warps.
- **MASCAR** (Sethia, Jamshidi, Mahlke, HPCA 2015; abstract), https://doi.ieeecomputersociety.org/10.1109/HPCA.2015.7056031.
  - When memory saturates, one owner warp gets priority for memory and compute-ready warps are favoured.
  - This is the closest "memory-warp priority" policy. It is single-kernel, though, and it limits memory warps rather than guaranteeing them a share.
- **OWL** (Jog et al., ASPLOS 2013; abstract), https://pure.psu.edu/en/publications/owl-cooperative-thread-array-aware-scheduling-techniques-for-impr/.
- **Multi-application memory scheduling** (Jog et al.):
  - GPGPU 2014 (abstract), https://www.cs.utexas.edu/users/skeckler/pubs/GPGPU_2014_App_Aware_Memory.pdf;
  - MEMSYS 2015 (abstract), https://www.cs.utexas.edu/users/skeckler/pubs/MEMSYS_2015_Anatomy.pdf.
  - Both work at the DRAM controller, which is our R8, not inside the SM.

### 2.4 Sharing an SM between kernels: spatial multitasking, QoS, preemption

**Closest hardware prior work, which the paper must position against:**

- **SMK, "Simultaneous Multikernel GPU"** (Wang, Yang, Melhem, Childers, Zhang, Guo, HPCA 2016), https://cs.sjtu.edu.cn/~guo-my/PDF/Conferences/C161.pdf (read).
  - Pairs memory-intensive and compute-intensive kernels on one SM.
  - CTAs are dispatched so static resources stay fair (Dominant Resource Fairness).
  - "The warp scheduler allocates cycles to kernels in proportion to an individual kernel's dynamic cycle utilization when run independently."
  - +17% over spatial-only sharing.
  - Difference from ours:
    - its aim is throughput and fairness between similar tenants, not a bounded guarantee for a tenant that needs tiny issue and a large buffer;
    - it predates TMA, so no engine decoupling;
    - it was evaluated on GPGPU-Sim's scheduler.
- **Wang et al., "Quality of Service Support for Fine-Grained Sharing on GPUs"** (ISCA 2017), https://doi.org/10.1145/3079856.3080203, PDF https://cs.sjtu.edu.cn/~guo-my/PDF/Conferences/C175.pdf (read).
  - A QoS goal becomes an instruction quota per epoch: Quota = IPC_goal × T_epoch, split across SMs by CTA count.
  - The "Enhanced Warp Scheduler" stops issuing from a kernel once its quota is used up.
  - Quotas "indirectly control memory bandwidth"; direct bandwidth control was hard.
  - **This is the baseline to beat for (a).** A quota is a *cap* enforced per epoch. Our lane needs a *floor* with low latency: a TMA producer delayed by an epoch stalls the stream.
- **Dai et al., "Accelerate GPU Concurrent Kernel Execution by Mitigating Memory Pipeline Stalls"** (HPCA 2018), https://hzhou.wordpress.ncsu.edu/files/2022/12/HPCA18.pdf (read).
  - States our failure mode almost verbatim: under intra-SM sharing, "one kernel, even as computing-intensive, may starve from not being able to issue memory instructions in time". L1 thrashing and memory-pipeline stalls caused by a memory-intensive kernel hurt the others.
  - Fix: balance memory accesses and limit each kernel's in-flight memory instructions.
  - Improves the weighted speedup of Warped-Slicer by 24.6% and of SMK by 27.2%.
  - This is the closest prior work for (f) and for the reverse direction we measured (the 12× shared-memory slowdown).
- **Warped-Slicer** (Xu, Jeon, Kim, Ro, Annavaram, ISCA 2016; abstract), https://dblp1.uni-trier.de/rec/conf/isca/XuJKRA16.html: per-kernel CTA partition inside an SM, chosen by an analytical model.
- **GPU Maestro** (Park, Park, Mahlke, ASPLOS 2017; abstract), https://bibtex.github.io/ASPLOS-2017-ParkPM.html: chooses at runtime between spatial partitioning and SMK.
- **Spatial multitasking** (Adriaens et al., HPCA 2012; abstract), doi 10.1109/HPCA.2012.6168946.
- **Preemption:**
  - Tanasic et al. (ISCA 2014; abstract), https://upcommons.upc.edu/bitstream/handle/2117/26093/isca2014.pdf: context switch or drain;
  - Chimera (Park, Park, Mahlke, ASPLOS 2015; abstract): switch, drain, or flush idempotent kernels.
  - These are the (e1) family. Our arithmetic rules them out at the granularity needed.
- **Elastic kernels** (Pai et al., ASPLOS 2013; abstract), doi 10.1145/2451116.2451160.
- **DYNCTA** (Kayiran et al., PACT 2013; abstract), https://okayiran.github.io/docs/pdf/NMNL-PACT2013.pdf: fewer CTAs for memory-intensive kernels.
- **Virtualising residency:**
  - Zorua (Vijaykumar et al., MICRO 2016; abstract), https://www.pdl.cmu.edu/PDL-FTP/associated/Zorua.pdf: virtualises registers, scratchpad and thread slots;
  - register-file virtualisation (Jeon et al., MICRO 2015) and Virtual Thread (Yoon et al., ISCA 2016): URLs not found (memory).
  - These relate to (b) and (g3).

### 2.5 Decoupling the fetch

- **Decoupled access/execute** (Smith, ISCA 1982; abstract), doi 10.1145/1067649.801719.
- **CudaDMA: warp specialisation with DMA warps** (Bauer, Cook, Khailany, SC 2011; abstract), https://research.nvidia.com/publication/2011-11_cudadma-optimizing-gpu-memory-bandwidth-warp-specialization. It is the software producer warp that (c) turns into hardware.
- **Decoupled affine computation** (Wang and Lin, ISCA 2017; abstract), https://www.cs.utexas.edu/users/lin/papers/isca17.pdf.
- **Virgo** (Kim et al., ASPLOS 2025; abstract), https://arxiv.org/abs/2408.12073: a cluster-level matrix unit. Vortex's DXA comes from this line of work.
- Hopper TMA and Blackwell `tcgen05` (a single thread issues the MMA, accumulators in TMEM) are the vendor's decoupling *inside one kernel*. Nothing public decouples a *second tenant*.

### 2.6 SMT fairness on CPU cores (what transfers to (a) and (e2))

- **ICOUNT** (Tullsen et al., ISCA 1996; abstract), https://people.csail.mit.edu/emer/media/papers/1996.05.isca.smt.pdf. Fetch goes to the thread with the fewest instructions in the front end. Transfer: an occupancy-based share instead of greedy.
- **Long-latency loads in SMT: FLUSH and STALL** (Tullsen and Brown, MICRO 2001; abstract), https://cseweb.ucsd.edu/~tullsen/smtloadabstract.html. A thread stalled on a load holds shared resources. Our lane is the inverse: the *compute* thread holds issue.
- **DCRA** (Cazorla et al., MICRO 2004; abstract), https://core.ac.uk/download/141674582.pdf. Resources are allocated by the class of what each thread needs, with borrowing of what others leave unused. Transfer: (b) with an elastic reserve.
- **MLP-aware fetch** (Eyerman and Eeckhout, HPCA 2007; abstract), doi 10.1109/HPCA.2007.346201.
- **POWER5 software-controlled thread priorities** (Boneti et al., ISCA 2008; abstract), https://core.ac.uk/download/148621402.pdf.
  - 8 levels on top of hardware balancing.
  - The priority difference sets the ratio of decode slots (memory, so UNV in detail).
  - This is the closest CPU analogue of (a) and (e2): a privileged software knob that sets an issue-slot ratio.
- **Intel RDT memory bandwidth allocation** (abstract), https://www.intel.com/content/www/us/en/developer/articles/technical/introduction-to-memory-bandwidth-allocation.html. An approximate per-core delay throttle selected by class of service. It is a cap, not a floor.

### 2.7 Co-locating LLM and DNN work in software (2022-2026)

- **NanoFlow** (OSDI 2025; read), https://www.usenix.org/conference/osdi25/presentation/zhu-kan.
  - Overlaps compute-, memory- and network-bound operations on one GPU through nano-batches and per-operation resource fractions R, profiled into an interference table (for example, decode attention at R = 0.4 reaches 80% of its peak).
  - It states NVIDIA GPUs "do not give explicit control over compute, memory, and network bandwidth".
  - It is the strongest software motivation for a hardware knob.
- **POD-Attention** (Kamath et al., ASPLOS 2025; read), https://arxiv.org/abs/2410.18038.
  - One fused kernel. Each CTA reads its SM ID and takes a ticket from per-SM atomic counters to decide between prefill and decode, so both run on the same SM.
  - A proportional split is up to 14% better than 50:50.
  - It limits contention by capping prefill splits and shrinking decode tiles, and does not analyse scheduler starvation.
  - It is the software state of the art for "same SM, two roles", and it works only inside one kernel.
- **Tally** (Zhao, Jayarajan, Pekhimenko, ASPLOS 2025; abstract), https://arxiv.org/abs/2410.07381: slicing and preemption at CTA granularity; ~7.2% p99 overhead for the high-priority task.
- **REEF** (OSDI 2022; abstract), https://www.usenix.org/conference/osdi22/presentation/han: preemption by killing and resetting idempotent kernels.
- **Orion** (EuroSys 2024; abstract), https://github.com/eth-easl/orion.
- Other systems that partition by SM, not inside it (abstract):
  - prefill/decode multiplexing, https://arxiv.org/abs/2504.14489;
  - Nexus, https://arxiv.org/abs/2507.06608;
  - DuetServe, https://arxiv.org/abs/2511.04791.
- **Elvinger, Strati, Enright Jerger, Klimovic, "Understanding GPU Resource Interference One Level Deeper"** (read), https://arxiv.org/abs/2501.16909. The SoCC'25 venue is not listed on arXiv, so it is unconfirmed.
- **Mosaic** (Strati et al.; read), https://arxiv.org/abs/2610.07504.
- LithOS, GPCPlace and Fan et al. are covered in `related-work-placement.md`. LithOS calls intra-SM control "best handled by hardware and compilers".

- **MuxServe** (Duan et al., ICML 2024), https://arxiv.org/abs/2404.02015 (search result; abstract).
  - Spatial-temporal multiplexing of several LLMs: models are placed together by popularity, prefill and decode are split, and SMs are shared through MPS percentages.
  - It allocates SM fractions, not issue inside an SM.
- **Horizontal fusion and megakernels.** This is the strongest *software* alternative to a hardware lane: put both tenants in one kernel and let the compiler balance them.
  - HFTA (Wang et al., MLSys 2021), https://arxiv.org/abs/2102.02344 (abstract): fuses repeated training jobs horizontally.
  - Mirage Persistent Kernel / MPK (OSDI 2026), https://arxiv.org/abs/2512.22219 and https://www.usenix.org/conference/osdi26/presentation/cheng (abstract): compiles a model into one persistent kernel with a per-SM task graph.
  - Hazy Research megakernels (2025), https://hazyresearch.stanford.edu/blog/2025-09-28-tp-llama-main (abstract).
  - POD-Attention (above) is the two-tenant case.
  - On Hopper, `setmaxnreg` (PTX ISA, warpgroup register reallocation) lets a fused kernel give a producer warpgroup few registers and its consumers many. That is a software version of the residency carve-out (b), *inside one kernel*.
  - Limits:
    - the tenants must be compiled together: one binary, one launch, one process, a shared lifetime;
    - issue arbitration among warps is still the hardware's. A fused lane warp starves by the same mechanism if the policy works as in §1.3. X0's fused backup variant tests exactly this.
- **Green Contexts and MIG.** Green Contexts partition SMs (§2.2). MIG on H100/H200 ties each compute slice to a fixed share of memory slices and L2, so bandwidth cannot be given to one tenant and SMs to another.
  - Both partition. Neither lets two tenants divide *within* an SM.

**Positioning table.** What each prior work controls, and what remains open.

| work | level | controls | inside one SM? | bounded floor for a memory tenant? | TMA-era decoupling? |
|---|---|---|---|---|---|
| Mascar (HPCA'15) | warp scheduler, one kernel | memory-warp priority under saturation | yes | no (it limits memory warps) | no |
| SMK (HPCA'16) | CTA dispatch + warp scheduler | fair static resources, issue cycles ∝ solo utilisation | yes | no (proportional, fairness) | no |
| Warped-Slicer (ISCA'16) | CTA counts per SM | partition by a performance model | yes | no | no |
| Wang et al. QoS (ISCA'17) | warp scheduler | instruction quota per epoch (a *cap*) | yes | no (caps the others; epoch-grain) | no |
| Dai et al. (HPCA'18) | memory pipeline | in-flight memory instructions per kernel | yes | partly (it prevents memory-pipeline starvation) | no |
| GPU Maestro (ASPLOS'17) | runtime | spatial vs SMK choice | partly | no | no |
| Orion (EuroSys'24), Tally (ASPLOS'25), REEF (OSDI'22) | runtime / kernel transformation | operator scheduling, slicing, preemption | no (time or SM) | no | no |
| MuxServe (ICML'24), NanoFlow (OSDI'25) | serving runtime | SM fractions, nano-batch overlap | no / implicit | no | no |
| POD-Attention (ASPLOS'25), megakernels, `setmaxnreg` | one fused kernel | role per CTA or warpgroup, registers | yes (same kernel) | no (the hardware arbitrates issue) | uses TMA |
| LithOS (SOSP'25), GPCPlace, Fan et al. | OS / runtime | TPC counts and placement | no (explicitly out of scope) | no | no |
| Green Contexts, MIG, MPS | driver | SM partitions, memory slices (MIG) | no | no | no |
| **this work** | warp scheduler + engine + dispatcher | issue *floor*, residency reserve, self-sequencing fetch | yes, across separate kernels or processes | **yes** | **yes** |

### 2.8 The gap

No 2024-2026 work found proposes an issue-priority floor, a residency reserve or a decoupled second-tenant engine *inside one SM* for co-located memory-bound and compute-bound jobs. The closest hardware work is SMK (2016), Wang et al. (2017, quotas) and Dai et al. (2018, memory-pipeline balancing), all pre-TMA and all evaluated on GPGPU-Sim-era scheduler models.

Software co-locates (POD-Attention, NanoFlow) but explicitly lacks a hardware knob. The published characterisation of the issue policy (CGGTY) contradicts our H200 order dependence, and that conflict is itself a result to report.

---

## 3. Design space

### 3.0 The budget of one memory lane at the fair share (INF, arithmetic on MEAS numbers)

Target per SM: 35.1 GB/s = 17.7 B/clock at 1.98 GHz.

| item | arithmetic | result |
|---|---|---|
| transfers, 32 KB | 35.1 GB/s / 32 KB | 1.07 M/s, one per 1,850 cycles |
| transfers, 16 KB | 35.1 GB/s / 16 KB | 2.14 M/s, one per 925 cycles |
| producer issue (TMA) | ~49 instructions per transfer (warprate `tma_producer`); 32 KB: 52 M instr/s | **0.66% of the SM** (7.92 G/s), 2.6% of one SMSP; 16 KB: 1.3% / 5.3% |
| producer issue, minimal loop | ~10-15 instructions (expect_tx, copy, try_wait without spin, advance, branch) | 0.2-0.4% of the SM |
| consumer issue (decode, mma.sync m16n8k16, FP16) | ldmatrix.x4 moves 512 B: 68.6 M/s; mma: 68.6 M/s × (M/8) tiles; M = 16 gives 137 M/s; +30% loop overhead | **~3% of the SM** (M = 16), ~2% (M ≤ 8) |
| consumer issue (Blackwell-style single-thread `tcgen05.mma`) | one instruction per 64×N×16 or larger tile | ≪ 0.1% |
| tensor-core time | FP16: 17.5 G weights/s × 2·M FLOPs = 562 GFLOP/s at M = 16; of 7.50 TFLOP/s per SM | **7.5%** (M = 16); 3.7% at M ≤ 8 (mma N = 8 is the minimum, so M = 4 pays as M = 8) |
| shared-memory port, landing | 17.7 B/clock of 128 | **13.8%** |
| shared-memory port, re-read by the consumer | another 17.7 B/clock | +13.8% (27.7% total if writes and reads share the port, as in Vortex; "partly" on H200) |
| bytes in flight (Little) | 35.1 GB/s × L. MEAS: one SM alone, 6 × 32 KB at 205 GB/s, so a transfer turns round in 0.96 µs; at chip saturation L is larger (UNV; X7 measures it). 1 µs: 35 KB; 2 µs: 70 KB | **35-70 KB** |
| ring in shared memory, one-stage TMA | in flight + one tile being consumed | 51-86 KB at 16 KB tiles; 67-102 KB at 32 KB |
| ring, two-stage (L2 prefetch ahead, pull from L2) | 35.1 GB/s × L2-pull latency (~0.3-0.5 µs loaded, INF from Fan et al.'s ~309-466-cycle L2 latencies plus queueing) + one tile | **~27-34 KB** at 16 KB tiles |
| L2 capacity for two-stage | 4.63 TB/s × (DRAM latency − L2 latency) ≈ 4.63 TB/s × 1-1.5 µs | 4.6-7 MB of the 50 MB-class L2 (9-14%) |
| registers | producer 1 warp × 32 = 1,024 (1.6%); consumer 1-2 warps × 64-96 = 2-6 K (3-9%) | 1.6-11% |
| ingest | lender's own TMA traffic (GEMM 320×128 tile, k-step 64: ~57 KB per ~1 µs at ~5 TF per SM, INF) ≈ 57 GB/s + lane 35 | ~92 of ~210 GB/s: not binding |

So the lane needs, per SM:
- **≤ 1% of issue for the fetch, ~3% if its consumer is on the same SM;**
- **4-8% of tensor-core time;**
- **14-28% of the shared-memory port;**
- **27-86 KB of buffer**, depending on the fetch scheme.

The asymmetry is the point:
- an issue guarantee can be tiny (bounded lender loss);
- the buffer cannot, unless the in-flight bytes are moved out of the SM (two-stage fetch, g1).

For comparison, what the lender has to spare:
- the cuBLAS GEMM leaves ~38 KB of shared memory and 1,024 registers;
- a CUTLASS GEMM at S-1 stages frees one stage (A+B ≈ 40-56 KB for 128×256 to 320×128 tiles at k = 64 FP16), at a cost X4 measures.

Vortex `h200s` equivalents (CODE / config):
- 22 cores, 64 warps, ISSUE_WIDTH 4 (`h100.json:6-13`, `h200s.json:24`);
- LMEM 228 KB in 16 banks × 8 B = 128 B/clock (`h100.json:14-16`);
- reachable memory ~716 GB/s, so the fair share is 32.5 GB/s per core = 16.4 B/clock, the same ratio as the real part.

A DXA transfer costs about 2 instructions (WGATHER to pack the descriptor, then the DXA opcode, `VX_dxa_unit.sv:28-38`, `VX_decode.sv:933-940`) plus a barrier expect_tx (`VX_wctl_unit.sv:160-163`) and the wait.

### 3.0b Honest headroom: what each mechanism can gain over the disjoint baseline

The baseline is disjoint SMs spread over the GPCs (MEAS): 4.50 TB/s with 32 memory SMs, or 4.03 TB/s with 24.

What the compute job keeps under that baseline depends on how its throughput scales with SMs:
- **Lender type P** (throughput ∝ SMs: large dense GEMMs, long-prompt prefill, training steps): keeps 100/132 = **0.76** (4.50 TB/s) or **0.82** (4.03 TB/s). This matches the FMA and CUTLASS measurements (CUTLASS lost more: 0.56 of its 132-SM rate on 100 SMs, through wave quantization).
- **Lender type D** (the measured Z-Image DiT, flat above ~70-80% of SMs): keeps ~0.97 at 76% and ~1.0 at 82% (interpolated from the MPS sweep 1.01 at "80%" and 0.90 at "70%"; MPS percentages are approximate SM counts).

Shared-SM estimates are INF:
- "lender keeps" = 1 − (the lane's issue/TC/shared-memory use) − R8 (the GEMM's own memory traffic, which hits disjoint equally and is excluded);
- the unexplained ~10% (§1.3 item 9) is shown as a pessimistic case.

| mechanism | memory job | lender keeps (opt. / pess.) | gain over disjoint, type P | gain, type D |
|---|---|---|---|---|
| measured today (FMA lender, MEM first, ILP 2) | 4.33 TB/s | 0.86 | **+8 to +9 points** (vs ~0.77-0.78) | n/a (FMA only) |
| (a) issue floor + (b) or (g1) residency | 4.5 | 0.95 / 0.86 | +19 / +10 | −2 / −11 |
| (c) engine, ring in shared memory, + (a) for the consumer | 4.5 | 0.96 / 0.87 | +20 / +11 | −1 / −10 |
| (c1) engine + private buffer + (a) | 4.5 | 0.97 / 0.88 | +21 / +12 | 0 / −9 |
| (c2) TC backfill (no consumer warp) | 4.5 | 0.97 / 0.92 (TC-idle dependent) | +21 / +16 | 0 / −5 |
| (d) remote fetch to 23-29 consumer SMs | 4.6 | 0.78-0.83 | +2 to +7 | −14 to −19 |
| (e2) resident micro-slices | 4.5 | 0.94 / 0.85 | +18 / +9 | −3 / −12 |
| ideal ceiling (free lane) | 4.6 | 0.97-1.00 | +21 to +24 (+28-32% relative) | ~0 |

Reading the table:
- **The hardware headroom exists only beside lenders whose throughput scales with SMs.**
- For the measured DiT the disjoint baseline is already near-free. Any sharing mechanism can at best tie it, and loses if its lender cost is ≥ 3%.
- So the evaluation must pick type-P lenders honestly, and show the DiT case as the negative control:
  - prefill of a long prompt beside decode;
  - a training step beside rollout decode;
  - a large-batch image model that does scale.
- **First measure the DiT's scaling with SM counts read back** (TPC masks, not MPS percentages): if it is in fact type P, the table's right column disappears.

### Mechanism (a): issue priority / reserved issue share for a "memory lane" warp class

- **Mechanism.** Mark lane warps with one bit per warp. In each scheduler, a token bucket accrues credit at rate s (e.g. s = 1/16) up to depth B (e.g. 4-8). When credit ≥ 1 and a lane warp is ready, the lane warp wins over the greedy pick and one credit is spent. Without credit, lane warps compete normally (or at lowest priority, configurable). This is a guaranteed minimum share with a cap, not strict priority.
- **Resource.** R1. It also fixes the luck-dependence on launch order and ILP.
- **Hardware.** Per warp: 1 bit (64 bits per SM). Per scheduler:
  - a credit counter of ⌈log2(B/s)⌉ ≈ 7 bits plus increment logic;
  - a "lane ready" OR-reduction over the scheduler's warps;
  - a 2:1 override mux in front of the existing select.
  Under 100 flip-flops per SM. No new queues.
- **Visibility.**
  - Driver: the lane bit per CTA in the launch descriptor (QMD field or launch attribute, e.g. `cudaLaunchAttributeMemoryLane`), with s set per SM by the driver/MPS (privileged).
  - ISA (optional): `setlane` for warp-specialised kernels that mark only their producer warp.
- **Lender protection.** Worst case: the lender loses s of each scheduler's slots, and only while a lane warp is ready. A lane that needs 0.7-4% uses that much, so with s = 1/16 the bound is 6.25% and the expected loss is ≈ actual use. Plus the unexplained MEM-first effect (§1.3 item 9), which must be bounded by X6.
- **Expected gain.**
  - MEM per SM goes from 0.7-11 GB/s (ILP 4 lender) to its demand of ~35 GB/s as long as the demand is under s.
  - At s = 1/16, a 49-instruction producer can issue 2.5 M transfers/s per scheduler = 81 GB/s at 32 KB. That is ≥ 2.3× the fair share, so one lane warp per SM suffices.
  - Chip level (INF): shared, any order, any ILP: ~4.5-4.6 TB/s with the compute job at ≥ 0.94 (bounded by s and R8).
  - Compare: today's lucky co-run 4.33 TB/s / 0.86; disjoint 4.50 / 0.76 and 4.03 / 0.82.
- **What changes in Vortex.**
  - `hw/rtl/core/VX_warp_sched.sv:377-398`: the `schedule_warps` priority encoder, lowest-wid-first today; add the lane override and credit.
  - Issue arbiter `VX_scoreboard.sv:344-354`: a sticky matrix arbiter. Add the same override, or the fetch-side fix is undone at issue.
  - Per-warp lane bit, set by a CSR write or by the KMU per CTA (`VX_cta_dispatch.sv`).
  - SimX: `scheduler.cpp:203-208` and `core.cpp:911-922`.
  - Effort: ~2-3 days SimX, ~1 week RTL with a unit test.
  - **But the baseline must first starve (§4.1).**
- **Risks.**
  - (1) If X0 shows tensor-core lenders do not starve the lane, (a) is needed only for CUDA-core phases.
  - (2) The consumer, not the producer, may be the issue-hungry part.
  - (3) The lane's instructions go to shared blocks too: Vortex has **one SFU block shared by the 4 slices** (`VX_config.toml:150`, round-robin in `VX_lane_dispatch.sv:96-110`), which carries DXA issue, barriers and CSRs, so issue priority does not reach it.
  - (4) The fixed-priority bank crossbar in the register file (`VX_opc_unit.sv:107-111`).
- **Closest prior work.**
  - SMT fetch and issue policies (ICOUNT; POWER5 software thread priorities; DCRA). They give priority or allocation for throughput or fairness, not a guaranteed tiny share for an asymmetric tenant.
  - GPU warp schedulers that prioritise memory warps (MASCAR) and intra-SM QoS quotas (Wang et al. ISCA 2017).
  - §2 has the URLs.

### Mechanism (b): residency carve-out

- **Mechanism.** The CTA dispatcher reserves, per SM, R_lane registers, S_lane bytes of shared memory and W_lane warp slots for lane CTAs. Lender kernels' occupancy is computed against (65,536 − R_lane, 228 KB − S_lane, 64 − W_lane). Lane CTAs can be admitted only into the reserve.
- **Resource.** R6.
- **Size.**
  - **Shared memory**: one-stage 51-86 KB (16 KB tiles) or 67-102 KB (32 KB); two-stage (g1) 27-34 KB.
  - **Registers**: 1,024 (producer only) to ~6 K (producer + consumer).
  - **Warps**: 1-3.
  - Against the cuBLAS GEMM (189 KB, 64,512 registers): a one-stage ring cannot fit (189 + 51 > 227); a two-stage ring of 34 KB fits (223 KB), but only a 32-register single warp fits in the registers. So with production GEMMs **either the consumer is elsewhere ((c2)/(d)) or the lender is compiled for less (X4)**.
- **Hardware.** A reserve register triple per SM in the dispatcher, plus a lane class in the admission check. Small.
- **Visibility.** Driver: a per-SM or per-context reserve (like a Green Context resource descriptor, but intra-SM). Compiler: the lender's occupancy calculation must see the reduced budget (cudaOccupancy*).
- **Lender protection.** The lender loses the reserve whether it is used or not, plus whatever (a) costs. Price from X4. A 34 KB reserve is ~15% of shared memory: for CUTLASS that is about one pipeline stage.
- **Expected gain.** On its own, none: residency without (a) still starves beside ILP-4 issue. It is the enabler for (a)/(c).
- **What changes in Vortex.**
  - (1) Concurrent residency of two kernels: **Vortex runs one kernel at a time** (one KMU, `Vortex.sv:106`; launches serialised, `VX_cp_launch.sv:7-17`; `VX_kmu.sv:289-298`). That is a multi-week change.
  - The cheaper route: one fused launch with roles by CTA index (`gemm_block` already splits roles, by SM). But admission uses **one fixed LMEM stride per kernel** (`VX_cta_dispatch.sv:482-502, 600-607, 726-764`), so a small lane CTA beside a big compute CTA needs a second stride class (~1 week SimX + RTL).
  - (2) **Vortex has no register budget**: GPRs are statically sized per warp (`VX_opc_unit.sv:49`), so register pressure must be *modelled* (an artificial per-core register limit in the dispatcher) or the GEMM-residency argument cannot be tested in Vortex.
- **Risks.**
  - The reserve is wasted when the memory job is idle (mitigation: an elastic reserve that the lender's next CTA wave reclaims; that adds preemption complexity).
  - The occupancy cliff: a lender needing 1 KB more than it has loses a whole CTA per SM.
- **Closest prior work.** SMK (Wang et al. HPCA 2016) partitions SM resources between kernels for fairness; Warped-Slicer (ISCA 2016) picks per-kernel CTA counts per SM; Zorua / register-file virtualisation (MICRO 2015/2016) relax static residency (§2).

### Mechanism (c): decouple the memory role from warp execution — a per-SM autonomous stream engine

- **Mechanism.** The driver or kernel programs a **descriptor ring** once per stream:
  - base, total bytes, tile size and shape (reuse the TMA tensor map), destination ring (N slots of T bytes);
  - full[i] and empty[i] barrier addresses;
  - an L2 prefetch lead d (for g1).
  The engine keeps a credit counter equal to the free slots. While credit > 0 and bytes remain, it issues the next tile, and on its completion it arrives on full[i]. The consumer arrives on empty[i] and the credit returns. No warp issues producer instructions.

  This is a DMA scatter-gather engine with TMA addressing and mbarrier completion. Hopper's TMA already does the address generation and completion; what is new is the self-sequencing loop and the credit return.
- **Resource.** R1 for the fetch side (no producer issue at all) and R6 (no producer warp or registers).
- **Hardware.**
  - Per SM: 2-4 stream contexts × ~64 B of descriptor + 16-bit credit and byte counters (~2 Kb of flops);
  - a sequencer FSM feeding the existing TMA/DXA request queue;
  - a credit-return path from the barrier unit (an "empty" arrive increments the credit).
- **Visibility.**
  - PTX-level `stream.start desc, ring` and `stream.stop`, or a driver-programmed per-SM stream that a kernel only consumes.
  - The ring lives in shared memory (c1) or a private buffer.
- **Who computes the memory job's math** (the hard part):
  - **(c1) a small private buffer + the tensor core.** The ring sits in a dedicated SRAM next to the tensor core (27-34 KB with g1), not in the lender's shared memory.
    - The memory job's consumer warp issues mma with operands from that buffer: it still needs ~2-3% issue, so pair with (a).
    - Or, on a Blackwell-like core, a single thread issues `tcgen05.mma` with the accumulator in a small TMEM allocation (M = 16 × N_rows FP32: ~8 KB per 128 output rows): issue ≪ 0.1%.
    - Hardware: 32 KB SRAM + one more operand source mux on the tensor core. About 1.5% of the SM's SRAM.
  - **(c2) the lender's idle tensor-core cycles.** A hardware sequencer issues the memory job's MMAs (weights from the ring, activations from a small register or SRAM buffer: one K-chunk × M = 128 × 16 × 2 B = 4 KB) into tensor-core cycles the lender leaves idle, with a minimum share so the stream cannot be blocked forever.
    - Needs a second accumulator context (8-16 KB) and a TC-side arbiter that gives the lender priority and the background context backfill plus a minimum share.
    - The lane needs 4-8% of TC time; a compute-bound GEMM leaves an idle fraction from its pipeline bubbles and epilogues (UNV; we have no measurement of tensor-core idle time — Nsight Compute `sm__pipe_tensor_op_hmma_cycles_active` per kernel would give it).
    - Lender cost: ≈ the TC time used when the lender wanted it (bounded by the minimum share) + accumulator SRAM.
- **Lender protection.** No issue taken for the fetch. Shared-memory port taken only if the ring is in shared memory (c in shared memory: 14% for landing). (c1) takes nothing from shared memory. (c2) takes ≤ min-share of TC time.
- **Expected gain.**
  - (c) + ring in shared memory + consumer under (a): like (a), with the producer's issue share removed. The lane's issue need falls from ~4% to ~3%: small in issue, but it **removes the latency-critical producer** (a producer delayed by one issue slot stalls the whole stream; a consumer delayed only fills the ring).
  - (c1)/(c2) remove the shared-memory and register footprint entirely. They are the only variants that coexist with an unmodified cuBLAS-class GEMM (38 KB, 1,024 registers free).
- **What changes in Vortex.**
  - DXA is per core already (socket-resident, `VX_socket.sv:771-806`, SOCKET_SIZE = 1). It has a descriptor table written by the host through DCRs (`VX_dxa_desc_table.sv:38-45`), a request arbiter → queue of 16 → round-robin worker dispatch (`VX_dxa_core.sv:55-85, 165`; `VX_dxa_dispatch.sv:58-62`), and completion via `VX_dxa_completion.sv:28-31` → txbar → scheduler barrier (`VX_core.sv:326`).
  - Add a sequencer in front of `VX_dxa_core`'s request arbiter that generates requests from a ring descriptor, and a credit-return input from the barrier unit (empty arrives).
  - SimX: `dxa/dxa_core.cpp:354-365`.
  - Effort: SimX ~1 week; RTL ~2-3 weeks.
  - (c1): a separate LMEM-like SRAM and a TCU operand mux, wherever the TCU fills its tile buffer (`VX_tcu_tmem.sv:28-36`, `VX_mem_unit.sv:107-121`), ~2 weeks SimX.
  - (c2): accumulator context and TCU arbiter, the largest (~1 month).
- **Risks.**
  - (1) If X0 shows the producer is not starved beside TC lenders, (c)'s issue advantage vanishes and its value rests on residency (c1).
  - (2) Credit-return latency adds to the loop. Size the ring by L + credit latency.
  - (3) (c2) depends on tensor-core idle time that may be ~0 in a tuned GEMM. Then it degenerates to stealing with a minimum share.
- **Closest prior work.**
  - Decoupled access/execute (Smith ISCA 1982);
  - CudaDMA warp specialisation (SC 2011) — software producers, which this replaces;
  - Hopper TMA and Blackwell `tcgen05` (single-thread MMA, TMEM) are the vendor's step in the same direction for *one* kernel;
  - Virgo (cluster-level matrix unit; Vortex's DXA lineage).
  - §2 has the URLs.

### Mechanism (d): offload only the fetch to the shared SMs; the memory job's compute stays on its own SMs

- **Mechanism.** The shared SMs' engines (lanes via (a) or (c)) fetch tiles and write them into the shared memory of a few "consumer" SMs of the memory job over an SM-to-SM path (DSMEM today, which works only inside a cluster, i.e. inside a GPC).
- **Resource.** It moves the consumer's issue, TC time and residency off the shared SMs. The fetch's remains.
- **Bound (MEAS + INF).**
  - The consumer's ingest caps at ~215 GB/s write-only and 157-170 GB/s staged-then-consumed (E2). Remote TMA delivers 75 GB/s per helper.
  - For 4.63 TB/s, the memory job needs ≥ 4.63 / 0.215 = **22-23 consumer SMs** write-only, **27-29** staged.
  - Disjoint spread needs ~32 SMs for 4.5 TB/s and 24 for 4.03.
  - So (d) saves only 3-9 SMs (2-7% of the chip) over the disjoint baseline, and it needs cross-GPC SM-to-SM paths: a GPC returns ≤ 1.28 TB/s and 22 consumers span ≥ 4 GPCs; with clusters confined to a GPC, each GPC's consumers must be fed by that GPC's helpers.
- **Hardware.** A remote-write path from every SM's engine to any SM's shared memory (or at least within a GPC, as today); credit flow across SMs.
- **Lender protection.** The lender loses only the fetch's footprint, the same as (a)/(c) producer-only, ~0.7% issue + the ring if staged on the lender. With remote write: no ring on the lender.
- **Expected gain.** Compute share 0.76 -> ~0.80-0.83 at 4.5 TB/s (INF). Small. **Helpers starve exactly like local MEM** (C.3: 3 helpers beside ILP-4 FMA delivered 1.0 GB/s), so (d) needs (a) or (c) anyway.
- **What changes in Vortex.** There is no SM-to-SM path today (four private links per SM into one L2, `noc-and-design-space.md` §1a). Large: a new network.
- **Risks.** The ingest bound makes the idea dominated by (c1) unless the consumer's ingest is raised, e.g. landing into a wide private buffer (c1) on the consumer, which is then (c1) + (d).
- **Closest prior work.** Hopper distributed shared memory and clusters; our own E2/C.3 measurements (software borrowing adds nothing).

### Mechanism (e): time multiplexing at fine grain

- **(e1) With a context switch** (the lender is swapped out for micro-slices). Ruled out by arithmetic:
  - Lender state: 64 K registers × 4 B = 256 KB + 189 KB shared memory = 445 KB per SM. Spilling it at even 128 B/clock takes 3,500 cycles = 1.8 µs each way.
  - For ≤ 10% overhead, slices of ≥ 36 µs. During the memory slice the SM must fetch at 35 GB/s ÷ duty cycle. With duty 0.16, the rate needed is 215 GB/s = the ingest cap.
  - The memory job therefore needs ≥ 16% of the SM's time + 2 × 1.8 µs of switch per period. The compute job keeps ≤ 0.84 minus switches: no better than disjoint (0.82 at 24 SMs). Dead.
- **(e2) Resident micro-slices** (both resident; a global timer gives the lane strict priority for w cycles every P cycles, e.g. w = 64, P = 1,024).
  - This is (a) with burstier service and a single per-SM timer instead of per-scheduler counters.
  - The lane must have work ready in its window. TMA issue batches well: issue ≤ D transfers per window.
  - Buffer to absorb the period: 35.1 GB/s × 1,024 cycles / 1.98 GHz = 18 KB, on top of L × rate.
  - Lender bound: w/P = 6.25% of issue when the lane is ready.
  - Cheaper than (a), coarser, worse for a latency-critical consumer.
  - Vortex: the same hook points as (a) plus one timer.
- **Closest prior work.** Chimera / instruction-level preemption (context switch); the SMT POWER5 priority duty cycles are the (e2) analogue (§2).

### Mechanism (f): bank-level / LSU isolation

- **(f1) Separate per-role MIO / LSU input queues.** Virtual channels with round-robin between roles, so one role's queued global loads cannot head-of-line-block the other's shared-memory operations (the 12× of §1.3 item 8). Hardware: duplicate the input queue (e.g. 2 × 8 entries per LSU block) + a 2:1 arbiter.
- **(f2) A partitioned landing region.** The lane's ring sits in a dedicated bank group or a separate SRAM macro, so its landing writes do not take the lender's 128 B/clock port. A separate macro is (c1)'s buffer. Reserving 2 of 16 banks would cost a bandwidth-bound lender 12.5% of its shared-memory bandwidth, so the macro is better.
- **(f3) Rate-limit DMA landing writes.** Landing writes take at most x% of port cycles while the lender has bank requests pending (token bucket at the bank arbiter).
- **Resource.** R2/R3.
- **Protects whom.** Mostly the **lender**. Our data show streaming loads hurt shared-memory users, not the other way round.
- **Vortex specifics, important** (CODE):
  - DXA writes and TCU tile-buffer reads share **one 128 B bank-row DMA port** through a round-robin arbiter (`VX_mem_unit.sv:113-121`).
  - **Any pending DMA request blocks all LSU passes to LMEM that cycle** (`pass_en = ~dma_valid && ~amo_stall`, `VX_lmem_unit.sv:506-515`; SimX `local_mem.cpp:519-548`).
  - So in Vortex a lane's landing writes have *absolute* priority over the lender's shared-memory loads. At 16.4 B/clock in 64 B beats that is ≥ 26% of LMEM cycles blocked, before the consumer's reads.
  - This is the opposite of the H200 direction and must be changed or calibrated before any co-run number means anything: (f3) at `VX_lmem_unit.sv:506-515`.
- **Expected gain.** (f2)/(f3) cap the lender's shared-memory loss near the lane's actual port use (14-28%) or below (f2 → 0). This matters only for lenders that are shared-memory-bandwidth-bound (wgmma GEMMs at ~80 B/clock of 128, INF from `open-threads.md`). On real hardware the size of the effect is unknown until X5.
- **Closest prior work.** Bank-partitioning/isolation in DRAM controllers and cache partitioning (Intel CAT/MBA); on GPUs, Dai et al. HPCA 2018 (memory-pipeline stalls of concurrent kernels) (§2).

### Mechanism (g): better or additional ideas

**(g1) Two-stage fetch: L2 prefetch far ahead, then just-in-time pull.**
- The engine (or producer) issues `cp.async.bulk.prefetch.L2` d tiles ahead of the landing copy. The landing copy then sees L2 latency instead of DRAM latency, cutting the SM-side ring from 51-86 KB to ~27-34 KB.
- This is what makes a lane fit beside a production GEMM (38 KB free).
- Costs:
  - 4.6-7 MB of L2 chip-wide (9-14%);
  - the risk of eviction by the lender's traffic (mitigated by L2 eviction-priority hints, which exist on Hopper as `evict_last` / access-policy windows).
- **Testable on the real H200 today with no new hardware** (X7). In hardware, (c)'s descriptor carries the lead d.

**(g2) Compiler- or driver-controlled yields in the lender.**
- If X1 shows forced switches rotate among warps (§1.3 item 4), a lender compiled to yield every N instructions gives the lane a share ∝ 1/N with no hardware. That is the GPU analogue of an ICOUNT-style throttle, applied to the lender.
- NVIDIA's SASS control codes carry a yield bit (§2), but PTX gives no control. Only the vendor could do this.
- Lender cost: a yield costs a switch only if another warp is ready.
- A useful *baseline* for the paper: "what software at the vendor could do".

**(g3) Lane as a separate hardware context, not a warp slot.**
- A 65th "background" context per SM with its own small register file (32 registers × 32 lanes = 4 KB) that only (a) or (c) can schedule.
- Removes the warp-slot and register parts of residency.
- Overkill unless X0/X3 show registers are the binding residency limit for typical lenders.

**(g4) Lender-aware placement plus lanes on only some SMs.**
- A lane on each SM is unnecessary: 35 GB/s × 132 = 4.63 TB/s. Lanes on ~66 SMs at ~70 GB/s each, which (a) at s = 1/16 supports (81 GB/s), halve the number of SMs that pay the lender cost.
- Spread them ≤ 8 per GPC so that 8 × 70 = 560 GB/s stays under the 1.28 TB/s GPC cap.
- Pure policy: the per-SM lane rate trades against how many SMs pay.

---

## 4. Comparison and recommended order

### 4.0 Comparison table

Gains are INF and assume the lane has its demand; the lender cost is the expected value and, in brackets, the bound.

"Lender keeps" is the optimistic case for a type-P lender. See §3.0b for the pessimistic case and for type-D lenders (the measured DiT), where no mechanism beats disjoint.

| mechanism | resource | hardware | visibility | memory job (chip) | lender keeps | fits beside cuBLAS-class GEMM | Vortex effort | kill result |
|---|---|---|---|---|---|---|---|---|
| (a) issue share | R1 | ~100 flops per SM | QMD bit / launch attribute | 4.5-4.6 TB/s | ~0.96 (≥ 0.94) | only with (b)/(g1) | days (SimX) | X0: no starvation beside TC lenders; or lender loss ≫ s (X6) |
| (b) carve-out | R6 | dispatcher reserve | driver + occupancy API | enabler only | 1 − X4 price | ring ≤ 38 KB only | 1-2 weeks + register model | X4: losing 34 KB costs the lender > 10% |
| (c) stream engine, ring in shared memory | R1 (fetch), R6 (warp) | ~2 Kb flops + sequencer | `stream.start` / driver | 4.5-4.6 | ~0.96 | with g1 | 1 week SimX | no gain over (a) at equal lender loss |
| (c1) + private buffer | + R3, R6 | + 32 KB SRAM, TC mux | same | 4.5-4.6 | ~0.97 | **yes** | +2 weeks | X5 shows the shared-memory port costs < 3% |
| (c2) TC backfill | + TC time | + accumulator context, TC arbiter | same | 4.5-4.6 | 0.92-0.97 (TC-idle dependent) | **yes** | ~1 month | tensor-core idle in a tuned GEMM < lane need |
| (d) remote fetch | moves consumer off | SM-to-SM path, cross-GPC | cluster-like | 4.6 | ~0.80-0.83 | yes | large (new network) | dominated by (c1) unless consumer ingest is raised |
| (e1) context-switched slices | — | — | — | — | ≤ 0.84 − switch | — | — | dead by arithmetic |
| (e2) resident micro-slices | R1 | 1 timer per SM | driver | 4.5 | ~0.95 (≥ 1 − w/P) | with (b)/(g1) | days | consumer latency suffers vs (a) |
| (f1-f3) shared-memory/LSU isolation | R2, R3 | queue copy / arbiter / SRAM | none | protects the lender | +0-14% for shared-memory-bound lenders | — | days (f3 in Vortex is mandatory) | X5: TMA landing does not slow LDS |
| (g1) two-stage fetch | buffer (R6) | none (prefetch exists) or 1 descriptor field | software today | enabler | −L2 capacity effect | makes the ring fit | days (DXA prefetch op) | X7: prefetched lines evicted, or L2-pull latency ≈ DRAM latency |
| disjoint spread (baseline, MEAS) | — | none | TPC masks | 4.50 / 4.03 | 0.76 / 0.82 | yes | exists | — |
| shared, lucky (MEAS) | — | none | launch order | 4.33 | 0.86 | no (FMA lender only) | — | — |

### 4.1 What must be calibrated in Vortex first

The prototypes are meaningless unless the Vortex baseline reproduces the H200 co-residency fingerprint. Today it cannot, for the following reasons (CODE).

1. **Per-warp fetch stall.**
   - A warp is marked stalled when scheduled and unlocked only at decode (`VX_warp_sched.sv:246-252`, `VX_decode.sv:1186-1199`), about 11 cycles per instruction per warp.
   - A compute warp therefore cannot issue every cycle, so no "greedy ILP-4 warp" exists. The lender's ability to monopolise a slice depends on having ≥ 11 ready warps per slice.
   - Starvation would then come from **fixed lowest-wid priority** (`VX_warp_sched.sv:377-398`; SimX `scheduler.cpp:203-208`): a high-wid lane warp starves whenever ~11+ lower-wid warps per slice are eligible. That is starvation by warp ID, not by age or ILP.
   - The fix in progress (no stall until decode) changes this completely: after it, fixed priority makes the lowest warp monopolise.
   - **Decide the policy deliberately:** implement a GTO-like greedy select with a configurable fallback (oldest / youngest / rotate-on-yield). Calibrate it against X1's fingerprint (ILP × order × warp count), reproducing at least the table in §1.3 item 3 and the 1/n rule of item 4.
   - The issue arbiter is a sticky matrix arbiter (`VX_scoreboard.sv:344-354`; `core.cpp:104` GreedyMatrix). Fetch and issue policies must agree, or one undoes the other.
2. **Concurrent residency.**
   - One kernel at a time (`VX_cp_launch.sv:7-17`, `VX_kmu.sv:289-298`) and one LMEM stride per kernel (`VX_cta_dispatch.sv:726-764`).
   - Short term: a fused `gemm_block`-style launch with roles chosen by CTA index *within a core*, both roles at the same LMEM stride. Longer term: a second stride class.
   - Registers: add an artificial per-core register budget to the dispatcher, or the residency question cannot be posed.
3. **Barrier drain.**
   - `BAR_DRAIN_SCOPE=1` in `h200s` (`h200s.json:16`) still makes a stalling barrier wait until every LSU block's requests are sent and core-wide stores have landed (`VX_wctl_unit.sv:183-199`). `bar_arrive` always waits for the core-wide drain (:194).
   - A co-resident lane's queued requests would delay every lender barrier. The H200 does not do this (MEAS E4).
   - It must become per-warp or per-CTA (the SimX experiment `VX_BAR_DRAIN=warp` exists, `notes/open-threads.md`) before co-run results mean anything.
4. **LMEM arbitration.** DMA has absolute priority over LSU passes (`VX_lmem_unit.sv:506-515`), and DXA writes and TCU reads share one 128 B DMA port (`VX_mem_unit.sv:113-121`). Calibrate against X5, and against E2's "writes and reads partly share", then set (f3) as the baseline arbitration.
5. **Shared SFU block** carrying DXA issue, barriers and CSRs for all 4 slices (`VX_config.toml:150`): check it does not become the lane's bottleneck when the lender uses barriers or CSRs heavily.
6. **Lender realism.**
   - `corun -b0` IPC 3.31 of 4 after the FPU and dispatch fixes (`open-threads.md`). The FMA lender must reach ~4.0 to reproduce "greedy never yields".
   - The tensor-core lender: `gemm_tmem` / wgmma at ≥ 2,000 FLOPs per clock, so its issue rate is realistically low.
7. **Memory and placement:**
   - `h200s` at 22 cores (fair share 32.5 GB/s per core);
   - pseudo-LRU vs FIFO L2 (FIFO lets a stream evict the lender's hot lines, `open-threads.md`). Use the policy that matches the real ratio of 1.00.
   - Vortex has no GPC level, so chip-level placement results are out of scope there. Use the real part for those.

### 4.2 Prototype first (SimX), and the smallest distinguishing experiment

**Prototype P1 = (a), the issue share.** It is the cheapest and tests the root-cause claim directly.

**Prototype P2 = (c) with (g1), the ring engine with an L2 prefetch lead.** It is the most likely final design and separates "the producer must be issue-free" from "a share is enough".

**Prototype P3 = (f3), the LMEM DMA rate limit.** It is needed as a fair baseline anyway (§4.1 item 4), and it gives the lender-protection curve.

**The smallest experiment that distinguishes them.**
- Setup:
  - 1 core of `h200s`, memory as on 22 cores (or 2 cores with the other 20 idle, so memory is uncontended);
  - one fused launch: lender CTAs (FMA at ILP 4 after calibration item 1, then `gemm_tmem`);
  - one lane CTA streaming at a target of 32.5 GB/s with a decode consumer (`gemm_block` M = 16 consumer, one warp).
- Five runs: baseline co-resident; P1 at s ∈ {1/64, 1/16}; P2 (ring 2-4 × 16 KB, lead d ∈ {0, 8}); P2 + P1 for the consumer; disjoint (2 cores, one per role).
- Metrics: lane GB/s, lender cycles, LMEM blocked cycles, the lane's per-transfer issue delay.
- What separates them:
  - P1 vs P2 at equal lender loss: if both reach 32.5 GB/s, (a) wins on cost;
  - if P1 needs s ≫ the lane's average use, because producer latency matters, then P2;
  - P3 on vs off shows how much of the lender loss is LMEM.

**Kill criteria.**
- **(a)** is killed if, *after calibration*, the Vortex baseline does not starve the lane. Then SimX cannot show the gain and the case rests on real-GPU data. It is also killed if lender loss > 2 × the lane's issue use (X6 carried over), or if the lane stays below 30 GB/s at s = 1/16 because something else binds (SFU block, LMEM).
- **(c)/(g1)** is killed if it gives no gain over (a) at equal lender loss and equal ring size, or if X7 shows L2-pull latency under load ≥ 0.7 × DRAM latency (then g1 buys nothing).
- **(f3)** is not needed if, with the lane at 32.5 GB/s, LMEM-blocked cycles cost the lender < 3%.

### 4.3 Order

1. Real GPU: **X0 first (go/no-go).** On GO: X1, X2, X5, X6, X8 (one holder job), then X3, X4, X7. On KILL: stop the hardware track; X4 and X7 still serve a software paper.
2. Vortex calibration items 1-4 of §4.1. Items 1 and 3 are in flight or decided elsewhere.
3. SimX P3 (baseline arbitration), then P1, then P2. The distinguishing experiment of §4.2.
4. Only if P2 wins: (c1) private buffer, and the RTL for the winner (synthesis cost, which `open-threads.md` lists as owed for every RTL-visible change).
5. (c2) and (d) only as discussion, unless X3 shows TC idle time is plentiful.

---

## 5. How this reads as a paper or proposal

**Framing.** "On today's GPUs, compute and memory bandwidth cannot be allocated separately."
- MIG ties memory slices to GPC slices.
- MPS, Green Contexts and TPC masks allocate SMs, and bandwidth then follows the SM count (per-SM ingest ~205-239 GB/s; GPC egress ~1.28 TB/s).
- Sharing an SM is the only way to give one tenant the bandwidth and the other the SMs, and today it fails in two ways:
  - (A) the block scheduler alternates SMs between non-co-residable kernels: the GEMM fell to 0.25 (MEAS);
  - (B) when the kernels do co-reside, greedy issue starves the memory tenant: 0.6-6.7% beside an issue-bound kernel (MEAS). Beside tensor-core kernels it is unknown until X0.
- The memory lane is the missing allocation knob: an issue floor, a residency reserve, a self-sequencing fetch.

**Claim**, contingent on X0 saying "go":
- A bandwidth-only tenant needs ≤ 1-4% of issue, 4-8% of tensor-core time and a 27-34 KB two-stage buffer per SM. That holds for decode batches B ≲ 16; at B = 64 decode needs ~39 SMs of tensor throughput and is a compute tenant.
- A lane costing < 0.1% of SM area lets that tenant draw its fair share (35 GB/s) from every SM.
- An SM-proportional compute tenant then keeps ≥ 0.94, against 0.76-0.82 with the best disjoint placement.
- Today's best software result is +8-9 points; the lane's ceiling is about +21.

**Deployment scenarios a reviewer accepts.**
- (1) Prefill + decode in one serving engine: per-replica decode batch ≤ 16 beside long-prompt prefill, which is type P. It competes with POD-Attention, NanoFlow and megakernels, which fuse inside one kernel; the lane works across separate kernels.
- (2) RL rollout decode beside policy training: training is type P.
- (3) Multi-model serving (MuxServe-style): a cold, small-batch model beside a busy one.
- Not the motivating example: the measured DiT, which is type D (§3.0b) and serves as the negative control.

**Baseline.**
- Disjoint SMs, spread over the GPCs, at most 5-6 memory SMs per GPC. Published-style: GPCPlace, Fan et al.; LithOS sizes TPC counts.
- Our measured points: 32 SMs give 4.50 TB/s at compute 0.76; 24 give 4.03 at 0.82.
- Also report the lucky co-run (MEM first, ILP 2: 4.33 / 0.86) to show that SM sharing *can* win but is fragile today.

**Headline experiment.**
- A **Pareto plot**: memory-job TB/s against the compute job's fraction of solo throughput.
  - Real-H200 points: disjoint spread at 8-48 memory SMs, packed, shared-lucky, shared-ILP4.
  - Simulated curves: lane (P1/P2) with s or ring size as the parameter, and disjoint in SimX for the cross-check.
- Then end-to-end: Qwen3.5-9B decode tokens/s (GPU kernel time) beside Z-Image-Turbo steps/s.
- Plus the anatomy figure: rate ∝ 1/warps, depth independence, order × ILP.

**Strongest objections and answers.**

1. *"Your starvation is an FMA microbenchmark artefact. Real compute kernels are tensor-core-bound and issue-light."*
   - Answer with X0/X3: cuBLAS, CUTLASS, FA3 and DiT elementwise kernels.
   - The DiT's CUDA time is 45% matmul, 26% elementwise and 12% attention (`corun-workloads.md`). The elementwise and softmax parts are issue-heavy.
   - If tensor-core lenders do not starve the lane, the claim narrows to residency + buffering + elementwise phases. Say so honestly.
2. *"Production kernels leave no room."*
   - The arithmetic of §3.0: 1,024 registers and 38 KB are left, enough for a one-warp producer with a 2 × 16 KB ring only with two-stage fetch.
   - X4 gives the price of a larger carve-out; (c1) avoids it.
3. *"Just use disjoint SMs and good placement."* At equal memory bandwidth the lane returns up to ~+19-21 points of compute share for type-P lenders (0.76 → ~0.95-0.97); +10-12 if the unexplained ~10% persists; nothing for type-D lenders (§3.0b). Measured so far: +8-9. Show it as the Pareto gap.
4. *"The memory system is the shared resource anyway."*
   - The DiT uses 1.7% of memory bandwidth.
   - But the CUTLASS GEMM's own traffic cut the stream from 4.49 to 3.7 TB/s: report R8 separately. It affects disjoint and shared equally, so it is not an argument for either.
5. *"Stream priorities, MPS or Green Contexts already do this."*
   - X8 shows stream priority acts at CTA dispatch, not issue.
   - Green Contexts and MPS percentages partition or limit SMs; none gives intra-SM issue QoS (§2).
6. *"SMT fairness and GPU intra-SM QoS are old"* (ICOUNT, DCRA, SMK, Wang ISCA 2017, Warped-Slicer).
   - Novelty:
     - (i) the tenant is asymmetric (tiny issue need, latency-sized buffer);
     - (ii) the fix is a bounded *guarantee*, not fairness or throughput maximisation;
     - (iii) TMA-era decoupling (a self-sequencing engine plus two-stage fetch) removes most of the footprint;
     - (iv) measured on a current part, with the exact starvation fingerprint.
   - Implement an instruction-quota baseline (Wang ISCA 2017-style) in SimX and beat it on lender loss at equal lane rate.
7. *"Vortex is not an H200."*
   - Reproduce the H200 fingerprint (X1 table, 1/n rule, depth independence) in calibrated SimX before showing any mechanism.
   - Show the real-GPU motivation and the software-feasible parts (g1, residency) on the H200 itself.
8. *"Power."* Driving HBM at 4.6 TB/s while the tensor cores are busy may lower clocks (Kuruppu et al.). Log clocks and power in X0 (sampled during the run); report energy per token and per step.
9. *"Isolation and security."* Co-residency shares L1, shared-memory banks and timing. Scope: same trust domain (one operator's two services, like MPS). Lane bits are set by the driver, not by tenants.
10. *"The bandwidth gain is tiny (4.5 vs 4.6 TB/s)."* Agreed. The gain is the compute job's SMs: frame every result as compute share at equal bandwidth.
11. *"Your own data show the effect only beside a synthetic FMA kernel; beside cuBLAS memory kept 0.63-0.78."*
    - Correct, and the paper must say so. The cuBLAS rows are block-scheduler interleaving (no shared SM; GEMM 0.25), not warp starvation.
    - X0 is the experiment that decides. Report it whichever way it comes out.
12. *"Fuse the two tenants into one kernel (POD-Attention, megakernels, `setmaxnreg`)."*
    - Fusion requires one binary, one launch and a shared lifetime. It also still relies on the hardware's issue arbitration (X0's fused variant tests whether a fused lane starves too).
    - The lane serves separate processes and services and gives a *guarantee*.
13. *"Decode at realistic batch is not bandwidth-only."* Agreed above B ≈ 16-32 (~0.62·B SM-equivalents of tensor throughput at full bandwidth). State the batch range and show the crossover.
14. *"The DiT does not care about losing 20% of its SMs."* Measured (type D). Show it as the case where disjoint is already optimal. The headroom claim is for type-P lenders only.
15. *"The baseline numbers are from short windows and idle clocks."* Rerun the key points (disjoint 24/32 SMs, shared MEM-first ILP 2) with 1 s windows and clocks sampled during the run, as part of X0's holder job.

---

## 6. Unverified and open

- The exact H200 issue policy at forced switches (X1); "oldest-first fallback" is INF from the order × ILP table only.
- The fused-kernel instruction count per transfer (taken as ~49 from the warprate loop).
- Loaded TMA latency at chip saturation (X7). The 35-70 KB range spans 1-2 µs.
- The cause of the ~10% lender loss with MEM first (X6).
- Whether TMA landing writes slow LDS (X5).
- Tensor-core idle time in production GEMMs (for (c2)).
- The ~57 GB/s per SM of GEMM TMA traffic is arithmetic from the tile shape, not measured.
- Literature items tagged "memory" in §2.
- The order × ILP table of `tpc-placement-and-borrowing.md` §C.2 does not match the raw files (no ILP-1 run; two cells differ). §1.3 uses the raw values; fix the source note.
- Whether the DiT is really type D: the MPS-percentage sweep did not read back SM counts. Repeat with TPC masks and `%smid`.
- The Qwen decode under MPS printed `'!!!!…'` in some runs (`framing-critique.md` item 2), so those deployment timings are suspect.
- How the cuBLAS co-run's GEMM fell to 0.25: wave quantisation plus memory blocks holding SMs is INF. A GEMM CTA trace with `%smid` would confirm it.
