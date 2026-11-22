# H200 L2 / memory-side structure, measured

Date 2026-11-21.
- GPU: one H200 on node atl1-1-02-014-16-0 (UUID GPU-7d994d23-...). This
  is a third die: earlier studies used atl1-1-03-019-14-0 and
  atl1-1-03-018-14-0.
- Holder job 15976443 `l2struct_h200`: submitted 15:56:09, started
  15:56:12 (3 s wait), cancelled 16:39:30 (43 min).
- Software: driver 615.71.09, toolkit 12.9.1.
- Tools: TPC masks (`gpu/tpc_smctrl.h`); new code `gpu/l2s_lab.cu` (lat / bw /
  tw / mix), `gpu/l2s_run.py`, `gpu/l2s_e1.py`, `gpu/l2s_analyze.py`.
- Raw outputs: `results_gpu/h200_l2s_*`.

Tags: FACT = measured here or printed by a tool; INFERENCE = my reasoning;
UNVERIFIED = not checked. Latencies are in SM clock cycles; the SM ran at
1980 MHz during every latency scan (in-kernel clock64/globaltimer).

## 0. Answer first

**Structure**

| Item | H200 measured | Confidence | Literature (H100 / H20) |
|---|---|---|---|
| ncu attributes | `l2s_count` 96, `num_l2s_per_fbp` 8, `fbp_count` 12, `l2_cache_size` 62,914,560 B (60 MiB), `fb_bus_width` 6,016, max SM 1,980 MHz, max mem 3,201 MHz; GPC/TPC attributes "n/a" | FACT | H100: 80 slices, 10 FBPs; H20: 96 slices, 12 FBPs |
| L2 partitions | **2**, of 4 GPCs each: side A = GPCs {0, 1, 5, 7}, side B = {2, 3, 4, 6} (GPC numbering of `h200_l2s_gpc.txt`; GPC 0 includes the 4 lone TPCs, SMs 124-131) | FACT, high: 4 SMs of side B classify all 65,536 lines identically | Two halves of 4 GPCs (Jin, GemAtlas, Fan) |
| Partition of an address | **p = PA12 ⊕ PA13 ⊕ PA15 ⊕ PA17 ⊕ PA19 ⊕ c(2 MB page)**; zero error on 65,536 lines over 4 pages; constant over every 4 KB block; 4 KB pages 2j and 2j+1 always in opposite halves (1,835,008 blocks) | FACT within 2 MB pages (bits 0-20 are physical in 2 MB pages; the page constant comes from physical bits above 20) | **identical** to GemAtlas Eq. (1) for H20 |
| Partition interleave | **4 KB** (runs of the class are 4 KB or 8 KB along addresses) | FACT | 4 KB (GemAtlas, Fan), "8 KB alternation" (DGNA A100, Accel-Sim 2.0) = runs of two |
| Remote-read replication | A line read from the far half is then a near hit for the reader's whole half; warming from either side-A SM gave the same classes | FACT | GemAtlas, DGNA, Accel-Sim 2.0 |
| L2 hit, near | **≈ 290** (p50 293-297; two sub-classes **≈ 280 and ≈ 305**, weights 0.38-0.45 / 0.55-0.62) | FACT | DGNA H100 295 (sub-NUMA 280/312), Fan H200 309 |
| L2 hit, far | **≈ 466-468** (GMM mean; p10 445, p90 490-493) | FACT | DGNA 470, Fan 466 |
| DRAM miss, home near | **≈ 546** (median; GMM 540-543) | FACT | DGNA 554, Fan ~490 |
| DRAM miss, home far | **≈ 730-736** (median; GMM 728-733) | FACT | DGNA 728, Fan ~655 |
| Extra cold class | 7-10% of cold lines at ≈ 960-975 (sd ≈ 180), on every SM; INFERENCE: DRAM row conflicts / refresh | FACT (class), INFERENCE (cause) | — |
| Near-hit sub-classes | The 280/305 split is shared by GPCs {0,1,2,3} and anti-correlated with GPCs {4,5,6,7} (agreement 0.84-0.95 within, 0.15-0.17 across), independent of the L2 side; not a linear GF(2) function of bits 7-20 | FACT (pattern), INFERENCE: a second, sub-partition level as DGNA's "sub-NUMA" (2 groups of 4 GPCs cutting across the halves) | DGNA ~32-cycle sub-NUMA |
| DRAM fetch granule | Cold read at 32 B steps: positions 0/1/2/3 of a 128 B line take 663/296/515/296 cycles: the second 32 B sector arrives with the first | FACT (64 B per miss) | — |
| Slice/channel routing granule | Bits 7 and 9 do not change the slice-contention result; bits 8, 10, 11 do. This fits a 256 B granule with a hash in which bit 9 is absent (GemAtlas). **It argues against a 1 KB interleave**: a 1 KB unit would make bit 8 irrelevant | FACT (bit effects), INFERENCE (granule), medium: the probe is confounded (§1d) | GemAtlas: 256 B, bit 9 absent |
| Slices / channels | 96 slices, 8 per FBP, 12 FBPs (ncu). Not determinable from bandwidth: fixing bits 8-11 (1/16 of the data) still gives 4.24 of 4.60 TB/s, and bits 8-15 (1/256) 3.51 | FACT (ncu), FACT (no confinement) | — |

**Bandwidth (§1e, §E)**
- **Cross-partition fabric: 1.28 TB/s per direction.** The whole chip
  reading only remote pages gets 2.56 TB/s (1.28 per side). Local pages only
  give 4.63 TB/s (2.31 per side). Mixed (the normal 4 KB interleave) gives
  4.58. One side's 66 SMs: local 2.31, remote 1.28, mixed 2.57.
- **Per-GPC limit 1.28 TB/s** (1277-1285 GB/s, ≈ 648 B per SM clock), reached
  at 8 SMs.
  - The same for DRAM and L2-resident data with disjoint buffers, and for
    local, remote or mixed pages.
  - Two GPCs give 2.54-2.56 TB/s in every one of the 28 pairs.
- **Fan's 2.8 TB/s is a shared-buffer effect.** With every SM reading the
  same 256 KB, one GPC reaches 2.75 TB/s at 18 SMs (4 MB shared: 2.28). With
  disjoint 256 KB per SM it stays at 1.27-1.29.
  - At chip level, L2-resident: disjoint 9.35-9.63 TB/s (1.16-1.24 per
    GPC), shared 23.1-25.0.
  - INFERENCE: same-line requests merge before the per-GPC limit applies, so
    the limit is on unique-line requests, not on return wires.
- **Saturation sweep.** Total = min(Σ GPC 1.28, Σ sides 2 × fabric = 2.56
  each, 4.6). One half alone never exceeds 2.56 TB/s with 2, 3 or 4
  saturated GPCs.
- **Bystanders lose.** One saturated GPC (16 SMs) cuts unsaturated SMs in
  every other GPC from 192 to 88 (same half) or 106 (other half) GB/s per SM
  (TMA). The total falls below the sum of the parts.
- **Writes** (16 B st.global.cg): 63 GB/s per SM, linear to 935 GB/s at 18
  SMs. No GPC limit reached; the per-SM write ceiling was not explored
  further.

**Part 2: the per-SM limit is one shared ingress, not shared memory.**
- Alone: TMA (32 KB × 6) 196-204 GB/s; 16 B loads 218 GB/s.
- Together in one CTA, TMA + loads total **215-230 GB/s** (78 + 137;
  124 + 105). That is one ceiling, not 196 + 218.
- TMA with independent shared-memory readers keeps 174-180 GB/s while the
  readers get 54-99. Shared-memory bandwidth is not what limits TMA.
- The ceiling moves a little with distance: 223 (near pages) / 219 (mixed) /
  206 (far pages) for 16 B loads (INFERENCE: in-flight-bound).

**Simulator parameters (H200)**

| Parameter | Value |
|---|---|
| L2 partitions | 2 |
| GPCs per partition | 4 ({0,1,5,7} / {2,3,4,6}) |
| Slices | 96 = 2 × 6 FBPs × 8 |
| Partition hash | PA12⊕PA13⊕PA15⊕PA17⊕PA19 ⊕ (physical bits ≥ 21); granule 4 KB |
| Slice/channel interleave unit | 256 B (not 1 KB) |
| L2 hit, near | 290 cycles (±12; sub-levels 280/305) |
| L2 hit, far | 467 cycles |
| DRAM miss, near home | 546 cycles |
| DRAM miss, far home | 733 cycles |
| DRAM fetch | 64 B per miss |
| Remote read | replicates the line into the reader's partition |
| Per-GPC request limit (unique lines) | 1.28 TB/s ≈ 648 B/clk at 1.98 GHz |
| Cross-partition fabric | 1.28 TB/s per direction |
| Per-side local memory | 2.31 TB/s; chip 4.63 TB/s (TMA, local pages) |
| Per-SM read ingress | ≈ 215-230 GB/s ≈ 110-116 B/clk, shared by TMA and LSU |
| Per-slice bandwidth | not determined (§1d) |

## 1. Structure

### 1a. Device attributes (FACT, `h200_l2s_ncu.txt`, `h200_l2s_env.txt`)

- ncu 2025.2.1 profiles on this node: no counter-permission error. It was
  queried on the unmasked `tpc_bwsweep`, because my mask callback cannot
  subscribe while the profiler holds the callback table ("tpc_smctrl:
  subscribe failed (210)").
- Attribute values printed:
  - `device__attribute_l2s_count 96`, `device__attribute_num_l2s_per_fbp 8`,
    `device__attribute_fbp_count 12`
  - `device__attribute_l2_cache_size 62,914,560`
  - `device__attribute_fb_bus_width 6,016`
  - `device__attribute_multiprocessor_count 132`
  - `device__attribute_max_gpu_frequency_khz 1,980,000`,
    `device__attribute_max_mem_frequency_khz 3,201,000`
  - `device__attribute_dram_bandwidth`, `device__attribute_num_gpcs`,
    `device__attribute_num_tpc_per_gpc`, `device__attribute_num_tpcs`:
    "(!) n/a"
- `lts__t_sectors.sum` and `dram__sectors_read.sum` worked;
  `ltc__t_sectors.sum` "n/a". `--print-metric-instances values` did not list
  instances in the output I captured.
- 96 × 640 KiB = 60 MiB matches the cache size. INFERENCE: 6,016 / 6,144 bits
  = 141/144 GB, i.e. one 128-bit slice of a stack is disabled.
- Clocks cannot be locked: `nvidia-smi -lgc` gives "The current user does not
  have permission to change clocks"; `-ac` is "deprecated".

### 1b. Latency map (FACT, `h200_l2s_lat.txt`, `h200_l2s_lat_analysis*.txt`)

Method:
- One thread on one SM (TPC mask + SM filter) times `ld.global.cg.u32`
  between two `clock64` reads.
- The next address is computed from the loaded value: a[w] = w, next =
  4·v + stride. So the second clock read waits for the data; this was
  checked in SASS (LDG, IMAD on the loaded register, CS2R).
- Probes: an 8 MB window at 128 B steps (65,536 lines), from the first SM of
  every GPC (SMs 0, 2, 4, 6, 8, 10, 12, 14) and a lone-TPC SM (130).
- Conditions: cold (1 GiB flush first), warm by itself, warm by SM 0
  (GPC 0, side A), warm by SM 14 (GPC 7, side A). Warming = one 1024-thread
  CTA on the warm SM reads every 32 B sector with ld.global.cg.
- Every probe checked the loaded word: errs = 0.
- A first version without the data dependency gave half the samples at 13
  cycles (ptxas overlapped the loads). It is kept as
  `h200_l2s_lat_INVALID_overlapped_loads.txt` and not used.

| SM (GPC, side) | cold (DRAM) GMM means / weights | warm self | warm by SM 0 (side A) |
|---|---|---|---|
| 0 (0, A) | 543 / 733 / 975; 0.43 / 0.47 / 0.10 | 282 / 307 | (self) |
| 2 (1, A) | 540 / 730 / 960 | 279 / 302 | 279 / 302 (all near) |
| 4 (2, B) | 541 / 728 / 961 | 279 / 301 | **289 / 467, 0.50 / 0.50** |
| 6 (3, B) | 542 / 729 / 964 | 279 / 304 | 290 / 468, 0.50 / 0.50 |
| 8 (4, B) | 542 / 729 / 955 | 279 / 304 | 290 / 469 |
| 10 (5, A) | 540 / 731 / 968 | 279 / 302 | 279 / 302 |
| 12 (6, B) | 542 / 729 / 954 | 279 / 301 | 290 / 468 |
| 14 (7, A) | 541 / 731 / 972 | 280 / 304 | 280 / 304 |
| 130 (0 lone TPC, A) | 522 / 560 / 734 / 987 | 276 / 315 | 276 / 315 |

- The home label used below is "far for SM 4 after a warm by SM 0". It
  agrees 100.000% with SMs 6, 8 and 12 under both warmers.
- Split by that label, cold medians are 545-553 for lines homed on the
  reader's side and 730-739 for the other side. That holds on all 9 SMs.
- Hits after a self-warm differ by only 6-7 cycles between local-home and
  remote-home lines (290 vs 297). That is what replication predicts: the
  self-warm put a copy on the reader's side.

### 1c. Sides (FACT)

- GPCs {0, 1, 5, 7} and {2, 3, 4, 6} split 4 + 4.
- The near sets are complementary: for side B the lines homed on side A are
  far, and vice versa. The cold medians mirror exactly on both sides.
- The lone-TPC SM 130 behaves as side A (GPC 0).
- Jin et al. report GPCs 0-3 left / 4-7 right on H100. The NVIDIA GPC
  numbering is not exposed, so "which four" cannot be compared; it is 4 + 4.

### 1d. Granularity

- **Partition (FACT).** The GF(2) fit of the home label over bits 7-20 with
  a per-2 MB-page constant gives exactly {12, 13, 15, 17, 19}, error 0.0
  (each of the 4 pages separately too).
  - Over 7 GiB at 4 KB resolution, one probe per block from SM 0 and SM 14,
    the hash plus a page constant voted per page (vote confidence ≥ 0.87)
    agrees 96.6% with the cold class. The disagreements are the 9.5%
    ≥ 850-cycle tail.
  - Pages 2j/2j+1 are opposite in 100% of 917,504 pairs. Contiguity is only
    known within 2 MB pages (cudaMalloc alignment printed "align_2MB=yes");
    bits above 20 are virtual.
- **Slice level (FACT for the numbers, INFERENCE for the reading;
  `h200_l2s_bw_slice.txt`).**
  - Method: SMs of side A (1 per GPC = 4 SMs, or 4 per GPC = 16 SMs) re-read
    a set of lines inside one near 4 KB block, `ld.global.cg` 16 B, shared
    list.
  - GB/s with 4 / 16 SMs:

| Set | 4 SMs | 16 SMs |
|---|---|---|
| 1 line | 435 | 826 |
| 2 lines differing in bit 7 | 218 | 510 |
| bit 8 | 434 | 809 |
| bit 9 | 218 | 508 |
| bit 10 | 434 | 812 |
| bit 11 | 434 | 810 |
| bits 8-9 (4 lines) | 218 | 502 |
| bits 8-11 (16 lines) | 522 | 1250 |
| whole 4 KB block (32 lines) | 522 | 1121 |
| 64 / 512 near blocks | 505-525 | 2018-2100 |

  - So bits 7 and 9 keep the "one slice" behaviour, and bits 8, 10, 11 break
    it. That is GemAtlas's routing (bit 8 in f0/s0, 10 in f1/s1, 11 in
    f2/s2, bit 9 in none) on a 256 B granule.
  - Confounds: a single hot line is faster than two lines in one slice
    (same-line merging), and the bits 8-9 set (two slices by this reading)
    is not twice the one-slice value. The per-slice bandwidth and the slice
    count therefore cannot be read off these numbers.
  - The block-list kernel used here reaches only about 110-130 GB/s per SM,
    so the 4-SM rows above 435 are demand-limited.
- **DRAM channel (f) (FACT).** All 132 SMs stream 256 B granules with fixed
  address bits, disjoint:

| Filter | Fraction of data | GB/s |
|---|---|---|
| all | 1 | 4598 |
| bit8 = 0 | 1/2 | 4514 |
| bits 8-9 = 0 | 1/4 | 4412 |
| bits 9-11 = 0 | 1/8 | 4370 |
| bits 10-11 = 0 | 1/4 | 4480 |
| bits 8-11 = 0 | 1/16 | 4240 |
| bits 12-15 = 0 | 1/16 | 4267 |
| bits 16-19 = 0 | 1/16 | 4322 |
| bits 8-15 = 0 | 1/256 | 3512 |

  - Fixing low bits never confines traffic to one channel; the hash spreads
    it. One channel's ~50 GB/s could not be isolated this way. Channel count
    stays inferred (6,016-bit bus; 12 FBPs).

### 1e. Bandwidth by distance (FACT, `h200_l2s_local.txt`; fixed 0.5 s window; 16 B loads, 32 warps per SM, 4 KB blocks from local / remote / mixed page lists)

| SMs | local pages | remote pages | mixed |
|---|---|---|---|
| 1 SM (GPC 0, 1, 2, 6) | 223 | 206 | 219 |
| 6 SMs of GPC 1 / GPC 6 | 1230 / 1229 | 1116 / 1115 | 1221 |
| all SMs of GPC 1 / GPC 6 | 1283 / 1285 | 1283 / 1285 | 1284 |
| side A, 66 SMs (side B the same) | 2314 | 1284 | 2565 |
| 2 per GPC, 16 SMs | 3213 | 2455 | 3051 |
| 3 per GPC, 24 SMs | 4140 | 2554 | 3953 |
| 4 per GPC, 32 SMs | 4449 | 2564 | 4374 |
| 6 per GPC, 48 SMs | 4607 | 2562 | 4575 |
| whole chip, 132 SMs | 4629 | 2562 | 4580 |
| whole chip TMA 4 KB tiles | 4634 | 2561 | 4613 |

Reading (INFERENCE):
- Remote traffic is capped at 1.28 TB/s per direction. With the normal mix,
  each side can take at most 2 × 1.28 = 2.56 TB/s.
- The spread 16-SM job (3.05 TB/s mixed) is *not* fabric-bound: local-only
  gives 3.21, i.e. only 5% more. At 24-32 SMs, local-only is 5% above mixed.
- A GPC's 1.28 does not depend on how many partitions it targets: local,
  remote and mixed are equal once saturated. Jin's "more destinations give
  more GPC bandwidth" does not show here for DRAM.

## E. Saturation, bystanders and L2 aggregate (coordinator E-1/E-2) (FACT, `h200_l2s_e1.txt`, `h200_l2s_e2.txt`)

Protocol:
- Fixed window of 1.0 s; every SM starts at the same globaltimer instant.
  Bytes completed inside the window count; finish time is not used.
- Disjoint buffers per SM: 64 MB, or 32 MB in the sweeps, of contiguous
  memory per SM.
- TMA = 1 issuing thread, 32 KB × 6; LD16B = 32 warps, 8 × 16 B in flight
  per lane. Every run passed the check (TMA first/last word, load word per
  512 B).
- NVML was sampled every 200 ms (`h200_l2s_nvml_clocks.csv`; 12,389
  samples). Power reached 583 W against a 560 W limit, with clock-event
  reason 0x4 (SW power cap). The in-kernel SM clock fell to 1570-1850 MHz in
  DRAM-saturated runs; it is printed per run (`clock_mhz_avg`).
- GB/s are memory-bound and are reported as measured; bytes per SM clock
  differ for those runs.

**Bystanders** (2 SMs = first SM of the first two TPCs, in each of GPCs 0-5
and 7; GPC 6 is side B). Per-SM GB/s:

| Config | side-A bystanders | side-B bystanders | GPC 6 SMs | total |
|---|---|---|---|---|
| bystanders alone, TMA | 192 | 192 | — | 2687 |
| + GPC 6 with 16 SMs, TMA | **106** | **88** | 80 each (1278) | 2655 |
| + GPC 6 with 6 SMs, TMA | 182 | 180-181 | 178-179 | 3609 |
| GPC 6 alone: 16 / 6 SMs, TMA | — | — | 80 / 195 | 1284 / 1170 |
| bystanders alone, LD16B | 196 | 196 | — | 2742 |
| + GPC 6 with 16 SMs, LD16B | **132** | **93** | 80 each | 2891 |
| + GPC 6 with 6 SMs, LD16B | 179 | 177 | 174-176 | 3546 |

- A saturated GPC slows unsaturated SMs everywhere. The same half is hit
  more (88-93 vs 106-132), and the chip total falls below the sum of the
  parts (2.66 vs 2.69 + 1.28).
- INFERENCE: the bystanders are in-flight-bound (192 KB or 128 KB in flight
  each), so their rate halving means their latency doubled. The saturated
  GPC's backlog queues at shared memory-side resources (L2 or fabric
  queues).

**Saturation sweep**: 12 SMs per GPC, total GB/s TMA / LD16B (per-GPC in
brackets).

| GPCs | balanced across halves | all on one half |
|---|---|---|
| 1 | 1284 / 1284 | — |
| 2 | 2565 / 2557 [1.28] | 2512 / 2539 [1.26-1.27] |
| 3 | 3762 / 3783 [1.24-1.28] | **2565 / 2563 [0.855]** |
| 4 | 4465 / 4332 [1.08-1.12] | **2565 / 2564 [0.641]** |
| 6 | 4566 / 4391 [0.73-0.76] | — |
| 8 | 4564 / 4464 [0.56-0.57] | — |

- Total = min(g × 1.28, Σ over halves of 2.56, ≈ 4.6). One half's GPCs
  together never exceed 2.56 TB/s; §1e shows that is the fabric carrying
  the remote half.
- The LD16B totals at 4-8 GPCs (4.33-4.46) are 0.1-0.2 below TMA, at a
  lower SM clock (1570-1660 vs 1800-1850 MHz).
- All 28 GPC pairs (12 + 12 SMs, LD16B, 0.5 s): same half 2538-2542, opposite
  halves 2555-2558. So there is no pair-specific limit.
  - The earlier "two packed GPCs: 2.27-2.28 TB/s"
    (`h200_tpc_network.txt`, GPC6 18 SMs + GPC7 14 SMs) was a makespan
    artefact of unequal SM counts.
- Single GPCs: 1283.5-1285.0 for all 8.

**L2 aggregate (E-2)**, all 132 SMs:

| Run | TMA | LD16B |
|---|---|---|
| L2-resident disjoint, 128 KB per SM (16.5 MB) | 9351 | 9556 |
| L2-resident disjoint, 256 KB per SM (33 MB) | 9595 | 9627 |
| L2-resident shared 128 KB / 256 KB (one buffer for all) | — | 23,117 / 25,005 |
| DRAM, 32 MB per SM | 4604 | 4542 |

- Per GPC, L2-resident disjoint: 1.16-1.24 TB/s. **The per-GPC limit applies
  to L2 hits as well**; the L2 itself can deliver ≥ 25 TB/s when lines are
  shared.

**Same-GPC sweep** (Fan's Fig. 2 conflict; `h200_l2s_gpcsweep.txt`; 0.3 s
windows; GPC 6, GPC 1 identical within 1%). Total GB/s:

| n SMs | 1 | 2 | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|---|---|
| DRAM read disjoint, LD16B | 219 | 429 | 847 | 1221 | 1279 | 1283 | 1285 | 1285 | 1284 | 1285 |
| DRAM read disjoint, TMA | 196 | 391 | 782 | 1170 | 1281 | 1284 | 1284 | 1285 | 1285 | 1284 |
| L2 read disjoint 256 KB/SM | 245 | 483 | 976 | 1245 | 1273 | 1289 | 1273 | 1289 | 1273 | 1286 |
| **L2 read shared 256 KB** | 244 | 488 | 980 | 1302 | 1355 | 1585 | 1947 | 2247 | 2514 | **2753** |
| L2 read shared 4 MB | 245 | 487 | 973 | 1278 | 1297 | 1417 | 1610 | 1831 | 2055 | 2276 |
| DRAM write disjoint (16 B st.cg) | 63 | 127 | 253 | 380 | 505 | 630 | 756 | 815 | 876 | 935 |
| L2 write disjoint | 63 | 127 | 253 | 380 | 505 | 629 | 755 | 812 | 860 | 927 |

- Fan's ~2.8 TB/s at 18 SMs is reproduced only with a shared buffer. With
  disjoint buffers (DRAM or L2) the GPC is flat at 1.27-1.29 TB/s from 8
  SMs.
- INFERENCE: a per-GPC merge of same-line reads (Accel-Sim 2.0's LRC). The
  merge is stronger for a small shared set (256 KB) than for 4 MB.
- Writes: 63 GB/s per SM with this kernel, linear, far from any GPC limit
  (cause of the per-SM write rate not studied).

## 2. Part 2: shared memory or the SM's ingress? (FACT, `h200_l2s_part2.txt`; 0.5 s windows; SM 0, and SMs 0+1 = one TPC)

| Configuration | 1 SM | 2 SMs of a TPC |
|---|---|---|
| (i) TMA alone, 1 issuer 32 KB × 6 | 195.9 | 391.4 |
| (i) TMA alone, 2 issuers 32 KB × 3 | 204.3 | 408.0 |
| (ii) 16 B loads alone, 32 warps (default carveout) | 218.2 | 426.3 |
| (ii) loads alone, 24 warps | 161.4 | 333.3 |
| (ii) loads alone, 32 warps, carveout 100 (small L1) | 80.8 | 145.0 |
| **(iii) same CTA: TMA 32 KB × 6 + 24 load warps** | **215.1** (78.1 + 137.0) | 427.4 |
| **(iii) same CTA: TMA + 16 load warps** | **229.6** (124.5 + 105.1) | 452.8 |
| (iii) two CTAs (TMA kernel + 24 / 30-warp load kernel), both carveout 100 | 157.7 / 161.8 (43 + 114-119) | 254.6 / 276.9 |
| (iv) shared-memory readers alone, 2 / 4 / 8 warps | 108 / 127 / 127 | 217 / 253 / 253 |
| (iv) TMA 32 KB × 5 alone | 183.3 | 363.0 |
| (iv) TMA × 5 + 1 / 2 reader warps (different slots) | 179.6 + 54 / 174.0 + 99 | 357 + 108 / 348 + 199 |
| near pages: loads / TMA 4 KB × 12 × 4 issuers / both | 223.1 / 94.8 / 196.9 (47 + 150) | — |
| far pages: loads / TMA 4 KB / both | 205.8 / 93.9 / 174.1 (42 + 132) | — |

Plain statement:
- **The per-SM limit is one ingress shared by TMA and loads**, about 215-230
  GB/s, or 110-116 B per SM clock. Run together, the two paths add up to one
  ceiling, not to 196 + 218.
- **Shared-memory bandwidth is not the limit.** TMA keeps 174-180 GB/s while
  independent readers take 54-99 GB/s of shared-memory reads; the sum
  exceeds the TMA rate alone.
- The ceiling moves with distance by about 8%: near 223, far 206 GB/s for
  loads. INFERENCE: in-flight-bound, so far latency costs a little.
- Two SMs of a TPC give exactly 2×. There is no TPC-level ingress share.
- The two-CTA variant needs both kernels at the maximum shared carveout to
  co-reside, which also shrinks L1 (loads alone then 81). It does not
  separate the paths cleanly, so the same-CTA numbers are the answer.
- My shared-memory reader reaches 127 GB/s (64 B/clk) per SM. The borrow
  agent measured 251-253 GB/s with a different reader, so treat my reader
  numbers as a lower bound for shared-memory read capacity (UNVERIFIED why).

## 3. Method problems found and fixed (FACT)

1. **Latency probe without a data dependency.** ptxas overlapped successive
   loads: half the samples were 13 cycles. Fixed with a dependent address;
   the old file is marked INVALID.
2. **List-indirect streaming kernels.** Per-transfer dependent list lookups
   capped TMA at 108 GB/s and loads at 91-128 GB/s per SM (vs 196-239).
   - Fixed in `k_tw`: the next tile's source is prefetched, and each load
     warp streams whole 4 KB blocks with the next block index prefetched.
   - `k_bw` (used only for §1d) still has the lookup cost: 105-128 GB/s per
     SM, cause not fully found. A version without lookups was not faster
     either.
3. **Maximum shared-memory carveout** shrinks L1 and cuts 16 B loads to 81
   GB/s per SM (`tpc_lab` found 62-67 earlier).
4. **Static + dynamic shared memory > 227 KB** fails to launch ("invalid
   argument"); the limit used is 224 KB.

## 4. Unverified / open

1. The slice count and per-slice bandwidth (the §1d probe is confounded by
   same-line merging and by the block-list kernel's own per-SM limit).
2. What the 280/305 near-hit sub-classes are. They are not a linear hash of
   bits 7-20 and split GPCs {0-3} from {4-7}.
3. The cause of the 7-10% ≈ 970-cycle cold class.
4. Why writes run at 63 GB/s per SM.
5. Whether the per-GPC limit is the request port (unique lines) or something
   else. Shared reads exceed it (2.75 TB/s per GPC), so it is not the return
   wires.
6. The bystander slowdown mechanism (queueing where?). A run with the
   saturated GPC reading local-only pages would separate fabric from L2/DRAM
   queues; not done.
7. MPS clients and other dies: the map was identical on all three nodes
   measured; die variation (Fan: 5/28) not encountered.

## 5. Files

`/storage/project/r-hkim358-0/echung67/vortex-exp/`:
- Sources: `gpu/l2s_lab.cu` (+ binary `gpu/l2s_lab`), `gpu/l2s_run.py`,
  `gpu/l2s_e1.py`, `gpu/l2s_analyze.py`, `gpu/l2s_holder.sbatch`.
- Lists: `gpu/l2s_lists/` (near/far 4 KB block lists per side, line sets,
  granule filters).
- Latency samples: `gpu/l2s_lat/*.u16`, `home_h.npy`, `cold_classes.npy`.
- Raw outputs in `results_gpu/`:
  - setup: `h200_l2s_holder.out`, `h200_l2s_env.txt`, `h200_l2s_ncu.txt`,
    `h200_l2s_probe.txt`, `h200_l2s_map.txt`, `h200_l2s_gpcmap.txt`,
    `h200_l2s_gpc.txt`, `h200_l2s_sides.txt`, `h200_l2s_nvml_clocks.csv`
  - latency: `h200_l2s_lat.txt`, `h200_l2s_lat_analysis.txt`,
    `h200_l2s_lat_analysis2.txt`, `h200_l2s_lat_INVALID_overlapped_loads.txt`
  - bandwidth: `h200_l2s_bw_gpc_INVALID_kernel_limited.txt` (first,
    kernel-limited version, not used), `h200_l2s_bw_slice.txt`, `h200_l2s_e1.txt`, `h200_l2s_e2.txt`,
    `h200_l2s_gpcsweep.txt`, `h200_l2s_local.txt`, `h200_l2s_part2.txt`
