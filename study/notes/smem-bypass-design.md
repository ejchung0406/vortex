# Feeding the tensor core without shared memory: is raising per-SM ingest worth it for decode?

Status: design study, 2026-11-21. Read-only: no code edits, no simulations and no GPU jobs
for this note. Tags: **FACT** = measured by us (file given) or read in code (file:line);
**LIT** = from a cited paper or vendor document; **INF** = my inference or arithmetic.
Clock 1.98 GHz throughout, so 1 B/clock = 1.98 GB/s per SM.

The two-outcome measurement (TMA into shared memory and loads into registers at the same
time on one SM, `gpu/l2s_run.py` `phase_mix`, "Part 2") had **not produced results** when
this note was written (no `notes/h200-l2-structure.md`, no `results_gpu/h200_l2s_mix.txt`
at 16:15). Section 2.3 covers both outcomes.

---

## 0. Answer first

**Verdict: not worth a proposal on its own.** Bypassing shared memory, or raising one SM's
ingest, is a secondary SM-side part of a bigger story. That story must also change the
tensor core's small-N operand path and has to be argued from the SMs it frees. It is not a
bandwidth win by itself. Three numbers carry the verdict:

1. **59 B/clock (117 GB/s per SM).** This is the most weight data the H200 tensor core can
   take in when the decode GEMM pads M to the 64-row wgmma tile. It is 3,786 FLOPs/clock ÷
   64, and the value is the same for FP16, BF16 and FP8 (§1). It is about half of the
   ingest ceiling (110 B/clock TMA, 120 B/clock loads). The best real kernels sit right at
   this compute bound:
   - cuBLAS BF16 at M ≤ 16 on 8-14 SMs: 107 GB/s per SM, 92% of the bound.
   - cuBLAS BF16 at M = 64: 57 GB/s, at the 128-row bound of 58.6.
   - CUTLASS FP8 at M = 16-128: 30 GB/s, about 53% of its 128-row FP8 bound. Its run time
     stays flat up to M = 128.
   
   **Today's decode kernels are tensor-core bound on padded rows, not ingest bound.** More
   ingest buys them nothing.
2. **110-140 B/clock: four ceilings that coincide.**
   - TMA write-only ingest: 110 B/clock (215 GB/s).
   - Plain 16-byte loads: 120 B/clock (239 GB/s).
   - Shared-memory reads: 127 B/clock. wgmma's shared-memory (SS) operand path at small N
     stops at about 112 B/clock.
   - The register (RS) operand path of wgmma at N = 8-16: about 136-141 B/clock (LIT,
     derived from Luo et al.'s wgmma-vs-N table).
   
   A kernel that does escape the padding (weights as the 64-row operand, tokens as N = 8-16,
   "swap-AB") hits the next ceiling at about the same height. So raising ingest alone gains
   **≤ 1.24×** (136/110). Removing the staging penalty gains **1.26-1.5×** (157-170 →
   215-239 GB/s). The register path can win part of that in software today (§3.0).
3. **98%.** A decode job spread at b = 4 SMs per GPC (32 SMs) already gets 4.56 of the
   chip's 4.63 TB/s.
   - Per-SM gains count only at b ≤ 2 per GPC.
   - The best realistic hardware gain (160 → 239 GB/s per SM) lowers the SMs needed for
     90% of the memory roof from about 32 to about 21-24. That frees ≈ 8-11 SMs, 6-8% of
     the chip.
   - Fixing the kernel first (107 → 160-239 GB/s per SM) frees ≈ 12-20 SMs and needs no
     silicon.
   - In our own co-run scenario the DiT keeps 1.01 of its throughput with 80% of the SMs
     (`corun-workloads.md`). With fixed kernels the decode job's 20% of the SMs
     (26 SMs, spread) already reach ~4.0-4.4 TB/s.

**What is worth doing:**
- (i) The software baseline: a swap-AB decode kernel, with RS operands fed by plain loads.
  On a real H200 it decides whether software already reaches the 215-239 GB/s ceiling.
- (ii) In Vortex, a modelling fix, not research: give the tile-buffer reads their own LMEM
  bank port. The banks are already 1RW+1R. This moves `gemm_tmem_decode` from 82 GB/s
  (port-bound) to its engine bound of 114 GB/s, a 1.39× gain.

If the direction is pursued, pair it with a GPC-level change or the memory-lane/isolation
idea. There the "per-SM ingest × SMs per GPC" product matters. §5 gives the smallest
experiment.

---

## 1. The arithmetic of a decode step on the tensor core

### 1.1 Rule

- Decode: C[M×N] = A[M×K] · W[K×N]. Each weight element is read once from DRAM and used
  for 2·M useful FLOPs.
- The tensor core does not run M rows. It runs **P padded rows**, where P is the
  tile's token dimension:
  - P = 64 for one wgmma warpgroup (m64nNk16).
  - P = 128 for the usual CUTLASS two-consumer tile.
  - P = 64 for Vortex's tc.mma, which always covers 64 lanes
    (`gemm_tmem_decode/kernel.cpp`: `static_assert(kBlocks * wg_cfg::xtileM == 64)`; lanes
    past M "compute on whatever shared memory holds").
- Weight bytes the tensor core can consume per clock (INF, exact arithmetic):

  **R = F_eff · b / (2 · P)**, where:
  - F_eff = FLOPs/clock actually reached per SM;
  - b = bytes per weight element;
  - P = padded token rows.

- H200 per SM: F = 3,786 FLOPs/clock for FP16/BF16 dense (989.5 TFLOPS / 132 / 1.98 GHz,
  `profiles/h200s.json` sources) and 7,572 for FP8.
- **For FP16 and FP8, F·b = 7,572 in both cases**, so FP8 doubles the elements per byte but
  not the bytes per clock. R = 3,786 / P B/clock at peak for either.

### 1.2 Table: weight bytes per clock the tensor core can consume, H200 SM

Ingest ceilings for comparison (FACT): staged 79-86 B/clock (157-170 GB/s); TMA
write-only 110 (215-219); plain 16 B loads 120 (239); shared-memory reads 127 (251-253).

| kernel form (P) | M = 1 | M = 4 | M = 16 | M = 64 | useful-FLOP fraction at M=1/4/16/64 |
|---|---|---|---|---|---|
| wgmma, W as B, M padded to 64 (P = 64), at peak | 59 B/clk = 117 GB/s | 59 | 59 | 59 | 1.6% / 6% / 25% / 100% |
| same, 128-row tile (P = 128) | 29.6 = 58.6 GB/s | 29.6 | 29.6 | 29.6 | 0.8% / 3% / 12.5% / 50% |
| swap-AB (W as the 64-row A, tokens as N, P = max(8, ⌈M⌉₈)), ideal | 473 | 473 | 237 | 59 | 12.5% / 50% / 100% / 100% |
| swap-AB, SS operands, measured small-N rate (LIT) | ~100 (smem-read bound) | ~100 | ~90 | 58 | |
| swap-AB, RS (A = W from registers), measured small-N rate (LIT) | ~136-141 | ~136-141 | ~136-141 | 58 | |
| W4A16 (INT4 weights, FP16 MMA), P = 64 | 14.8 = 29 GB/s | 14.8 | 14.8 | 14.8 | |
| W4A16, swap-AB RS | ~35 = 70 GB/s | ~35 | ~35 | 14.8 | |
| CUDA cores, GEMV (HFMA2 512 FLOPs/clk; FP32-acc 256) | 512 / 256 | 128 / 64 | 32 / 16 | — | |
| Vortex TMEM engine, tc.mma 64 × 512 × 16, 3,684 FLOPs/clk (FACT config) | 57.6 = 114 GB/s | 57.6 | 57.6 | 57.6 | |

How the swap-AB rows are derived (LIT → INF). Luo et al., arXiv 2501.12084 v2, Table 9,
H800 wgmma m64nNk16 FP16→FP32 dense, TFLOPS:

| N | SS | RS |
|---|---|---|
| 256 | 728.5 | 731.9 |
| 64 | 719.6 | 719.7 |
| 32 | 477.3 | 710.3 |
| 16 | 287.0 | 434.2 |
| 8 | 158.2 | 216.7 |

Reduced per SM at the paper's 1,755 MHz and 114 SMs:
- **SS** (A and B both from shared memory). One instruction reads A = 2 KB plus B = 32·N
  bytes. The rate comes out at 111 / 112 / 112 B/clock at N = 8 / 16 / 32. **The SS path
  at small N is bound by about 112 B/clock of shared-memory reads**, 88% of the 128 B/clock
  banks.
- **RS** (A from registers). Every instruction costs about 15 cycles at N = 8 and at
  N = 16, so A (= W) enters at 135-136 B/clock. The H200 per-clock scaling gives about 141.
  Whether this is an A-path width or a per-instruction cost cannot be told from the table.
  The consequence is the same either way: **swap-AB with RS can absorb ≤ ~136-141 B/clock
  of weights at N ≤ 16.**
- FP8 wgmma (k32) moves the same 2 KB of A per instruction. INF: if the per-instruction
  cost is the same, the B/clock cap is the same. The paper gives FP8 only at N = 256.

### 1.3 The measured kernels sit at the padded-compute bound, not at ingest

| kernel (real H200) | per SM | the bound from §1.2 | reading |
|---|---|---|---|
| cuBLAS BF16, M = 1-16, 8-14 SMs (MPS, `results_gpu/h200_cublas_lowprec.txt`) | 106-109 GB/s | P = 64: 117 | 92% of the padded-compute bound (INF: a 64-row tile) |
| cuBLAS BF16, M = 64, 8-14 SMs | 57-58 | P = 128: 58.6 | ~98% of the 128-row bound |
| cuBLAS FP16, M = 1-16, 8-14 SMs | 52 (39 at M = 1) | P = 128: 58.6 | different kernel choice; at the 128-row bound |
| cuBLAS FP16/BF16, all 132 SMs | 32-33 (4.23-4.37 TB/s) | chip roof 4.63/132 = 35 | memory-bound at the chip, not per SM |
| CUTLASS FP8 TMA+wgmma, M = 16..128, 8-66 SMs (`h200_cutlass_fp8.txt`) | 29-31 | FP8 P = 128: 58.6 at peak | **run time identical for M = 16, 32, 64, 128** (1.0895 ms at 14 SMs; doubles at 256). 3,980 FLOPs/clk/SM at M = 128 = 53% of FP8 peak. Compute-bound on its tile. |
| HF Qwen3.5-9B decode matmuls, 8 / 14 SMs (`corun-workloads.md`) | 104 / 88 | 117 | near the 64-row bound |
| Vortex `gemm_tmem_decode` today / with port budget (`upstream-pr-candidates.md` items 19, 27) | 82 / 114 | engine 114 | port-bound, then engine-bound (§2.4) |

**The crossover (INF).** Ingest-bound ⇔ R(P) > I. That gives P* = 3,786 / I:
- **P* ≈ 34** for TMA write-only (I = 110);
- **≈ 32** for loads (120);
- **≈ 44-48** for the staged path (79-86).

A wgmma kernel that pads M to ≥ 64 is always compute-bound for M < 64; for FP8 and FP16
alike that is ≤ 59 B/clock (117 GB/s). **Ingest only becomes the limit for swap-AB
kernels with N ≤ 32.** Those kernels then meet the operand-path caps of §1.2 (SS
~100-112, RS ~136-141 B/clock) almost at once.

The Hopper tensor core can therefore take in at most ~1.24× today's ingest at small N
through its best path. Raising ingest further needs a wider tensor-core operand path at
small N as well. That path is not memory-side hardware.

Quantization (FP8, INT4) lowers R. It makes decode more compute-bound per SM, not more
ingest-bound. W4A16 at P = 64 needs only 29 GB/s per SM.

---

## 2. Where the per-SM ceiling comes from

### 2.1 Measured edges (FACT; `tpc-placement-and-borrowing.md`, `open-threads.md` E1/E2, `h200_borrow_e*.txt`)

| edge | value | note |
|---|---|---|
| shared-memory reads alone | 251-253 GB/s = 127 B/clk (7-16 warps) | = 32 banks × 4 B; Luo et al. measure 127.4-127.9 B/clk/SM on H800 (LIT) |
| TMA writes alone | 205-219 GB/s ≈ 110 B/clk | needs ≥ 128-192 KB in flight (32 KB × 6, or several issuers) |
| staged (TMA write, warps read every word) | 157-170 (best); 100-170 by consumer warps | 79-86 B/clk each way: **158-172 B/clk of bank traffic > 128**. So writes and reads do not share one 128 B/clk port; they partly overlap |
| 16-byte loads to registers | 239 (2 × 1024 threads, 4 × 16 B in flight per thread); 115 with 1 × 1024 threads and 8 in flight (same 128 KB) | the load path depends on warp count, not bytes alone (INF: a per-warp or per-scheduler limit) |
| loads with the maximum shared-memory carveout | 62-67 | **loads land in the L1 data array**: 28 KB of L1 → 65 GB/s (Little: ≈ 430 ns) |
| 4-byte loads | ~110 | instruction-rate bound |
| L2-resident vs DRAM | same ceiling | a port limit, not a latency limit, at enough depth |
| one GPC | 1.28 TB/s ≈ 646 B/clk ≈ 5.4-6.2 SMs at the single-SM ceilings | no TPC or CPC level |
| chip | 4.63 TB/s | |

**Little's law for the landing buffer (INF from FACT points).**
- TMA, 16 KB × 1 / 2 / 4 / 8 transfers in flight: 30 / 57 / 104 / 142 GB/s.
- TMA, 32 KB × 1 / 2 / 3 / 6: 52 / 99 / 142 / 208 GB/s.
- Below saturation the bytes-in-flight / bandwidth ratio is 550-690 ns. **About 140 KB of
  landing space per SM is needed to run at 215 GB/s from DRAM.** That is 61% of the 228 KB
  of shared memory.
- From L2 (16 KB × 1 = 61 GB/s, 32 KB × 2 = 183 GB/s) the ratio is about 270-360 ns, so
  about 60-80 KB is needed.
- This is the central physical fact of the study: **shared memory (or the L1 for loads) is
  the latency-tolerance buffer.** No design can remove the buffer. A design can only remove
  the *second traversal* (write, then read through the same banks) or move the buffer
  elsewhere.

### 2.2 Inferred structure (INF; nothing here is documented by NVIDIA)

- **Bank read bandwidth, 128 B/clk.** Binds reads alone (127). It also binds wgmma SS at
  small N (112 = 88% of it).
- **TMA write path, ~110 B/clk.** Below the banks, so not bank-bound. It is the TMA
  engine's write datapath or the SM's response ingress. One TMA engine per SM (LIT, Bose et
  al.).
- **Write/read sharing.** Staged traffic of 158-172 B/clk exceeds one 128 B/clk port.
  Writes therefore have their own path, or a wider one, and conflict with reads only
  partly (bank-conflict cycles, MIO queue, mbarrier round trips). The 21-27% penalty
  (215 → 157-170) is what options (a), (b) and (d) can recover at most.
- **L1 fill path, ≥ 120 B/clk.** Loads reach 239 GB/s while also landing in the same
  unified SRAM. INF: fills are forwarded to registers, or written through a port that does
  not contend with the hit/read path as an LDS stream does. It is unknown whether the
  fill-plus-read of loads costs bank cycles the way TMA-write-plus-LDS does.
- **SM ingress port.** At least 120 B/clk (the larger of the two paths). If it is a
  128 B/clk crossbar port, the loads run at 94% of it.
- **GPC egress.** ~646 B/clk per GPC. This, not the SM, binds any job with ≥ 6 SMs in a GPC.

### 2.3 What the pending two-outcome measurement means for each design

The measurement runs TMA 32 KB × 6 together with 16-30 load warps, in one CTA or in two
CTAs, on SM 0.

**Outcome S: shared ingress.** TMA plus loads sum to ≈ 215-240 GB/s.
- The per-SM cap is the SM's L2-response port (~120 B/clk).
- (a), (b) and (d) can only recover the staging loss: 157-170 → ≤ 215-239, i.e. 1.26-1.5×.
  They never exceed ~239.
- (c) is the only option that lifts the cap. It needs (b) or (a) and a wider operand path
  as well, or it is wasted (§1).
- (f) is unchanged per SM.

**Outcome D: separate limits.** The sum is ≫ 240, e.g. 350-450.
- The ingress port is ≥ the sum. Each path is limited by its own machinery: the TMA
  engine's ~110 B/clk, and the load path's warp-dependent ~120.
- **A kernel that splits its weights between TMA and loads can reach the sum in software
  today.** That is a strong baseline that removes (c)'s motivation.
- The next binding point is then the consumer: the operand path, ~112 SS / ~136 RS B/clk.
- (a) and (b) become relevant because they free bank cycles for the consumer. (d) gains a
  real case: a second, independent landing path that feeds the tensor core.
- Either way the GPC cap (1.28 TB/s) is untouched.

In both outcomes the per-SM decode gain is capped by the tensor core's small-N operand
supply (§1.2) at ~136-141 B/clock ≈ 270-280 GB/s. The most any SM-side change can deliver
for decode is therefore ~1.15-1.3× over a well-written staged kernel's 215, or ~1.7× over
157-170.

### 2.4 Vortex today (FACT from code)

- **LMEM banks.**
  - With `NUM_LSU_BLOCKS > 1` (h200s: = ISSUE_WIDTH = 4, `VX_config.toml:154`) each bank
    is a `VX_1rw1r_ram` (`VX_lmem_unit.sv:857-874`).
  - **Port A** serves DMA, atomics and the first engine. **Port B** is read-only and is used
    *only* by the second LSU read engine (`VX_lmem_unit.sv:21-28`, `:844-855`).
  - The DMA port takes whole bank rows: 16 × 8 B = 128 B (`LMEM_DMA_DATA_SIZE`,
    `VX_gpu_pkg.sv:2034`).
- **One DMA port, two clients.** DXA writes and TCU tile-buffer reads are arbitrated onto
  one port (`VX_mem_unit.sv:86-129`). SimX does the same: one client per cycle,
  round-robin (`sim/simx/mem/local_mem.cpp` `dma_fire`, `core.cpp:196-198, 363-372`).
- **DXA write width.** `DXA_LMEM_WORD_SIZE` is a full 128 B row (`VX_dxa_pkg.sv:24`). The
  worker drains one 64 B L1 line per beat (`VX_dxa_smem_wr.sv`, one pending line;
  `L1_LINE_SIZE = MEM_BLOCK_SIZE = 64`, `VX_config.toml:92-99`), so a DXA write uses a full
  row slot for 64 useful bytes.
- **Port accounting** for `gemm_tmem_decode` (tc.mma 64 × 512 × 16, FP16; per 16 KB of W:
  2 KB of A by bank rows). Port slots per W byte (INF):
  - DXA write: 1/64;
  - B read: 1/128;
  - A read: (1/8)/128.
  
  The total is 0.0244 slots per byte → **41.0 B/clock = 81 GB/s. Measured: 82.** With a
  160 B/clock budget the kernel becomes engine-bound at 57.6 B/clock = 114 GB/s (measured
  114).
- **Ingress.** DXA ingress is one 64 B/clock gmem port (`noc-and-design-space.md` table;
  `VX_CFG_DXA_GMEM_PORTS`), 127 GB/s, at the lowest priority on socket port 0. The real
  part does ~110 B/clock. There is no SM-to-L2 network: four private 94-cycle links per SM.
  The GPC interaction cannot be modelled until `noc_model.patch` lands.

---

## 3. Design options

Common yardsticks:
- I_eff is the per-SM decode consumption a design allows, not the copy rate.
- Job bandwidth B(b) for b SMs per GPC, spread over 8 GPCs:
  B(b) ≈ min(8 · min(b · I_eff, 1,280), 4,630) GB/s, with the measured loaded-latency
  derate above ~3 TB/s.
- Measured anchors for the spread single-SM placement:

| I per SM | b = 1 (8 SMs) | b = 2 (16) | b = 4 (32) | kernel |
|---|---|---|---|---|
| 160 | 1,264 | 2,413 | 4,085 | S, staged |
| 205 | 1,622 | 3,090 | 4,557 | W, TMA write-only |
| 239 | 1,885 | 3,514 | 4,428 | L16, 16 B loads |

### 3.0 Baseline: do nothing in hardware, fix the kernels

Why real kernels sit at 16-109 GB/s per SM against a ~215 ceiling (FACT + INF):

1. **Padded tensor work (dominant).** M is padded to 64 or 128 rows, so the kernels are
   compute-bound at 59 or 29.6 B/clock (§1.3). The CUTLASS FP8 run time is flat from
   M = 16 to 128: direct evidence.
2. **Tensor efficiency on that tile.** CUTLASS FP8 runs at 53% of FP8 peak. Clock,
   schedule and tile choice are unexamined.
3. **Too few bytes in flight.**
   - A 4-stage 16 KB ring stops at 104 GB/s by Little's law, even if nothing else binds
     (FACT: 16 KB × 4 = 104).
   - Typical CUTLASS stage counts (4-7 stages of a 16 KB B tile, 64-112 KB) cap the ring
     at ~100-150 GB/s (INF).
4. **All-SM runs are chip-bound.** "16-20 per SM" (cuBLAS FP16, all SMs, 44-48% of peak)
   and the 32 per SM at 92% are chip-level figures. 4.63 TB/s / 132 = 35 per SM, so per-SM
   ingest is irrelevant there.
5. **Host overhead** (HF: ~20 of 27 ms per step) does not touch per-SM rates.

What a better kernel alone recovers (INF, to be measured):

| kernel | per SM | why |
|---|---|---|
| swap-AB, SS, TMA-staged, ≥ 140 KB in flight | **~157-197 GB/s** | staged ceiling (FACT 157-170) vs the SS small-N read cap of ~100 B/clk ≈ 197 GB/s |
| swap-AB, RS, A fed by `ldmatrix` from TMA-staged shared memory | ~157-170 | still staged |
| **swap-AB, RS, A fed by 16 B global loads straight into the wgmma A-fragment registers** (W pre-permuted offline, as Marlin does) | **up to ~239** | the load ceiling. The RS cap of ~270 GB/s does not bind. This **already bypasses shared memory for the weights.** Unmeasured. Risks: register budget (128 KB in flight ≈ 16 registers per thread at 2,048 threads, plus fragments) and whether LDG traffic and wgmma issue coexist at full rate |
| CUDA-core GEMV, M ≤ 2 | ~239 (load ceiling) | HFMA2 needs only ≥ 2 FLOPs/B |

The gain over today's best real decode kernel (107) is 1.5-2.2×, and 5-8× over CUTLASS
FP8. This is larger than any hardware option below. Cost: kernel work only. Prior art:
- FlashDecoding++ (MLSys'24) pads flat GEMMs to 8, not 64.
- Marlin (PPoPP'25) and Machete (Neural Magic, 2024) put weights in registers with offline
  permutation.
- Swap-AB for small-M FP8 GEMMs is used in production libraries (LIT, not re-checked here).

### 3.a TMA destination = tensor-core operand buffer ("push" into the tile buffer / a FIFO)

- **Data path.** L2 → SM ingress → DXA/TMA → **B (or A) operand FIFO in the TCU** → FEDP.
  The LMEM write and read go away.
- **Hardware.**
  - A descriptor bit "destination = TCU queue q".
  - Credit flow control from the TCU to the DXA.
  - A completion signal tied to consumption, not to the write.
  - The FIFO itself.
  - **The FIFO must be the landing buffer.** L2 responses return out of order, and Little's
    law (§2.1) needs ~140 KB from DRAM (60-80 KB from L2) at 215 GB/s.
  - The Vortex B tile buffer is 640 B at 32 threads with depth 2 (`upstream-pr-candidates.md`
    item 17). A 640 B FIFO sustains ≈ 1 GB/s; a 16 KB FIFO ≈ 30 GB/s (FACT: 16 KB × 1 = 30).
  - So (a) only works with a shared-memory-sized FIFO. That is a second shared memory with
    its own port, i.e. option (b) with less flexibility.
- **What happens to A.** The activations are small and reused across all N tiles. Keep them
  in LMEM, or in TMEM via `tc.cp` (Vortex already supports A from TMEM; `VX_tcu_tmem.sv:30-35`,
  `a_fill_*`). Only W streams.
- **Several CTAs.** The TCU (and its tile buffers) is per SM and runs one op at a time
  (`VX_tcu_tmem.sv:28-33`, one in-order queue). A push FIFO needs an owner. Either one
  streaming CTA per SM, or per-CTA queues with a static partition of the FIFO.
  Preemption/context switch must drain it.
- **Layout.** The FIFO is consumed in the engine's order.
  - Weights must be pre-packed block-major (LAYOUT_PACKED). Inference engines do this
    offline anyway.
  - K-major W[N][K] needs the DXA's K-major scatter, which drains one element per beat
    (`VX_dxa_smem_wr.sv:84-93`). That is 2-8 B per beat instead of 64: fatal for a stream.
- **ISA/runtime.** New descriptor mode; tc.mma operand "from queue q".
- **Gain.**
  - Vortex: removes the port bound (82 → 114, engine-bound) for the M-padded kernel. That
    is identical to (b) and needs the large FIFO.
  - H200-like: ≤ (b).
  - GPC: no change.
- **Cost.** 3-5 weeks of RTL + SimX (INF):
  - `VX_dxa_smem_wr.sv`, `VX_dxa_completion.sv`, `VX_tcu_bbuf.sv`/`VX_tcu_abuf.sv`,
    `VX_tcu_tbuf.sv`, `VX_mem_unit.sv`;
  - SimX `dxa/dxa_core.cpp`, `tcu/tcu_tbuf.cpp`.
- **Risks.** The buffer-size argument above. Deadlock between credit and completion.
  Partial transfers. Multicast.
- **Prior work.**
  - D2MA (PACT'14) decouples address generation and lands in shared memory.
  - Decoupled access/execute (Smith, ISCA'82) uses architectural queues, whose depth must
    cover memory latency: the same Little's-law constraint.
  - Imagine/Merrimac stream register files are exactly a large landing buffer.
- **Verdict.** Dominated by (b).

### 3.b Dual-ported or split shared memory (writes and reads in separate bank cycles)

- **Data path.** Unchanged (L2 → TMA → shared memory → tensor core). Writes and operand
  reads use different bank ports.
- **Hardware.** Variants:
  - (b1) a 1RW+1R bitcell. INF: about 1.3-1.5× bank area for an 8T-class cell; a rule of
    thumb, UNVERIFIED for this process.
  - (b2) split the banks into halves with independent ports, with the stage ring alternating
    halves. No cell cost; the software or allocator places stages.
  - (b3) more banks.
- **Vortex already has (b1):** port B exists and is idle except for the second LSU read
  engine. The change is routing, not cells.
- **ISA/runtime.** None for b1/b3. b2 needs a placement rule (allocator or descriptor hint).
- **Gain.**
  - H200-like: staged 157-170 → up to the write ceiling of 215, i.e. 1.26-1.37× for swap-AB
    SS kernels. Still capped by the SS small-N read rate (~100 B/clk ≈ 197 GB/s), so
    realistically 1.15-1.25×.
  - Nothing for M-padded kernels, which are compute-bound at 117.
  - Vortex, M-padded `gemm_tmem_decode`: port A carries 1/64 slot per byte (64 B/clk), port
    B 0.0088 (113 B/clk). The engine binds at 57.6 B/clk: **82 → 114 GB/s (1.39×).** This
    equals the "160 B/clock budget" result, now with a physical meaning.
  - Vortex swap-AB kernel: port B reads ~102 B/clk with 128 B DXA beats. Then the DXA gmem
    port binds (64 B/clk = 127 GB/s; ~184-188 with the 2-port experimental set).
- **GPC.** Per SM only; matters at b ≤ 2 (+25-35% job bandwidth at b = 1-2, ≤ +3% at b = 4).
- **Vortex changes (b1), about 1-2 weeks including rtlsim parity (INF):**
  - `VX_lmem_unit.sv`: second DMA client on port B, arbitrated with `b_bank_*` of the read
    engine;
  - `VX_mem_unit.sv:86-129`: two DMA interfaces instead of one arbiter;
  - `local_mem.cpp` `dma_fire`: let the TCU client fire on the B side in the same cycle as
    a DXA write;
  - optionally 128 B DXA write beats (`VX_dxa_smem_wr.sv`, already among the calibration
    knobs in `dxa_lmem_calib.patch`).
- **Risks.**
  - Port B contention with the LSU read engine (`ldmatrix`-like consumers).
  - Same-cycle read-during-write on a row. Ordering is by mbarrier, but the RDW mode of
    `VX_1rw1r_ram` must be checked.
  - Synthesis cost unmeasured.
- **Prior work.** Banked/dual-ported scratchpads are standard. NVIDIA's partial overlap
  (157-170 > 128) suggests the part already has something like this.

### 3.c A second or wider SM ingress for a designated SM class ("fat-pipe" SMs)

- **Data path.** A few SMs per GPC get 2× L2-response ports (and a 2× TMA write path) to
  the GPC crossbar.
- **Hardware.**
  - Crossbar radix +1 per fat SM per GPC.
  - TMA engine ×2.
  - **The consumer side must follow**: (b), plus a small-N tensor operand path ≥ 2×
    (§1.2). Otherwise I_eff stays ≤ 136-141 B/clk.
- **ISA/runtime.** None in the ISA. The runtime must place memory-bound kernels on fat SMs
  (TPC masks or green contexts already allow this; `tpc-placement-and-borrowing.md` Part A).
- **Gain.**
  - Copy: up to 2× per SM.
  - Decode with today's operand paths: ≤ 1.24× (136/110).
  - With a doubled operand path: I ≈ 430. Then b = 1 → ~3.4 TB/s (vs 1.62); b = 2 → chip
    roof (vs 3.09); b = 4 → no change.
  - SMs needed for 90% of the roof: ~24 → ~12-14.
- **GPC.** One fat SM at 430 uses a third of a GPC's 1.28 TB/s, so b ≤ 2 fat SMs per GPC
  stay under the cap.
- **Cost.** Wiring and crossbar ports in every GPC. Idle when no memory-bound job runs.
- **Vortex.** Per-core port counts do not exist (uniform profile; `VX_CFG_L1_MEM_PORTS`
  per socket). This needs per-socket parameters in `VX_socket.sv`/`socket.cpp` and the NoC
  model to show the GPC. 2-3 weeks after `noc_model.patch`.
- **Risks.**
  - Outcome D (§2.3) would make it unnecessary.
  - Value depends entirely on also widening the tensor operand path.
- **Prior work.** None found for GPUs (`related-work-placement.md` §9-10). Heterogeneous
  "big-pipe" cores exist in CPU SoCs; not checked.

### 3.d Tensor core reads operands directly from L1/L2 lines (a wide "load-operand" tensor op)

- **Data path.** L2 → L1 (allocate, fill-forward) → TCU operand buffer. Registers and
  shared memory are skipped.
- **Hardware.**
  - A TCU→L1 request port (Vortex already has one for sparse metadata: the TCU AGU binds to
    the LSU's `TcuReqIn`, `sim/simx/core.cpp:374-383`, `TCU_META_ENABLE`).
  - Line-to-fragment routing.
  - The landing buffer is the L1: ~140 KB at 215 GB/s. That competes with the shared-memory
    carveout (FACT: max carveout → 62-67 GB/s for loads).
- **ISA/runtime.** A new tensor op with a global address descriptor. CUDA's
  `wmma::load_matrix_sync` from global memory is the narrow software ancestor; it compiles
  to loads into registers.
- **Gain.**
  - About the load ceiling (239), i.e. the same as the §3.0 RS+LDG software kernel, but
    without the register cost.
  - Outcome D (§2.3) adds a separate landing path: TMA for one operand, L1 for the other.
- **GPC.** Per SM.
- **Cost.**
  - Vortex: 3-4 weeks (`VX_tcu_tbuf.sv`/`VX_tcu_bbuf.sv` request path, dcache client,
    SimX `tcu_tbuf.cpp`, `lsu_unit.cpp`).
  - The Vortex dcache is 4 × 32 B banks = 128 B/clk, shared with all loads.
- **Risks.**
  - L1 thrash by a stream.
  - Out-of-order line returns into a fixed operand order (needs an L1-resident reorder).
  - It is the software RS+LDG kernel in hardware. If that kernel reaches ~239, (d) only
    saves registers.

### 3.e Compressing or packing weights (FP8, INT4, MX formats): the software alternative reviewers will raise

- **Effect.** Bytes per token fall 2× (FP8) or 4× (INT4/FP4), so tokens/s rise 2-4× at the
  same ingest. This beats every option above by a wide margin.
- **Interaction.** It *lowers* the per-SM ingest a decode kernel can use (§1.2):
  - W4A16 at P = 64 needs 29 GB/s per SM;
  - W4A16 swap-AB RS needs ~70 GB/s per SM.
  - The kernel becomes tensor-issue-bound and dequant-ALU-bound. Dequant at 110 B/clk of
    INT4 is 220 elements/clk, close to the 128-lane integer ALU's rate (INF).
- **Hardware variants.**
  - Native low-bit operands in the tensor core: Blackwell tcgen05 FP4/MX kinds (LIT).
    Vortex has MXFP8/MXFP4/NVFP4/INT4 formats in the TCU (`VX_tcu_pkg.sv:34-37, 210-222`;
    disabled by default, `VX_config.toml:446-451`).
  - Dequant in the DXA is the wrong place: it multiplies shared-memory traffic by 2-4.
- **Verdict.** It is the strongest software argument against "raise ingest". Ingest gains
  multiply on top of quantization only for swap-AB kernels with N = 8 and native low-bit
  MMA.

### 3.f L2-side operand staging (per-GPC stream buffer feeding an SM's tensor core)

- **Data path.** DRAM → L2/GPC stream buffer (a run-ahead descriptor engine) → SM ingress
  → small SM landing → TCU.
- **Hardware.** A per-GPC SRAM buffer (≈ DRAM latency × the GPC's 1.28 TB/s ≈ 0.8-1 MB per
  GPC, INF) and a descriptor engine. The SM landing then only covers the GPC hop: 60-80 KB
  instead of ~140 KB.
- **Gain.**
  - **None per SM.** The ceiling is a port, not latency: L2-resident data hits the same cap
    (FACT).
  - What it buys is **shared-memory capacity**. A memory job needing ~60 KB instead of
    ~140 KB can co-reside more easily.
  - But production GEMMs also use 98% of registers (FACT), so co-residency stays blocked.
- **GPC.** It sits at the GPC level. It can only help a GPC-egress change, by prefetching
  ahead of a widened egress.
- **Cost.** Large: new L2-side engine, coherence and eviction policy.
- **Vortex.** Needs the NoC model first.
- **Verdict.** Not a per-SM ingest design. Possibly a component of a GPC-level proposal.

### 3.g Summary table

I_eff is per SM for decode GEMM consumption. B(b) is job bandwidth in TB/s for b SMs per
GPC, spread over 8 GPCs.

| option | I_eff (GB/s) | B(1) | B(2) | B(4) | ISA visible | Vortex effort | main risk |
|---|---|---|---|---|---|---|---|
| today, real kernels | 30-109 | 0.24-0.87 | 0.48-1.74 | 0.96-3.4 | — | — | — |
| 3.0 kernel fix, swap-AB SS staged | 157-197 | 1.26 | 2.41 | 4.09 | no | 1 wk (kernel) | small-N wgmma rate |
| 3.0 kernel fix, swap-AB RS + loads | ≤ 239 (unmeasured) | 1.89 | 3.51 | 4.43 | no | — (H200 kernel) | registers, issue overlap |
| 3.b dual-port + SS swap-AB | ~197-215 | ~1.6 | ~3.1 | ~4.5 | no | 1-2 wk | port-B arbitration |
| 3.a push to operand FIFO | ≤ 3.b | ≤ 1.6 | ≤ 3.1 | ~4.5 | yes | 3-5 wk | FIFO = shared memory size |
| 3.c fat ingress, operand path unchanged | ≤ 270 | ~2.1 | ~4.0 | 4.6 | no | 2-3 wk + NoC | operand path caps it |
| 3.c fat ingress + 2× operand path | ~430 | ~3.4 | 4.6 | 4.6 | no | > 6 wk | cost; idle when unused |
| 3.d load-operand tensor op | ~239 | 1.89 | 3.51 | 4.43 | yes | 3-4 wk | = software RS+LDG |
| 3.e FP8 / INT4 | ingest need ÷ 2-4 | tokens/s × 2-4 | | | no | exists in TCU | accuracy |
| 3.f GPC stream buffer | unchanged | — | — | — | yes | large + NoC | not per SM |

SMs needed for ~90% of the 4.63 TB/s roof (spread):
- today's best kernels (107): ~40-44;
- staged kernel fix (160): ~32;
- 215-239: ~21-24;
- full 2× (430): ~12-14.

---

## 4. Related work

**Tensor-core and matrix-unit operand supply.**
- **Hopper wgmma** (PTX ISA, LIT):
  - A comes from registers or a shared-memory descriptor; B from a shared-memory descriptor
    only.
  - M is fixed at 64 per warpgroup; N is 8-256 in steps of 8.
  - For decode the weights must therefore be B (in shared memory, padded M) or A
    (swap-AB, registers or shared memory).
  - There is no global-memory operand.
  - Measured small-N rates: Luo et al., arXiv 2501.12084, Table 9 (§1.2).
  - mma.sync reaches on average 62.9% of peak on Hopper: Luo et al., arXiv 2402.13499.
- **Blackwell tcgen05 + TMEM** (LIT):
  - A comes from TMEM or shared memory, B from shared memory. Accumulators are in TMEM.
  - `tcgen05.cp` fills TMEM *from shared memory*; `tcgen05.st` fills it from registers.
  - So a registers→TMEM path exists (a loads-based bypass is possible in principle), but
    the TMA→operand path still goes through shared memory. The vendor kept shared memory as
    the landing buffer in two generations.
  - Vortex's TMEM engine follows this structure (`VX_tcu_tmem.sv`).
- **FIBER** (Liu, Leng, Zhou, Ding et al., arXiv 2608.19628, per `related-work-placement.md`):
  thread-register decoupled execution as "a redundancy-free alternative for matrix operand
  supply" (1.8× Hopper, 2.09× Blackwell, simulated). Operands come from the register file;
  it targets operand redundancy and parallelism, not SM ingest.
- **Virgo** (ASPLOS'25, arXiv 2408.12073; built on Vortex in Chipyard): moves the matrix
  unit to the cluster level, fed from the cluster's shared memory, for operand reuse and
  energy (−67.3% / −24.2% active power vs Ampere- and Hopper-style). It *strengthens* shared
  memory as the operand source. It is the closest prior work on "where the matrix unit
  reads from" and needs reuse; decode weights have none.
- **CPU matrix extensions** (LIT, general knowledge, not re-checked):
  - Intel AMX `TILELOADD` loads tile registers from memory through the L1.
  - Arm SME loads directly into ZA tile slices (LD1 to ZA).
  - Both "bypass" a scratchpad, but their latency tolerance comes from hardware prefetchers
    into L1/L2 and from out-of-order cores. That is option (f)'s idea on a CPU.

**Asynchronous copy and DMA into the SM.**
- **D2MA** (Jamshidi, Samadi, Mahlke, PACT'14): data-parallel DMA that decouples address
  generation and gives "a more direct path ... into the shared memory", with transparent
  synchronization; up to 2.29×. The ancestor of TMA/DXA; it lands in shared memory.
- **CudaDMA** (Bauer, Cook, Khailany, SC'11): DMA warps (warp specialization) staging into
  shared memory. The software ancestor of producer/consumer TMA kernels.
- **Ampere cp.async (LDGSTS)** removed register staging. **Hopper TMA** removed address
  generation. Neither removed shared memory.
- **TMA performance**: Bose, Avalos, Pan, Rogers, arXiv 2610.34907. Whole-GPU only; a
  2-3× per-issue latency tax below 8 KB; "one TMA engine per SM".
- **Decoupled access/execute** (Smith, ISCA'82) and **stream processors** (Imagine,
  MICRO'98; Merrimac, SC'03): architectural queues or a stream register file between
  memory and compute. Their size is set by memory latency × bandwidth: the same constraint
  that sinks 3.a with a small FIFO.

**Decode kernels.**
- FlashDecoding++ (Hong et al., MLSys'24): flat-GEMM padding to 8 instead of 64,
  double-buffered.
- Marlin (Frantar et al., PPoPP'25): INT4 weights, offline-permuted, register-resident
  operands, near-ideal speedup up to batch 16-32.
- Machete (Neural Magic, 2024): Hopper mixed-input with weights as the wgmma RS A operand.
- These are the software baselines a reviewer will cite for 3.0 and 3.e.

**Not found.** No paper reports or attacks a per-SM DRAM ingest ceiling; no hardware
proposal streams DRAM data into a tensor core without a shared-memory-sized landing buffer
(`related-work-placement.md` §9-10, searches listed there).

---

## 5. Verdict, and if pursued: first SimX prototype and smallest experiment

**Verdict.** Not a direction worth a proposal on its own. The numbers:

1. **Today's decode kernels are not ingest-bound.** The padded tensor core consumes
   ≤ 59 B/clock (117 GB/s per SM) for FP16 or FP8, half the ingest. Real kernels sit at
   that bound (107 / 57 / 30 GB/s for 64- and 128-row tiles).
2. **The shared-memory bypass is mostly a kernel change, and capped.**
   - Escaping the padding (swap-AB) is a kernel change.
   - Once escaped, the tensor core's small-N operand paths (~100-112 B/clock SS,
     ~136-141 RS) coincide with ingest (110-120). So raising ingest alone gives ≤ 1.24×.
   - Removing the staging penalty gives 1.26-1.5×. Loads straight into RS operands may
     already get it in software.
   - Shared memory cannot be removed, only its second traversal: ~140 KB of landing space
     is needed at 215 GB/s.
3. **The GPC/chip limits leave little room.**
   - At b = 4 per GPC the job already gets 98% of the roof.
   - The realistic hardware gain frees ~8-11 SMs (6-8%) versus a fixed kernel.
   - Fixing the kernel frees 12-20 SMs for free.
   - In the measured co-run (the DiT keeps 1.01 at 80% of the SMs), fixed kernels on 20%
     of the SMs already approach the roof.

**Where it can still belong.**
- As the SM-side half of a GPC-level or isolation proposal: per-SM ingest × SMs per GPC
  is what lets a memory job live on few SMs. It only pays if three things are doubled
  together: ingest, a write/read-separated landing buffer (3.b), and a wider small-N
  operand path.
- Make the case in "SMs returned to the co-running compute job", not in GB/s per SM.

**In Vortex.**
- Give the tile-buffer reads their own LMEM port (3.b). The banks already have port B. Treat
  it as a calibration/modelling fix. It makes Vortex's staged ingest behave like the part's
  partial write/read overlap instead of full exclusivity.
- Do not build 3.a or 3.d.

**First SimX prototype (about 1 week; knobs default off; rtlsim parity later):**
1. `VX_CFG_LMEM_TCU_PORT_B`: in `sim/simx/mem/local_mem.cpp`, let the TCU DMA client fire
   on the B side in the same cycle as a DXA write on A, arbitrated with the second read
   engine. Wire it in `core.cpp:363-372`.
2. 128 B DXA write beats (the existing calibration knob from `dxa_lmem_calib.patch`).
3. A swap-AB variant of `gemm_tmem_decode`:
   - W as the 64-row A operand, read by bank rows;
   - tokens as B with TILE_N = 16;
   - first measure tc.mma FLOPs/clock at N = 16 in `tcu_roof`. The engine must reach
     ≥ ~1,600 FLOPs/clock (43%) not to bind.
4. Predicted (INF, from the port accounting in §2.4):

| kernel | today | port B | port B + 128 B beats | then binding |
|---|---|---|---|---|
| M-padded `gemm_tmem_decode` (GB/s per SM) | 82 | 114 (engine) | 114 | engine |
| swap-AB | ~78 (port) | | ~127 | 64 B/clock DXA gmem port; ~184-188 with 2 gmem ports |

   - Run with the load-staged variant too (study rule), at 1/2/4/11/22 SMs. 22-SM totals
     should not move (memory-bound, 729 GB/s).

**Smallest decisive experiment (real H200, 1-2 days, before any RTL).**
- A one-SM swap-AB wgmma decode microkernel, M = N = 16, K = 8192, FP16, W pre-permuted,
  in three variants:
  - (i) SS with TMA staging, ≥ 140 KB in flight;
  - (ii) RS with `ldmatrix` from TMA-staged shared memory;
  - (iii) RS with 16 B global loads straight into A fragments.
- Report GB/s per SM and tensor utilization, with the TMA-only and loads-only copies as
  controls.
- Reading the result:
  - (iii) ≥ ~200 GB/s: software already bypasses shared memory; the hardware direction has
    ≤ 1.1× left. Drop it.
  - (iii) ≈ (i) ≈ 157-170: staging (bank sharing) binds everyone, and 3.b has a measured
    1.3× case at b ≤ 2 per GPC.
  - All three ≤ 120: the small-N tensor path binds, and the proposal would have to be about
    the operand path, not memory.
- Together with Part 2 (shared vs separate ingress) this settles the question.

---

## 6. Diagram specifications

**D1: SM ingest data path with measured ceilings (block diagram).**
- Boxes left to right:
  - HBM (4.63 TB/s chip);
  - L2 slices;
  - GPC crossbar/egress (1.28 TB/s per GPC, ≈ 6 SMs);
  - SM ingress port (≥ 120 B/clk, "shared or separate? Part 2");
  - splitting into two paths:
    - (upper) TMA engine → shared-memory banks (write ~110 B/clk), then banks → wgmma SS
      operand read (127 B/clk alone; ~112 B/clk at small N);
    - (lower) L1 fill (landing; carveout-dependent) → register file (loads 120 B/clk) →
      wgmma RS operand (~136-141 B/clk at N ≤ 16).
- A red bracket over "banks" labelled "staged write+read: 79-86 B/clk each (157-170 GB/s)".
- Tensor core box labelled "consumes ≤ 59 B/clk at M padded to 64; ≤ 29.6 at 128".
- Option letters (a)-(f) as dashed alternative arrows:
  - (a) TMA → tensor-core FIFO;
  - (b) second bank port;
  - (c) second ingress arrow;
  - (d) L1 → tensor core;
  - (f) a buffer box at the GPC.
- Colour: measured edges solid, inferred dashed.

**D2: Consumption vs padding ("decode roofline").**
- x: padded token rows P (8, 16, 32, 64, 128; log scale).
- y: weight B/clock per SM (0-500).
- Curve R = 3,786/P (ideal), plus measured SS and RS points at N = 8/16/32/64 from Luo
  Table 9 (scaled).
- Horizontal bands:
  - staged 79-86;
  - TMA 110;
  - loads 120;
  - shared-memory read 127;
  - RS cap ~136-141.
- Markers for the real kernels at their P:
  - cuBLAS BF16 107 GB/s → 54 B/clk at P = 64;
  - cuBLAS M=64 → 29 at P = 128;
  - CUTLASS FP8 → 15.5 at P = 128;
  - Vortex 41 → 57.6 at P = 64.
- Shade "ingest-bound" (left of the crossover P* ≈ 32-48) vs "compute-bound".

**D3: Job bandwidth vs SMs per GPC (line chart).**
- x: b = 1, 2, 3, 4, 6 SMs per GPC (8 GPCs).
- y: TB/s (0-4.8).
- Lines for I = 107 (today's best), 160 (staged fix), 215 (3.b), 239 (RS+LDG / 3.d),
  430 (3.c + operand path).
- Measured anchors as dots (W, S, L16 tables). Dashed ceilings at 4.63 (chip) and 8 × 1.28
  (GPC).
- Annotate "per-SM gains vanish at b ≥ 4".

**D4: Vortex LMEM port slot accounting (stacked bars).**
- One bar per configuration:
  - today;
  - 128 B beats;
  - TCU on port B;
  - both;
  - swap-AB today;
  - swap-AB with both.
- Stacks: DXA write slots, B-read slots, A-read slots per KB of W. The resulting B/clk is
  written on top: 41.0 / 60.2 / 64→engine 57.6 / engine 57.6 / ~39 / ~100→gmem-port.
- A horizontal marker at the engine bound of 57.6 for the M-padded kernel.

**D5: Little's law, the landing buffer (scatter + line).**
- x: bytes in flight per SM (log; 640 B to 256 KB).
- y: GB/s.
- Measured TMA points: 16 KB × 1/2/4/8 and 32 KB × 1/2/3/6 (DRAM); L2-resident points.
- Fitted lines at 600 ns (DRAM) and ~300 ns (L2), capped at 215.
- Vertical markers:
  - Vortex B tile buffer 640 B;
  - 16 KB FIFO;
  - L2-hop landing 60-80 KB;
  - DRAM landing 140 KB;
  - 228 KB of shared memory.
- Caption: "a tensor-core FIFO must be shared-memory sized".
