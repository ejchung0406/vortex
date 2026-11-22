# H200 bystander effect: what triggers it, where the shared resource is, and a model

Date 2026-11-21.
- GPU: one H200 on atl1-1-02-014-9-0 (GPU-c0239183-...). This is a different die
  from the L2-structure study, which used atl1-1-02-014-16-0.
- Holder job 15985900 `bys_h200`: 21:03:11 → cancelled 22:15 (1 h 13 min).
- Software: driver 615.71.09, CUDA 12.9.1.
- Tags: FACT = measured here; INFERENCE = my reasoning; UNVERIFIED = not checked.
  Prior-work statements are limited to what `notes/related-work-placement.md` and
  `notes/gpu-noc-sources.md` record.

## 0. Answer first

**What triggers it.** A *backlog of unique-line requests* from one GPC whose
outstanding demand exceeds its 1.28 TB/s port. Bytes flowing is not the trigger;
requests queued are.
- At the same delivered rate (1.27-1.28 TB/s from GPC 6), same-half bystanders
  get:
  - 174 GB/s with 8 aggressor SMs (at the knee) and 88 with 16 (over-subscribed);
  - 168 GB/s at TMA depth 3 and 88 at depth 6, with 16 SMs.
- Pacing the aggressor to 95% of its limit (1.22 TB/s, below the cap) gives the
  bystanders 0.85-0.90×. At 100% they fall to 0.49-0.56×.
- A shared-buffer aggressor fails to hurt anyone:
  - its 16 SMs request flat out and the GPC delivers 2.30 TB/s;
  - but few *unique* lines go downstream, because same-line reads merge in the
    GPC;
  - bystanders stay at 0.99× (FACT).
- Not a clock or power artefact (FACT):
  - the SM clock is 1980 MHz with and without the aggressor, and the
    clock-event reasons are 0;
  - forcing 1800 MHz with HMMA on 118 other SMs costs bystanders 6% in GB/s at
    constant bytes per clock;
  - with HMMA beside the aggressor at 1800 MHz, bystanders get exactly the same
    88/105 GB/s as at 1980 MHz;
  - an aggressor at 10% duty (128 GB/s average) still costs TMA bystanders
    their proportional share, the same at 0.1 ms/1 ms and 1 ms/10 ms.

**Where the shared resource is.** It is in the L2 slices (or the crossbar input
they feed) that *serve the aggressor's data*. It is not the DRAM or memory
controllers, not the cross-partition fabric, and not a clock or power effect.
- Evidence chain (FACT unless tagged):
  1. A DRAM-free aggressor (L2-resident, 256 KB per SM) cuts same-half
     bystanders to 89 GB/s (0.45×), the same as a DRAM aggressor. So the queue
     is not at the memory controllers.
  2. L2-resident bystanders are cut to 88.5 GB/s (0.43×) by either aggressor.
     So the hit path suffers too.
  3. Harm follows the slices that *source* the aggressor's data:
     - aggressor reading only its own half's pages: same-half local readers
       0.57×, other-half local readers 1.00×;
     - aggressor reading only the other half's pages: other-half local readers
       0.64×, same-half local readers 0.96×;
     - an L2-resident aggressor (data replicated into its own half) leaves the
       other half untouched (0.99-1.00×).
  4. The fabric is not the queue:
     - same-half harm is complete with an L2-resident aggressor, which sends
       nothing across;
     - ncu fabric→LTS stall cycles are the same with and without harm (17% of
       cycles), and highest in the harmless paced case (32%).
  5. It is downstream of the GPC's same-line merge: shared-buffer aggressor →
     no harm (point 1 of the trigger).
  6. Slice-set dependence, partial: in one 2 MB page, an aggressor confined to
     GemAtlas f0 = 0 granules hurts bystanders on the same f0 half 0.45× and on
     the other half 0.73×. The s0 bit does not separate (0.58 / 0.60).
  7. Not bandwidth exhaustion at the slice output: ncu `lts2xbar` is active
     ~52% of cycles with or without the aggressor (max slice 0.71). It is
     proportional to bytes moved (0.33 M cycles per GB in all cases).
- INFERENCE (what remains inference): responses for the over-subscribed GPC
  occupy the slice-output / crossbar-input buffers of the slices that source
  them. The congested GPC's port returns credits at 1.28 TB/s, so those buffers
  stay full. Requests from every other SM to those slices then wait in the same
  shared queue (head-of-line or shared-credit blocking), and each requester gets
  a share in proportion to its outstanding requests.
  - Which exact buffer (slice output FIFO, crossbar input, or slice input
    credits) cannot be told apart from outside. No exposed ncu counter shows its
    occupancy.

**Model (fits the dose-response data).** For a bystander in the same half as an
over-subscribed GPC:
- `L_agg = n_agg · W_agg / C_del`, the aggressor's Little's-law latency
  (W = bytes in flight per SM; C_del = the GPC's delivered rate, ~1.28 TB/s);
- `r_by = min(r_free, W_by / (L_agg − δ))`, with **δ = 0.22 µs** (fitted; 6
  points range 0.19-0.24).
- Accuracy:
  - within ±2% for n = 10-16 aggressor SMs, depth 4/6, and 16 B-load bystanders
    beside a 16 B-load aggressor;
  - −5% for depth-1 TMA bystanders;
  - over-predicts near the knee (n = 8 +10%, depth 3 +15%) and for 16 B-load
    bystanders beside a TMA aggressor (+21%).
- In words: a same-half bystander's requests take as long as the aggressor's
  own, less 0.22 µs. Bystanders and aggressor SMs with equal windows get nearly
  equal per-SM rates (88 vs 80 GB/s).
- Other half: `r_by = W_by / (f·(L_agg − δ) + (1 − f)·W_by/r_free)`, where f is
  the fraction of the bystander's requests served by the congested half.
  - Fitted f = 0.44 for 16 B loads (4 KB page-pure chunks; the geometric value
    is 0.5).
  - Fitted f = 0.69-0.84 for TMA (a 32 KB tile spans 8 pages and waits for its
    slowest part).

**Cheap fix indicated: yes.** In software it is an in-flight budget, not a rate
limit (FACT):
- Capping the aggressor at 3 × 32 KB per SM (1.5 MB per GPC ≈ C × 1.2 µs):
  - costs it 0.5% (1272 vs 1278 GB/s);
  - lifts bystanders from 0.45/0.54× to 0.86/0.92×.
- Rate pacing to 95% costs 5% for 0.85/0.90×.
- Spreading the same 1.28 TB/s over 2 or 4 GPCs costs nothing and gives
  0.89-0.91×.
- The residual ~10% is the general-load loss also seen at 1.2 TB/s with an
  unsaturated aggressor (0.91×).
- Latency-bound streams recover less (one load at a time: 0.64-0.70× at 90-95%
  pacing).
- This is the M8 "admission by return-credit budget" (gpc-limit-design.md)
  measured in software form. A hardware version would cost the over-subscribed
  GPC < 1%.

**Practical size.** With a decode-like lane (16 B loads on 2 SMs in each of the 8
GPCs) beside a tenant packed into one GPC:
- the lane runs at 0.31× (60 vs 194 GB/s per SM);
- the chip moves 2.15 TB/s against an ideal 4.39 TB/s: 51% of the sum is lost.
- With two packed GPCs, 31% is lost against the 4.6 TB/s roof.

**The three numbers it rests on**
1. Same delivered 1.27-1.28 TB/s, different backlog: same-half bystanders get
   **168 GB/s at aggressor depth 3 vs 88 GB/s at depth 6**; 174 vs 88 for 8 vs 16
   SMs.
2. A **DRAM-free (L2-resident) aggressor still cuts same-half bystanders to
   89 GB/s (0.45×)**, and L2-resident bystanders to 88.5 (0.43×).
3. A **shared-buffer aggressor delivering 2.30 TB/s leaves bystanders at 0.99×**.
   Only the backlog of unique lines downstream of the GPC hurts.

## 1. Method (FACT)

- Tool: `gpu/bys_lab.cu`, built into `/tmp/$USER/bys` by `gpu/bys_build.sh`.
  - Plans: `gpu/bys_plan.py`. Runner: `gpu/bys_phase.sh` via `gpu/bys_srun.sh`.
  - Tables: `gpu/bys_report.py` and `gpu/bys_analyze.py`. Model fit:
    `gpu/bys_model.py`.
  - Raw outputs: `results_gpu/h200_bys_<phase>.txt`. Plans as run:
    `results_gpu/h200_bys_plans.txt`.
- One process per phase:
  - it allocates 24 GiB (a[i] = i) and classifies the L2 home side of all 12,288
    2 MB pages in-process: SM 0 warms 16 lines per page, then SM 2 (other side)
    times dependent loads;
  - a far hit means "homed on SM 0's side"; the within-page hash
    PA12⊕13⊕15⊕17⊕19 is applied and the page constant voted;
  - 12,272-12,280 pages agree 16/16, ≤ 1 page unresolved, 0 errors (every phase).
- Scenario = one launch:
  - each SM of each group runs one worker CTA with the group's role, placed by
    a TPC mask; `%smid` of every unit is written back and checked;
  - common globaltimer start; fixed 1.0 s window; only bytes completed inside
    the window count;
  - disjoint buffers per SM: 64 MB DRAM-resident, 256 KB L2-resident, or 4 KB
    page lists by home side.
- NVML in-process every 10 ms: SM clock, instant power, clock-event reasons.
  The in-kernel SM clock per unit comes from clock64 / globaltimer, and bytes
  per SM clock are reported.
- 5 trials per scenario (3 for sanity), interleaved trial-major. Values are the
  median over trials of the group's per-SM median.
  - Ranges over trials are below 1% everywhere except where shown.
- Checks: TMA first and last word per tile, every 16 B load's word, every
  latency-probe word.
  - 1,178 trials: 0 errors, 0 missing or misplaced units, 0 retries.
- Roles:
  - TMA: 32 KB × 6, one issuer = 192 KB in flight per SM.
  - LD: 32 warps × 8 × 16 B per lane = 128 KB in flight.
  - LAT: one thread, dependent loads.
  - FMA: mma.sync f16.
  - ST/TMAST: writes.
- Aggressor = 16 SMs of GPC 6. Bystanders = first SM of the first two TPCs of
  each other GPC: 6 in GPC 6's half (byA), 8 in the other half (byB).

**Die facts found on the way (FACT)**
- L2 sides differ by die. Here side A = GPCs {0, 2, 6, 7} and side B =
  {1, 3, 4, 5}, by the sidecheck far fraction 0.000 vs 0.499. The L2-structure
  die had A = {0, 1, 5, 7}.
  - The TPC-bit → SM map was identical (66 bits, 0 mismatches).
  - `results_gpu/h200_bys_sides.txt`; plans were made side-aware.
- "GPC 0" (its 4 cluster TPCs plus the 4 lone TPCs) delivers 1.47 TB/s with 16
  SMs. Every other GPC delivers 1.284-1.286. So on this die the lone TPCs are not
  in GPC 0's physical GPC (INFERENCE). It affects only q5_rot0.

**Tool problems found and fixed (FACT)**
1. The first `bys_lab` capped one SM at 61.6 GB/s (TMA) and 103 (16 B loads)
   against 196 / 219 for `l2s_lab tw` on the same node. Causes:
   - per-iteration bookkeeping on the critical path between data return and
     re-issue: a 64-bit modulo per tile, a 64-bit divide in the timeline, an
     extra globaltimer read in the gate, local-memory slot arrays;
   - a static 8 KB histogram in every CTA.
   - The output is kept as `h200_bys_sanity_INVALID_slow_kernel.txt`.
2. The fix: fast paths that copy the E-1 loops for contiguous unpaced data. They
   reach 205 (TMA), 209 (TMA, L2), 222 (LD) and 34.1 (LD, 4 warps; E-1 kernel
   34.6).
3. Limits that remain:
   - The general path (paced, duty, page-list TMA, timeline) caps one SM at
     ≈ 105 GB/s. Paced aggressors therefore demand ≤ 105 GB/s per SM. That
     still over-subscribes a GPC: 16 × 105 = 1.68 TB/s against 1.28.
   - The page-list 16 B-load path reaches 156-162 GB/s per SM. Those scenarios
     are compared with their own list-path baselines.
   - The 8-SM paced runs (q2_n8_p*) are invalid: the general path held them at
     92 GB/s per SM.
   - The q1 duty runs with 50 µs bins are invalid: the timeline put the
     bystanders on the general path, at 94 GB/s. They were re-run without a
     timeline as q1b.

## 2. Q1: clock / power artefact? No (FACT, `h200_bys_q1.txt`, `h200_bys_q1b.txt`)

| Scenario | byA GB/s (×) | byB GB/s (×) | bytes/SM clk byA | SM clock (NVML / in-kernel) | power W | reasons |
|---|---|---|---|---|---|---|
| bystanders alone (TMA) | 195.9 | 195.2 | 99.0 | 1980 / 1980 | 373 | 0 |
| + GPC 6 16 SMs TMA | 88.3 (0.45) | 105.9 (0.54) | 44.6 | 1980 / 1980 | 372 | 0 |
| + HMMA on GPC 6's 16 SMs (no memory) | 196.1 (1.00) | 195.0 | 99.0 | 1980 | 389 | 0 |
| + HMMA on all 118 other SMs | 183.5 (0.94) | 182.9 | **101.9** | 1980 / **1800** | 505 | 0 |
| + aggressor + HMMA on 102 others | 88.3 (0.45) | 104.6 | 49.0 | 1980 / 1800 | 499 | 0 |

- The bystander effect happens at a full 1980 MHz and 372 W (cap 560 W).
- A real clock drop to 1800 MHz changes bystander GB/s by −6% at constant bytes
  per clock.
- With the aggressor, the bystander GB/s is the same at 1980 and at 1800 MHz.
- HMMA SMs show 1800 MHz in-kernel even when NVML reads 1980. INFERENCE: a
  tensor-core clock offset; it is irrelevant here.

**Low duty (q1b; no timeline; TMA aggressor on/off with period P):**

| aggressor on-fraction (P = 1 ms) | 0 | 0.10 | 0.25 | 0.50 | 0.75 | 1 | 0.10 at P = 10 ms | 0.50 at P = 10 ms |
|---|---|---|---|---|---|---|---|---|
| aggressor GB/s per SM (window average) | — | 8.0 | 20.0 | 40.0 | 59.9 | 79.8 | 8.0 | 39.9 |
| TMA byA GB/s | 195.9 | 188.0 | 173.9 | 150.7 | 127.3 | 88.3 | 186.9 | 149.5 |
| linear prediction 195.9 − d·107.6 | — | 185.1 | 169.0 | 142.1 | 115.2 | — | 185.1 | 142.1 |
| LD byA GB/s | 200.3 | 137.2 | 124.1 | 102.3 | 80.5 | 48.4 | 136.2 | 101.3 |

- TMA bystanders lose about in proportion to the aggressor's on-time. The period
  does not matter (0.1 ms vs 1 ms on). So the effect is fast (≪ 100 µs) and
  needs no average power.
- 16 B-load bystanders beside a TMA aggressor lose far more than in proportion at
  low duty (0.69× at 10%), the same at both periods. UNVERIFIED why: no
  fast-path timeline was available to see on- vs off-phase rates.

## 3. Q2: dose-response (FACT, `h200_bys_q2.txt`)

**Aggressor SMs in GPC 6 (TMA 32 KB × 6):**

| n | 1 | 2 | 4 | 6 | 7 | 8 | 10 | 12 | 14 | 16 |
|---|---|---|---|---|---|---|---|---|---|---|
| GPC 6 delivered TB/s | 0.19 | 0.38 | 0.75 | 1.08 | 1.20 | 1.27 | 1.27 | 1.28 | 1.28 | 1.28 |
| byA (same half) × | 0.99 | 0.98 | 0.96 | 0.93 | 0.91 | 0.89 | 0.75 | 0.62 | 0.52 | 0.45 |
| byB (other half) × | 0.99 | 0.98 | 0.96 | 0.94 | 0.93 | 0.92 | 0.80 | 0.67 | 0.59 | 0.54 |

**TMA depth at 16 SMs, and per-SM pacing (100% = 80 GB/s per SM = the GPC
limit / 16):**

| | d1 | d2 | d3 | d4 | d6 | p50 | p75 | p90 | p95 | p100 | p150 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| GPC 6 TB/s | 0.60 | 1.02 | 1.27 | 1.27 | 1.28 | 0.64 | 0.96 | 1.15 | 1.22 | 1.28 | 1.28 |
| byA × | 0.97 | 0.91 | 0.86 | 0.69 | 0.45 | 0.94 | 0.91 | 0.89 | 0.85 | 0.49 | 0.49 |
| byB × | 0.97 | 0.93 | 0.92 | 0.74 | 0.54 | 0.95 | 0.93 | 0.92 | 0.90 | 0.56 | 0.56 |

- Below saturation the loss grows mildly with bytes flowing: ≤ 11% at
  1.15-1.20 TB/s, the same whether set by SM count or by pace.
- At saturation the loss is set by the excess in flight, at constant delivered
  bytes:
  - n 8 → 16: 0.89 → 0.45;
  - depth 3 → 6: 0.86 → 0.45.
- Paced ≥ 100% gives 0.49 (not 0.45), because the paced path caps each SM's
  demand near 105 GB/s and so carries less in flight.

**Several saturated GPCs (16 SMs each; bystanders in the remaining GPCs):**

| saturated GPCs (side) | per-GPC TB/s | byA × | byB × |
|---|---|---|---|
| 6 (A) | 1.28 | 0.45 | 0.54 |
| 6, 2 (A, A) | 1.09, 1.10 | 0.47 | 0.72 |
| 6, 2, 7 (A, A, A) | 0.79 each | 0.47 | 0.71 |
| 6, 1 (A, B) | 1.28, 1.28 | 0.44 | 0.45 |
| 6, 1, 3 (A, B, B) | 1.28, 0.94, 0.94 | 0.44 | 0.41 |
| 6, 2, 1 (A, A, B) | 0.94, 0.94, 1.28 | 0.41 | 0.44 |
| 6, 2, 1, 3 (2 + 2) | 0.97-0.98 | 0.43 | 0.43 |

- One saturated GPC in a half is enough to cut that half's bystanders to
  ≈ 0.44×. More saturated GPCs in the same half barely change it (0.41-0.47).
- The other half's loss shrinks (0.54 → 0.71) when the aggressor half is limited
  by its side cap (2.2-2.4 TB/s for the half) rather than by the GPC ports.
  INFERENCE: the backlog then sits partly at the side or fabric limit, not only
  in the slices.

## 4. Q3: where the shared queue is (FACT, `h200_bys_q3.txt`, `h200_bys_q3b.txt`, `h200_bys_ncu.txt`)

**Data source of aggressor and bystander** (GB/s per SM, × vs the same
bystanders alone):

| Aggressor (GPC 6, side A) | Bystanders | byA | byB |
|---|---|---|---|
| DRAM, TMA | DRAM, TMA | 88.3 (0.45) | 105.9 (0.54) |
| **L2-resident 256 KB/SM, TMA** (agg alone 1.27 TB/s) | DRAM, TMA | **89.1 (0.45)** | **198.6 (1.02)** |
| L2-resident, 16 B loads | DRAM, TMA | 90.0 (0.46) | 204.8 (1.05) |
| DRAM, TMA | **L2-resident**, TMA | **88.5 (0.43)** | 206.6 (1.00) |
| L2-resident, TMA | L2-resident, TMA | 89.2 (0.43) | 206.6 (1.00) |
| DRAM, TMA | L2-resident, 16 B loads | 94.3 (0.39) | 244.2 (1.00) |
| **shared 4 MB, 16 B loads** (GPC delivers 2.30 TB/s) | DRAM, TMA | **194.2 (0.99)** | 193.6 (0.99) |
| shared 256 KB, 16 B loads (GPC delivers 1.84 TB/s) | DRAM, TMA | 177.2 (0.90) | 195.3 (1.00) |
| writes, st.global.cg (0.87 TB/s, below the cap) | DRAM, TMA | 153.8 (0.78) | 152.6 (0.78) |
| writes, TMA store (0.88 TB/s) | DRAM, TMA | 154.9 (0.79) | 153.8 (0.79) |

**Page locality** (16 B loads over 4 KB-block lists by home side; × vs the same
bystander lists alone). Plan names were written for the other die: on this die
`aggloc` = aggressor *remote-only*, `aggrem` = aggressor *local-only*.

| Bystander pages \ aggressor pages | aggressor local-only (homed A) | aggressor remote-only (homed B) | mixed |
|---|---|---|---|
| bystanders local (own-home) | byA 0.57, byB 1.00 | byA 0.96, byB 0.64 | byA 0.45, byB 0.95 |
| bystanders remote-only | byA (reads B) 0.86, byB (reads A) 0.70 | byA (reads B) **0.40** (fabric B→A shared, agg falls to 0.93 TB/s), byB (reads A) 0.99 | byA 0.48, byB 0.86 |
| bystanders mixed | byA 0.60, byB 0.76 | byA 0.46, byB 0.66 (agg 1.01 TB/s) | byA 0.46, byB 0.66 |

- Every cell follows one rule: a bystander is hurt in proportion to how much of
  its data comes from the slices that serve the aggressor's data. Those are its
  home slices for misses and its own half's replicas for hits.
- With an aggressor remote-only, same-half local readers are almost untouched
  (0.96), although the aggressor's responses end in its own half.
- The one fabric-bandwidth case: byA reading B-homed pages beside a remote-only
  aggressor. They share the 1.28 TB/s B→A fabric with it, at equal per-SM rates
  (58.8 vs 58.2 GB/s). That is plain bandwidth sharing; the aggressor itself
  loses there.

**Slice sets in one 2 MB page** (L2-resident; 32 warps × 4 KB in flight; 256 B
granules; aggressor takes granules with fn = 0 and PA9 = 0, bystanders fn = v
and PA9 = 1):

| Split | byA, same set (v = 0) | byA, other set (v = 1) | byB |
|---|---|---|---|
| f0 = PA8⊕12⊕16⊕18⊕20 (GemAtlas FBP bit) | 71.6 (0.45) | 115.8 (0.73) | 158.4 (1.00) |
| s0 = PA8⊕16⊕17 (GemAtlas slice bit) | 91.5 (0.58) | 94.6 (0.60) | 158.4 (1.00) |
| no filter (PA9 split only) | 91.1 (0.58) | — | 158.4 |

- INFERENCE: f0 splits the FBPs (memory partitions) into two sets. Keeping the
  aggressor's lines off half the FBPs spares bystanders on the other half
  partly (0.73 vs 0.45). s0 does not separate anything.
- A set-disjoint bystander still loses 27%. Either part of the shared queue is
  common to all slices (e.g. the crossbar), or f0 is not a pure FBP split on
  H200 (UNVERIFIED: the hash is GemAtlas's for H20).
- The first version with 8 shallow warps (q3, `q3_sl_*`) built no backlog: the
  aggressor delivered only 1.18 TB/s and bystanders lost ≤ 9%.

**ncu (unmasked, %smid placement, 300 ms, `h200_bys_ncu.txt`)**, as a fraction of
LTS cycles:

| | bystanders alone | aggressor alone | both (harmful) | both, L2 aggressor | both, aggressor paced 95% (harmless) |
|---|---|---|---|---|---|
| lts2xbar active avg / max slice | 0.53 / 0.61 | 0.25 / 0.50 | 0.52 / 0.71 | 0.66 / 0.72 | 0.70 / 0.86 |
| ltcfabric2lts stalled avg | 0.17 | 0.04 | 0.17 | 0.11 | 0.32 |
| xbar2lts active avg | 0.13 | 0.06 | 0.12 | 0.16 | 0.17 |
| DRAM read in the window | 821 GB | 386 GB | 797 GB | 639 GB | 1.08 TB |

- No exposed counter separates harm from no harm. Slice-output links are about
  half busy, so the harm is blocking, not bandwidth (INFERENCE).
  `lts2xbar` stall or credit counters were not in the queried list.

## 5. Q4: latency of a one-load-at-a-time probe (FACT, `h200_bys_q4.txt`, `h200_bys_lathist.txt`)

Cycles at 1980 MHz. Probe A = SM 0 (side A, GPC 6's half); probe B = SM 2
(side B). Each cell: p10 / p50 / p90; "rate" = loads completed in the window
relative to no aggressor.

| | none | aggressor DRAM | aggressor L2-resident | aggressor paced 95% | aggressor paced 90% |
|---|---|---|---|---|---|
| near hit, A | 276/292/308 | 740/1444/2188 (rate 0.38) | 276/324/**3300** (0.54) | 508/684/988 (0.64) | 484/564/884 (0.68) |
| near hit, B | 276/292/308 | 516/716/1236 (0.60) | 276/292/308 (1.00) | 292/708/900 (0.68) | 284/700/876 (0.70) |
| far hit (remote L2), A | 444/468/492 | 772/2484/3628 | 460/2500/3932 | 484/748/1068 | 484/732/1004 |
| far hit, B | 444/468/492 | 476/1828/2588 | 444/468/492 | 484/748/1004 | 476/740/988 |
| DRAM miss home near, A | 524/548/796 | 572/1356/2068 | 532/684/1260 | 524/564/900 | 516/548/836 |
| DRAM miss home far, A | 700/732/908 | 780/1564/2356 | 732/860/3788 | 708/772/1116 | 700/748/1028 |
| DRAM miss home near, B | 524/540/756 | 508/540/732 | 524/540/756 | 516/548/804 | 516/548/796 |
| DRAM miss home far, B | 700/732/948 | 716/892/1636 | 700/732/948 | 708/756/1020 | 700/748/996 |

- The queue delays hits as well as misses (FACT).
- With a DRAM aggressor every hit in its half is delayed (p10 740).
- With an L2-resident aggressor most hits pass but ≥ 10% wait > 3000 cycles.
  INFERENCE: only the slices currently blocked delay a request.
- Side-B probes see the delay only on lines served by side A (far-home misses:
  +160 cycles at p50; far hits +1360). Their own-home misses are unchanged
  (540).
- At 90-95% pacing (no backlog) hits still take 2-2.4× and misses +0-40 cycles:
  ordinary loaded-queue delay. A latency-bound stream feels it (rate 0.64-0.70),
  a deep one hardly does (0.85-0.92).
- Unresolved:
  - far hits on side A are 5× slower with an L2-resident aggressor, yet
    bandwidth bystanders reading remote pages beside a local-only aggressor
    lose only 14%;
  - same-half local readers are spared by a remote-only aggressor.
  - The replica path's place in the queue is not settled.

## 6. Q5: who is hurt most; fairness (FACT, `h200_bys_q5.txt`)

| Bystander kind (in flight per SM) | alone GB/s | byA × | byB × |
|---|---|---|---|
| TMA 32 KB × 6 (192 KB) | 196.2 | 0.45 | 0.54 |
| TMA 32 KB × 1 (32 KB) | 44.4 | 0.35 | 0.41 |
| 16 B loads, 32 warps (128 KB) | 200.3 | **0.24** | 0.38 |
| 16 B loads, 4 warps (16 KB) | 33.0 | 0.34 | 0.58 |
| 16 B loads, 1 warp (4 KB) | 8.4 | 0.33 | 0.60 |
| one dependent load (probe, near hit) | — | 0.38 | 0.60 |

- Shallow streams lose ≈ 0.33-0.38× in the aggressor's half, a little more than
  the deepest TMA stream (0.45×).
- 16 B loads with 128 KB in flight beside a TMA aggressor are hurt most (0.24×).
  Beside a 16 B-load aggressor they lose only 0.46× (sanity: 92.4 GB/s).
- The aggressor loses nothing measurable: 1278-1286 GB/s with and without
  bystanders. The exception is fabric sharing (§4).
- Rotating the aggressor over GPCs 1-7, same-half bystanders get 88.2-88.5 and
  other-half 105.8-106.0 in every case. The effect is symmetric and identical for
  every GPC. (GPC 0's 16-SM group is not one physical GPC on this die: 1.50 TB/s;
  bystanders 0.52/0.58.)

## 7. Q6: can software avoid it today? (FACT, `h200_bys_q2.txt`, `h200_bys_q6.txt`, `h200_bys_q3.txt`)

| Aggressor policy | aggressor TB/s (cost) | byA × | byB × |
|---|---|---|---|
| 16 SMs, unpaced, 32 KB × 6 | 1.278 | 0.45 | 0.54 |
| **in-flight cap 32 KB × 3 per SM (1.5 MB per GPC)** | 1.272 (**−0.5%**) | **0.86** | **0.92** |
| in-flight cap 32 KB × 4 | 1.274 (−0.3%) | 0.69 | 0.74 |
| rate pace 95% | 1.216 (−5%) | 0.85 | 0.90 |
| rate pace 90% / 85% | 1.152 / 1.090 (−10 / −15%) | 0.89 / 0.89 | 0.92 / 0.92 |
| rate pace 100% | 1.277 | 0.49 | 0.56 |
| same 1.28 TB/s spread 8 + 8 SMs over 2 GPCs (paced) | 1.282 (0) | 0.89 | 0.91 |
| same, 4 × 4 SMs over 4 GPCs, same half / both halves | 1.28 (0) | 0.89 / 0.90 | 0.91 / 0.90 |
| 8 + 8 SMs unpaced (each GPC at its knee) | 1.86 of 2.52 alone | 0.60 | 0.74 |

- Bystanders on local-only pages beside the normal (mixed) aggressor:
  - in the other half they are protected (0.95);
  - in the aggressor's half they are not (0.45).
- Small back-offs work, and an in-flight budget works best. A budget of
  C × ≈ 1.2 µs per GPC keeps the GPC at its cap and removes most of the harm.
  - The residual 8-14% is the ordinary loaded-latency cost (Q2 below
    saturation).
- INFERENCE for hardware (M8):
  - a per-GPC outstanding-bytes counter with B_cr ≈ 1.5 MB would give the same
    result transparently, at < 1% for the over-subscribed GPC;
  - the notes' earlier estimate B_cr ≈ C × 0.37 µs = 470 KB is too small for
    32 KB TMA tiles: depth 2 = 16 × 64 KB = 1 MB already under-fills the GPC
    (1.17 TB/s alone, 1.02 beside bystanders).

## 8. Q7: practical size (FACT, `h200_bys_q7.txt`)

Lane = 16 B loads (32 warps) on 2 SMs in each of the 8 GPCs (16 SMs; alone 194 GB/s
per SM, 3.11 TB/s). Tenant = TMA on all other SMs of k packed GPCs (as MPS would
pack it), or the same SM count spread over all GPCs.

| Tenant | tenant alone TB/s | lane × | tenant together TB/s | chip TB/s | ideal min(sum, 4.6) | lost |
|---|---|---|---|---|---|---|
| packed in 1 GPC (16 SMs) | 1.29 | **0.31** | 1.20 | 2.15 | 4.39 | **51%** |
| spread, 16 SMs | 3.07 | 0.52 | 2.55 | 4.17 | 4.60 | 9% |
| packed in 2 GPCs (same half, 30 SMs) | 2.46 | 0.33 | 2.17 | 3.18 | 4.60 | 31% |
| spread, 30 SMs | 4.50 | 0.36 | 3.46 | 4.57 | 4.60 | 1% |
| packed in 4 GPCs of one half | 2.85 | 0.30 | 2.63 | 3.44 | 4.60 | 25% |
| packed in 2 + 2 GPCs | 4.60 | 0.22 | 3.91 | 4.57 | 4.60 | 1% (roof-bound) |

- Packed beside a decode-like lane, a single memory-heavy tenant costs the lane
  ~70% of its rate and the chip half its throughput.
- Spread, the lane's loss is ordinary sharing of the 4.6 TB/s roof. Even 1-2
  packed GPCs make the chip run at 47-69% of what it could.

## 9. Implications for a simulator model of the L2 side (INFERENCE)

To reproduce the effect:
- Model the GPC return port as a 1.28 TB/s (≈ 648 B per SM clock at 1.98 GHz)
  serializer **with credits back to the slices**. Do not use an infinite buffer
  at the GPC.
- Give each L2 slice one shared response buffer / output credit pool for all
  destinations, with no per-destination VOQ. Then responses for a blocked GPC
  stall the slice for everyone. Requests admitted to a slice should be served in
  arrival order, so throughput shares are proportional to outstanding requests.
- Let SMs keep their full windows: TMA 192 KB per SM, LD 128 KB, so a GPC can
  have 3 MB outstanding. Otherwise no backlog forms. Same-line merging in the
  GPC (before the port) must stay, or a shared-buffer aggressor will hurt.
- Data for a remote line is sourced by its home slice. Harm follows the sourcing
  slices: model the home slice as the one holding the response. The fabric is a
  separate 1.28 TB/s-per-direction link, shared by bandwidth only.
- Checks a model must pass:
  - same-half bystander = W / (n·W/C − 0.22 µs);
  - other half f ≈ 0.44 (page-pure streams);
  - DRAM-free aggressor gives the same same-half harm and no other-half harm;
  - in-flight cap 3 × 32 KB gives 0.86×;
  - 95% pacing gives 0.85-0.90×.
- In Vortex terms: the existing GPC-switch prototype needs a credit return from
  the group port to the L2 banks' response queues. A shared response queue per
  bank (the VX_CFG_L2_FWD_HOLD behaviour of commit 462002754 is the same family:
  a bank holding a forwarded fill on a full response queue) would produce the
  cross-group coupling. A per-destination queue would not.

## 10. What prior work already reports (only what our notes support)

- **Fan et al., arXiv 2609.24270.**
  - They use same-GPC contention ("SMs sharing a GPC contend on the GPC's L2 Xbar
    bandwidth") only as a topology probe.
  - Their 2.8 TB/s per GPC is a shared-buffer effect (our L2-structure note).
    Here the shared-buffer case is also the one that does *not* harm bystanders.
  - No cross-GPC interference is reported in what our notes record.
- **GPCPlace, arXiv 2611.24458.**
  - Reports interference *within* shared GPCs: a latency-critical job beside a
    background job in the same GPCs slows 2.42×, probe latency 193 → 386 ns, on
    an RTX 4090.
  - Assumes dedicated GPCs isolate tenants. Its port model (one request per cycle
    per GPC port, per-slice queues at the GPC port, equal arbiter) is unsourced.
  - On H20/A800 it finds packing effects small.
  - Our result contradicts the isolation premise on H200: a dedicated,
    over-subscribed GPC slows every other GPC's SMs.
- **DGNA, arXiv 2607.19922.** Latency and NUMA characterisation; no bandwidth
  experiments. Our unloaded latencies match theirs (near hit 292, far hit 468,
  near/far miss 540-548/732).
- **Jin et al., MICRO'24.**
  - GPC speedup ratios.
  - V100 "contiguous SM" degradation of the contiguous job itself (62% at
    28 SMs on one memory partition).
  - "Some of the GPC speedup is provided in space."
  - Nothing on SMs in other GPCs in what our notes record.
- Our own Observation G / M8 (`gpc-limit-design.md`) predicted this effect from a
  makespan run. This study confirms it as a backlog effect, not a bandwidth one.
- Claim supported: as far as these four works are summarised in our notes, none
  reports that an over-subscribed GPC slows SMs in other GPCs, or measures its
  trigger, location or size. No claim is made beyond them.

## 11. Open / unverified

1. Why 16 B-load bystanders lose more than in proportion to a low-duty TMA
   aggressor (q1b), and more than the model beside a TMA aggressor (+21%).
2. The replica / far-hit path: far hits on side A 5× slower with an L2-resident
   aggressor, yet bandwidth readers of remote pages lose only 14%.
3. Which buffer holds the backlog (slice output FIFO, crossbar input, or slice
   input credits). No exposed ncu counter separates them.
4. Whether the H20 hash bit f0 is an FBP split on H200, and why the f0-disjoint
   bystander still loses 27%.
5. The other half's partial recovery when the aggressor half is limited at the
   side cap (two or three saturated GPCs in one half).
6. A hardware credit budget (M8) is measured only in software form (in-flight
   cap and pacing).

## 12. Files

`/storage/project/r-hkim358-0/echung67/vortex-exp/`:
- Sources: `gpu/bys_lab.cu`, `gpu/bys_plan.py`, `gpu/bys_phase.sh`,
  `gpu/bys_srun.sh`, `gpu/bys_build.sh`, `gpu/bys_holder.sbatch`,
  `gpu/bys_report.py`, `gpu/bys_analyze.py`, `gpu/bys_model.py`,
  `gpu/bys_ncu_metrics.env`. Binaries only in `/tmp/$USER/bys` on the node.
- Raw outputs in `results_gpu/`:
  - setup: `h200_bys_holder.out`, `h200_bys_check.txt`, `h200_bys_sides.txt`,
    `h200_bys_plans.txt`, `h200_bys_ncu_metrics.txt`
  - phases: `h200_bys_sanity.txt`, `h200_bys_q1.txt`, `h200_bys_q1b.txt`,
    `h200_bys_q2.txt`, `h200_bys_q3.txt`, `h200_bys_q3b.txt`, `h200_bys_q4.txt`,
    `h200_bys_q5.txt`, `h200_bys_q6.txt`, `h200_bys_q7.txt`
  - latency and counters: `h200_bys_lathist.txt` (8-cycle histograms),
    `h200_bys_ncu.txt`
  - invalid, kept for the record: `h200_bys_sanity_INVALID_slow_kernel.txt`
