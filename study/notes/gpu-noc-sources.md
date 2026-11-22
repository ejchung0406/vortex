# GPU on-chip network and L2/memory organisation: what the primary sources say

Compiled 2026-11-21. Read-only survey. Every structural claim below carries its
source, location and a short verbatim quote. Tags used in the tables:

- **documented**: NVIDIA states it (whitepaper, tuning guide, Nsight guide, driver source, patent text).
  A patent documents an *embodiment*, not a product; this is noted each time.
- **measured**: a paper measured it on hardware.
- **inferred**: a paper (or this note) concludes it from measurements or arithmetic; the
  source itself uses words like "suggests", "likely", "we infer", "not confirmed".
- **figure-read**: a value read off a plotted bar/point by eye; approximate (±10%).

Abbreviations: WP = whitepaper; MP = memory partition (Jin et al.'s term); FBP = frame
buffer partition; FBPA = FB partition "A" unit (DRAM controller); LTC = L2 cache unit;
LTS = L2 slice.

---

## 0. Sources read and their identity

| # | Source | What it is | Version / date | GPUs actually tested |
|---|---|---|---|---|
| 1 | Jin, Rocca, Kim, Kasan, Rhu, Bakhoda, Aamodt, Kim, "Uncovering Real GPU NoC Characteristics: Implications on Interconnect Architecture", MICRO 2024 (pp. 885–898, doi 10.1109/MICRO61859.2024.00070) | measurement paper, 14 pp. | MICRO'24 | V100, A100, H100 (Table I lists 132 SMs, 3.35 TB/s, i.e. SXM numbers, but a 1.755 GHz max clock, which is the PCIe boost; ambiguous) |
| 2 | **GemAtlas** = arXiv:2611.13860v1 [cs.AR], submitted 10 Nov 2026. Title: "Uncovering the Memory Hierarchy of Modern NVIDIA GPUs for Accurate Simulation". Authors: Ziwen Wang, Jiapeng Zhou, Changxi Liu*, Dongsheng Wang, Yun Chen* (HKUST-Guangzhou; Tsinghua). **Identifier confirmed real and on-topic** (L1/L2/constant cache reverse engineering + Accel-Sim integration). | SASS-microbenchmark paper | v1 | **H20 (primary)**, H100, A800, RTX 4090. **Not H200.** |
| 2b | Liu, Chen, Carlson, "DGNA: Dissecting GPU NUMA Architecture through Microbenchmarking and Data Analysis", arXiv:2607.19922v1 (22 Jul 2026) | measurement paper (GemAtlas ref [33]); source of the 161/174/175/32-cycle numbers | v1 | A100, H100 SXM5 (and RTX 5090 briefly) |
| 3 | NVIDIA H100 Tensor Core GPU Architecture whitepaper **V1.02** (mirror) | documented | 2022 | — |
| 3b | NVIDIA A100 Tensor Core GPU Architecture whitepaper | documented | 2020 | — |
| 3c | NVIDIA Hopper Tuning Guide (docs.nvidia.com) | documented | current | — |
| 3d | Nsight Compute Profiling Guide (docs.nvidia.com), sections 2.3.x and 2.4.6 | documented | current (chars 0–200,000 of 237,141 read) | — |
| 4a | US12020035B2 "Programmatically controlled data multicasting across multiple compute engines", Nvidia; filed 2022-03-10, granted 2024-06-25 | patent (embodiment) | — | — |
| 4b | US11429534B2 "Addressing cache slices in a last level cache", Nvidia; filed 2021-04-13 (priority 2019-09-25), granted 2022-08-30 | patent (embodiment) | — | — |
| 5 | Luo, Fan, Li, Du, Wang, Chu, "Benchmarking and Dissecting the Nvidia Hopper GPU Architecture", arXiv:2402.13499v1 (21 Feb 2024; IPDPS 2024) | measurement paper | v1 | A100 PCIe, RTX 4090, **H800 PCIe (114 SMs, HBM2e, 1755 MHz)** |
| 6a | NVIDIA open-gpu-kernel-modules, commit a4b95f6a2d (2026-11-18), headers + mem_sys sources | documented (driver identifiers/comments) | — | — |
| 6b | Accel-Sim gpgpu-sim_distribution `dev` branch, configs `SM90_H100`, `SM90_H200`, `src/gpgpu-sim/{addrdec.cc,l2cache.cc,gpu-sim.cc}`; PR #143 "Accel-Sim 2.0: Hopper (H100/H200) performance model" (merged 2026-08-25) | simulator source | dev @ 2026-10-20 | — |
| 6c | Pan, ..., Rogers, "Architecting the Next Generation of Asynchronous, Distributed GPUs for the AI Era" (Accel-Sim 2.0 paper), arXiv:2608.22602 (23 Aug 2026), Sec. II-C | measurement + simulator | v1 | H100, B200 |
| 7 | Chips and Cheese, "Nvidia's H100: Funny L2, and Tons of Bandwidth", Chester Lam, 3 Jul 2023 | third-party measurement (OpenCL) | — | H100 PCIe (114 SMs) |
| 8 | NVIDIA developer forum thread 300533 "What is the inter-SM linkage of DSM cluster" | forum (rumour-grade) | 2024–2026 | — |

Freshness flags: the H100 WP v1.02 is from 2022 and says "Memory data rates not finalized"
(p37); Chips and Cheese is from 2023 and tests H100 PCIe only; Luo et al. test H800 PCIe,
not H100 SXM; GemAtlas's primary GPU is H20 (a GH100 part with 78 SMs, 96 GB, 60 MiB L2).
**No source in this list measured an H200.**

---

## A. The hierarchy between an SM and DRAM

### Answer

The request path that the sources support is: SM → TPC (2 SMs) → [CPC] → GPC → crossbar
→ L2 partition (one of two, each serving the GPCs "directly connected" to it) → FBP
(containing L2 slices and DRAM controller logic) → FBPA/memory controller → HBM channel.

- **Documented by NVIDIA**: SM, TPC (2 SMs/TPC), GPC, L2 "partitions" (two on A100;
  "the partition" on H100 without a count), "partitioned crossbar", L2 slices (A100: 80, 8 per
  memory controller), 512-bit memory controllers and HBM stack counts, FBP/FBPA/LTC/LTS as
  unit names (Nsight, driver), CPC as a name (patent text; driver has a
  `LITTER_NUM_CPC_PER_GPC` query but no published value).
- **Measured**: two latency "partitions" on A100/H100 (Jin, DGNA, GemAtlas); 2×5×8 = 80 L2
  slices on H100 and 2×6×8 = 96 on H20 (GemAtlas); 4 GPCs per partition (DGNA; GemAtlas
  H100 4+4, H20 4+3); CPC-dependent SM-to-SM latency (Jin).
- **Inferred**: the CPC as a bandwidth level (Jin: "existence ... is not confirmed"); one
  FBP per 512-bit memory controller on H100 (count match only); 8 HBM3 channels per FBP
  (arithmetic from JEDEC + "512-bit"); anything about H200 beyond capacity/bandwidth.

**Is the GPC the largest SM grouping?** It is the largest grouping exposed to software
(clusters run "within a GPC") and the scope of the SM-to-SM network. Physically there is a
larger grouping: the half-die L2 partition / "micro GPU", to which a set of GPCs is "directly
connected" (WP p37; patent US12020035 FIG. 6 "each micro GPU includes a plurality of
GPCs"). Measured as 4 GPCs per partition on A100/H100 (DGNA) and an asymmetric 4+3 on H20
(GemAtlas). DGNA further reports a "sub-NUMA" grouping of 2 GPCs inside a partition on H100.

### Counts

| | V100 | A100 (SXM4) | H100 SXM5 | full GH100 | H200 |
|---|---|---|---|---|---|
| SMs | 80 [Jin T.I] | 108 [A100 WP p19] | 132 [H100 WP p18] | 144 [WP p18] | not stated by NVIDIA in pages read |
| TPCs | 40 [Jin T.I] | 54 [Jin T.I] | 66 [WP p18] | 72 (9/GPC) [WP p18] | — |
| GPCs | 6 [Jin T.I] | 7 [A100 WP p19] | 8 [WP p18] | 8 [WP p18] | — |
| Max SMs/GPC | 14 [Jin T.I] | 16 [Jin T.I; A100 WP "up to 16 SMs/GPC"] | 18 [Jin T.I] | 18 | — |
| CPCs/GPC | — | — | 3 (inferred, Jin Fig. 7a); 3 in patent FIG. 7 embodiment (arithmetic: 24 ports / (2 arbiters × 4 ports)) | — | — |
| L2 partitions | 1 (implied; Jin) | 2 [A100 WP p35] | 2 (measured: Jin, DGNA, GemAtlas; WP p37 says "the partition" without count) | — | — |
| L2 slices | 32 [Jin T.I] | 80 = 2×40 [A100 WP p35]; **GemAtlas measures 2×5×4 = 40 on A800 — conflict** | 80 [Jin T.I]; 2×5×8 measured [GemAtlas T.III] | 96 in patent FIG. 7 embodiment (2 uGPU × 6 FBP × 8) | 96 modelled by Accel-Sim `SM90_H200` (48 ch × 2); H20 measured 2×6×8 = 96 (GemAtlas). **Not documented for H200.** |
| FBPs | — | 10 (GemAtlas A800: 2×5) | 10 (GemAtlas: 2×5) | 12 (patent embodiment) | inferred 12 |
| Memory controllers | 8 [Jin T.I] | 10 × 512-bit [A100 WP p19] | 10 × 512-bit [WP p18] | 12 × 512-bit [WP p18] | not stated |
| HBM stacks | 4 (8 MCs; not re-read here) | 5 HBM2 [A100 WP p19] | 5 HBM3 [WP p18] | 6 [WP p18] | NVIDIA pages give only "141GB HBM3e, 4.8TB/s"; 6 stacks inferred |
| HBM channels | — | — | 80 (inferred: 5 × 16 per JEDEC HBM3) | 96 (inferred) | 96 (inferred) |

### Quotes

- Jin, Sec. II-A, p2: "The GPU architecture is hierarchical: within a Texture Processing Cluster (TPC), two SMs are connected by a multiplexer; 7 to 9 TPCs form a Graphics Processing Cluster (GPC), and 6 to 8 GPCs form a GPU [12], [20], [21]." (This is background citing NVIDIA WPs; the WPs do not say "multiplexer"; that word is Jin's.)
- Jin, Sec. II-A, p2: "Each memory partition (MP) contains multiple L2 slices and a memory controller (MC) managing the off-chip memory [18], [19]."
- Jin, Sec. II-A, p2: "Recent GPUs (e.g., A100, H100) include a central interconnect that splits the GPU into left and right "partitions"."
- Jin, Sec. III-C, p5, footnote 4: "While NVIDIA documentation does not disclose this level of hierarchy, some hardware teardown [40] have also identified a potential intermediate level ... "CPC" or Compute Processing Cluster. ... The existence of this hierarchy is not confirmed".
- Jin, Sec. III-C, p5: "A group of 4 or 6 SMs (i.e. 2 or 3 TPCs) have similar latency characteristics ... suggests the potential existence of a new hierarchy of cores between the TPC and GPC".
- H100 WP p18: "8 GPCs, 72 TPCs (9 TPCs/GPC), 2 SMs/TPC, 144 SMs per full GPU ... 6 HBM3 or HBM2e stacks, 12 512-bit Memory Controllers 60 MB L2 Cache"; SXM5: "8 GPCs, 66 TPCs, 2 SMs/TPC, 132 SMs per GPU ... 80 GB HBM3, 5 HBM3 stacks, 10 512-bit Memory Controllers 50 MB L2 Cache"; PCIe: "7 or 8 GPCs, 57 TPCs ... 114 SMs ... 5 HBM2e stacks, 10 512-bit Memory Controllers".
- H100 WP p29: "The Clusters in H100 run concurrently across SMs within a GPC. A GPC is a group of SMs in the hardware hierarchy that are always physically close together."
- H100 WP p37: "Using a partitioned crossbar structure, the L2 cache localizes and caches data for memory accesses from SMs in GPCs directly connected to the partition."
- A100 WP p35: "The L2 cache is divided into two partitions to enable higher bandwidth and lower latency memory access. Each L2 partition localizes and caches data for memory accesses from SMs in the GPCs directly connected to the partition." ... "Each L2 cache partition is divided into 40 L2 cache slices. Eight 512 KB L2 slices are associated with each memory controller."
- A100 WP p19: "7 GPCs, 7 or 8 TPCs/GPC, 2 SMs/TPC, up to 16 SMs/GPC, 108 SMs ... 5 HBM2 stacks, 10 512-bit Memory Controllers".
- US12020035, FIG. 6: "each micro GPU includes a plurality of GPCs each with a plurality of SMs." "Each GPC connects to the L2 cache via a crossbar and per L2 slice LRC."
- US12020035, FIG. 7: "the GPU 700 includes two micro-GPUs 702." "Crossbar switch 706 is a switch network from LRC, or more specifically 6 frame buffer partitions (FBPs) ... where each FBP has 8 slices with 2 ports per slice, to SMs." "a CPC is a set of TPCs in the GPC that are optimized together in the physical design of the GPC." "Each CPC includes two arbiters, each having 4 ports to yield 24 ports of input to each GPC."
- GemAtlas Sec. II, p2: "This L2 is distributed across partitions, each connected to a subset of GPCs [24], [33]. Each partition contains multiple Frame Buffer Partitions (FBPs) that interface with the memory controllers directly, and each FBP groups multiple independently addressed L2 slices."
- GemAtlas Sec. V-A, p6: "Partition0 connects GPC0, GPC1, GPC2, and GPC4, whereas Partition1 connects GPC3, GPC5, and GPC6" (H20; read from `SR_SW_SCRATCH`, "0xA0000000 for 42 SMs and 0x80000000 for the other 36").
- GemAtlas Table III, p9: L2 "Part.×FBPs×slices": H20 2×6×8, H100 2×5×8, A800 2×5×4, RTX 4090 1×6×6; "GPCs per partition": H20 4,3; H100 4,4; A800 4,3.
- DGNA p2: "We find that 2 GPCs, each containing a group of SMs, connect to a sub-NUMA node, while 4 GPCs connect to an L2 NUMA node on both the A100 and H100."
- DGNA p11 (Sec. 5.1): "Both GPUs are equipped with 5 HBM stacks, with each stack having 2 memory controllers. Furthermore, both A100 and H100 have 2 L2 NUMA nodes."
- Nsight Compute guide 2.3.4: "gpc: The General Processing Cluster contains SM, Texture and L1 in the form of TPC(s)." "fbpa: The FrameBuffer Partition is a memory controller which sits between the level 2 cache (LTC) and the DRAM." "The number of FBPAs varies across GPUs." "lts: A Level 2 (L2) Cache Slice is a sub-partition of the Level 2 cache." "ltcfabric: The LTC fabric is the communication fabric for the L2 cache partitions."
- open-gpu-kernel-modules `src/common/sdk/nvidia/inc/ctrl/ctrl2080/ctrl2080fb.h` (FBP_COUNT doc): "Starting with Fermi the term partition is an ambiguous term, both FBP and FBPA mean FB partitions. The FBPA is the low level DRAM controller, while a FBP is the aggregation of one or more FBPAs, L2, ROP, and some other units." Also "LTC_COUNT: Returns the active LTC count across all active FBPs." "LTS_COUNT: Returns the active LTS count across all active LTCs."
- open-gpu-kernel-modules `ctrl0080gr.h`: query indices exist for `LITTER_NUM_LTC_PER_FBP`, `LITTER_NUM_SLICES_PER_LTC`, `LITTER_NUM_FBPA_PER_FBP`, `LITTER_NUM_CPC_PER_GPC`, `LITTER_NUM_MXBAR_FBP_PORTS`. **No GH100 values are published** (`published/hopper/gh100/hwproject.h` has none; GA100's has `NV_SCAL_LITTER_NUM_FBPAS 24`).
- open-gpu-kernel-modules `kern_mem_sys_ga100.c` `kmemsysIsPagePLCable_GA100`: branches on `ltsPerLtcCount * ltcCount != 80` and `ltsPerLtcCount != 4`, i.e. the code expects an 80-LTS configuration with 4 LTS per LTC on GA100 (so 20 LTCs; arithmetic).

### Table A

| claim | source | tag | confidence |
|---|---|---|---|
| 2 SMs per TPC; H100 SXM 8 GPC / 66 TPC / 132 SM; full GH100 8/72/144 | H100 WP p18 | documented | high |
| The two SMs of a TPC share a multiplexer into the NoC | Jin II-A p2 | inferred/background (word not in NVIDIA docs) | medium |
| CPC = 2–3 TPCs between TPC and GPC | Jin III-C p5 (latency correlation), patent US12020035 FIG. 7 (name and definition), driver `LITTER_NUM_CPC_PER_GPC` (name) | name documented; size measured-by-latency only | medium (existence high, sizes medium) |
| 3 CPCs per GPC on H100 | Jin Fig. 7(a); patent FIG. 7 embodiment arithmetic | inferred | medium |
| L2 is split into 2 partitions, each serving the GPCs directly connected | A100 WP p35 (documented for A100); H100 WP p37 (structure, no count); Jin/DGNA/GemAtlas (measured on H100) | documented + measured | high |
| 4 GPCs per partition on H100 | DGNA p2; GemAtlas T.III | measured | high |
| A100 has 80 slices, 8 × 512 KB per memory controller | A100 WP p35 | documented | high |
| H100 has 80 slices = 2 partitions × 5 FBPs × 8 | GemAtlas T.III; Jin T.I (80) | measured | high (count); medium (FBP grouping, via latency clustering) |
| A800 has 2×5×4 = 40 slices | GemAtlas T.III | measured, **conflicts with A100 WP (80)** | low |
| H200 has 12 FBPs / 96 slices / 96 HBM channels | none (H20 and patent embodiment and Accel-Sim config only) | inferred | low–medium |
| GPC is the largest SM grouping | WP p29 (programming), WP p37 + patent FIG. 6 (partition/uGPU groups GPCs) | documented | high: GPC is largest *programmable* grouping; the uGPU/partition is a larger physical grouping |

---

## B. L2 organisation

### Answer

- **Slices**: 80 on A100 (documented) and H100 (measured), 96 on H20 (measured). Each
  partition holds half the slices, grouped by FBP (8 slices per FBP on H100/H20, measured;
  "Eight ... L2 slices are associated with each memory controller" on A100, documented).
- **Is L2 partitioned per FB partition, selected by address?** Yes for the *home* location:
  GemAtlas recovers an XOR hash that maps a physical address to (partition bit, FBP, slice).
  But **a line can live in two slices**: a remote-partition read leaves a coherent copy in the
  requester's partition (GemAtlas V-E; DGNA; Accel-Sim 2.0 paper). So "the slice is a pure
  function of the address" holds for the home copy only.
- **"2 partitions"** is both things at once: a physical left/right split of the die (Jin:
  "splits the GPU into left and right partitions"; GPCs 0–3 left, 4–7 right in Fig. 6), and a
  NUMA-like near/far split relative to each GPC (WP: each partition serves "SMs in GPCs
  directly connected to the partition"). DGNA calls them "L2 NUMA nodes" and also finds a
  smaller "sub-NUMA" split inside each H100 partition.
- **Near/far latency numbers** come from three different methods; they agree on the gap
  (~160–180 cycles) but not on absolute values:

| GPU | source / method | near (L2 hit) | far (L2 hit) | gap | DRAM near / far | DRAM gap |
|---|---|---|---|---|---|---|
| A100 | DGNA (GMM over latency distributions, own SASS-level-like loads, p2/p12) | 218 | 379 | **161** | 385 / 546 | **161** |
| H100 SXM | DGNA (same) | 295 | 470 | **175** | 554 / 728 | **174** |
| H100 SXM | DGNA sub-NUMA within local partition (Fig. 7, p12) | 280 | 312 | **~32** | — | — |
| H20 | GemAtlas (SASS `S2R SR_CLOCKLO` around one load, V-A p6) | ~276 | ~446 | ~170 | ~530–570 / ~700–750 | ~170 |
| A100 | Jin (`ld.cg`, one thread, `clock()`, Fig. 8b, p6) | ~220 (figure-read; text: "similar to V100") | "approximately 400 cycles" | ~180 (figure-read) | miss penalty "relatively constant" | — |
| H100 | Jin (same, Fig. 8c/8f) | L2-hit latency "much more uniform" (~300, figure-read) because of local caching | — | — | miss penalty bimodal ~280 vs ~460 (figure-read) | ~180 (figure-read) |
| H100 PCIe | Chips and Cheese (OpenCL) | — | "nearly twice as long" | no number | — | — |

The 161 / 174–175 / ~32 numbers in our notes are **DGNA's**, not GemAtlas's. GemAtlas's own
number is "~170-cycle" (H20). All are in SM clock cycles at whatever clock the GPU ran.

### Quotes

- GemAtlas Sec. V, p5: "We find that its 96 slices are grouped into 12 FBPs across the two partitions and are selected by an XOR hash with a 128-to-96 remapping."
- GemAtlas V-A, p6: "Local and remote L2 hits take ∼276 and ∼446 cycles, and local and remote HBM accesses take ∼530–570 and ∼700–750 cycles, respectively. Both therefore incur a similar ∼170-cycle cross-partition penalty."
- GemAtlas V-B, p6: "We measure Ncollide = 12,288, which gives a slice count of S = 85.333 on H20. ... We infer a two-stage remapping design ... We therefore conclude that H20 has 96 active physical L2 slices, with 48 slices per partition and 640 KiB per slice." "We therefore conclude that each partition consists of 6 FBPs with 8 slices each" (FBPs found by latency-correlation clustering; "inferred" in the source's own wording for the remap).
- GemAtlas V-B, p6: "Crossing partitions thus adds an almost uniform penalty without changing the relative SM-to-FBP distances, which implies a one-to-one correspondence between the six FBPs of the two partitions".
- GemAtlas V-E, p7–8: "These copies continue to contend for a single cache set, showing that coherence preserves this cache set indexing" ... "each cache set can hold any mixture of lines mapped to that partition and coherent copies from the peer partition".
- DGNA p2: "on the A100, we observe a NUMA latency gap of 161 cycles for DRAM and 161 cycles for L2 caches, while on the H100, the NUMA latency gaps on DRAM and L2 are 174 cycles and 175 cycles, respectively." "H100 incorporates sub-NUMA nodes [4] within all L2 NUMA nodes, introducing an access latency gap 32 cycles".
- DGNA p12: "the latency overhead for accessing a remote sub-NUMA node is minimal, approximately 32 cycles." Fig. 7 caption: "The mean latency values are 280 cycles (μ0) and 312 cycles (μ1)".
- DGNA p2: "for read operations, home SMs directly read the data block into the L2 home node, while remote SMs buffer data into both L2 home and remote NUMA nodes."
- Jin III-C, p6: "for GPCs 0,3,4,5, the destination L2 slices were on the far partition – resulting in a much higher latency of approximately 400 cycles – caused by the extra latency of traversing back and forth across the GPU partitions." "H100 also has two partitions, similar to A100. However, the latency values are much more uniform across the GPCs (Figure 8(c))."
- Jin abstract, p1: "The non-uniformity can result in up to approximately 70% difference in on-chip latency."
- Chips and Cheese: "H100 also inherits A100's split L2 configuration." "Accessing the "far" partition takes nearly twice as long."
- Accel-Sim 2.0 paper, Sec. II-C: "each uGPU owns private L2 partitions and directly attached High Bandwidth Memory (HBM) stacks." "Once the remote access is resolved, the retrieved data is replicated and cached in both L2 partitions." "the hardware uses a hybrid "local write-through, remote write-back" policy."
- Nsight Compute 2.4.6: device attributes `device__attribute_l2s_count` "Total number of Level 2 cache slices.", `device__attribute_num_l2s_per_fbp` "Number of Level 2 cache slices per frame buffer partition.", `device__attribute_fbp_count` "Total number of frame buffer partitions." (These can be read on our H200 with `ncu --query-metrics`/device attributes to settle the H200 counts.)

### Table B

| claim | source | tag | confidence |
|---|---|---|---|
| H100 L2 has 2 partitions | Jin, DGNA, GemAtlas, Chips and Cheese | measured | high |
| Each FBP owns its slices; home slice chosen by an XOR address hash | GemAtlas V-C (H20) | measured (hash recovered on virtual addresses inside 2 MiB pages) | medium-high |
| Lines are replicated into the requester's partition on remote reads | GemAtlas V-E, DGNA p2, Accel-Sim 2.0 II-C, WP p37 ("localizes and caches") | measured; WP wording consistent | high |
| A100 near/far L2 gap 161 cycles | DGNA p2 | measured (GMM) | medium-high |
| H100 near/far gap 174 (DRAM) / 175 (L2) cycles | DGNA p2 | measured | medium-high |
| H100 sub-NUMA gap ~32 cycles, 2 GPCs per sub-NUMA node | DGNA p2, p12 | measured / inferred (sub-NUMA grouping via GMM + K-means) | medium |
| Cross-partition penalty ~170 cycles on H20 | GemAtlas V-A | measured | medium-high |
| Far L2 "nearly twice as long" | Chips and Cheese (H100 PCIe) | measured (no number) | medium |
| Partition interleave ~4–8 KB | DGNA p13 (A100 8 KB; H100 4 KB-unit formula), Accel-Sim 2.0 (H100 8192 B), GemAtlas Eq. (1) partition bit from PA[12] up | measured | medium (sources differ in detail) |

---

## C. Memory controllers, channels, address mapping

### Answer

- **H100 SXM**: 5 HBM3 stacks, 10 × 512-bit memory controllers (documented). GemAtlas
  measures 10 FBPs (2×5). Matching counts → one FBP per 512-bit controller (inferred). HBM3
  has 16 × 64-bit channels per 1024-bit stack (JEDEC, via secondary sources), so 80 channels
  and 8 channels per 512-bit controller/FBP (arithmetic, inferred).
- **H200**: NVIDIA's pages give only 141 GB HBM3e and 4.8 TB/s. Six stacks / 12
  controllers / 96 channels follow only from "full GH100 has 6 stacks, 12 512-bit
  controllers" (WP p18) — **inferred**. The closest measured part is H20 (also 6 stacks):
  12 FBPs, 96 slices.
- **FBPA vs FBP**: driver: an FBP aggregates "one or more FBPAs, L2, ROP"; Nsight: the FBPA
  "is a memory controller which sits between ... LTC and the DRAM". GH100 FBPA count and
  FBPA-per-FBP are not published. GH100 driver code reads 4 ECC counters per FBPA
  (`NV_PFB_FBPA_0_ECC_DED_COUNT__SIZE_1 4`, indexed by `subp`), suggesting 4 sub-partitions
  per FBPA (inferred, low).
- **Address → partition/FBP/slice** (GemAtlas Eq. (1), H20, bits 0–20 only):
  `p = PA12^PA13^PA15^PA17^PA19`; `f0 = PA8^PA12^PA16^PA18^PA20`; `f1 = PA10^PA13^PA16^PA17^PA19^PA20`;
  `f2 = PA11^PA14^PA18^PA19`; `s0 = PA8^PA16^PA17`; `s1 = PA10^PA16`; `s2 = PA11^PA16^PA17`;
  then logical FBPs 6,7 fold onto physical pairs (0,1), (2,3), (4,5) in rotation every 128 KiB
  (Eq. 2). Consequences (read directly off the equations): bits 0–7 never route, so the
  routing granule is **256 B**; bit 9 appears in none of the printed equations; the
  partition bit changes no finer than **4 KB** (lowest bit PA12).
- **Other granularity statements**: DGNA (A100) "the allocation alternates every 8KB";
  DGNA (H100) a 4 KB-unit formula with a 64 KB period (its first 32 KB pattern
  0,1,1,0,0,1,1,0 equals PA12⊕PA13, consistent with GemAtlas's partition bit; this
  consistency check is mine); Accel-Sim 2.0 paper (H100) "a uGPU mapping stride of 8192B".
  Accel-Sim configs use `dramid@8` (channel field starts at bit 8 → 256 B) with
  `-gpgpu_memory_partition_indexing 6` (IPOLY_MODULO) and `chiplet_partition_stride` default 8192.
- **The "1 KB interleave" figure is not supported by any source read.** The measured routing
  granule is 256 B (GemAtlas, H20); partition-level granule 4–8 KB.
- **Patent US11429534** (embodiment, not product): slices are "dedicated to" one memory
  channel, with a few "shared" slices; the routing "do[es] not require a full-crossbar switch".
  Its example FBP has two memory channel controllers. Our notes' phrase "less than a full
  crossbar" is **not verbatim**.

### Quotes

- H100 WP p36: "The H100 SXM5 GPU raises the bar considerably by supporting 80 GB (five stacks) of fast HBM3 memory, delivering over 3 TB/sec of memory bandwidth".
- GemAtlas V-C, p7: "These equations define a 7-bit logical routing space consisting of one partition bit p, a 3-bit logical FBP index flog = (f2 f1 f0) ∈ {0, . . . , 7}, and a 3-bit slice index s = (s2 s1 s0)". "H20 has only 2 × 6 × 8 = 96 active physical slices". "Modern NVIDIA GPUs commonly allocate memory using 2 MiB large pages, whose offset bits (bits 0–20) are identical in virtual and physical addresses ... We therefore report only this portion of the routing hash."
- GemAtlas V-B, p6: "each memory-side controller sustains more than 300 GiB/s in our measurements, which our probes cannot saturate."
- DGNA p13: "On the A100, the allocation alternates every 8KB, where each 8KB portion corresponds to a specific NUMA node. On the H100, The NUMA memory is allocated based on a formula" ID = ⌊(x+1−H(x−8))/2⌋ mod 2, x = ⌊(address mod 64KB)/4KB⌋.
- Accel-Sim 2.0 paper II-C: "each 8192B region alternates between slow and fast accesses. This reveals a uGPU mapping stride of 8192B, matching that of the A100 [35]."
- US11429534 abstract: "A system in having M memory controllers between a first memory and a second memory having N operative memory slices, where N and M are not evenly divisible, includes logic to operate the M memory controllers to linearly distribute addresses of the second memory across the N operative memory slices."
- US11429534: "Each slice of the last level cache 112 is dedicated to a particular one or more of the memory channels"; "most of the slices communicate directly with only one particular memory channel."; "The addressing schemes do not require a full-crossbar switch to achieve full connectivity between the slices in the LLC and the memory channels."; "the first memory channel controller 128 and second memory channel controller 130 are parts of a first frame buffer partition 136."; "For a 128 byte cacheline, O[6:0] is the address of the data within each cacheline".
- Accel-Sim `SM90_H100/gpgpusim.config`: `-gpgpu_n_mem 40`, `-gpgpu_n_sub_partition_per_mchannel 2`, `-gpgpu_cache:dl2 S:256:128:20,...` (256 sets × 128 B × 20 ways = 640 KB per sub-partition × 80 = 50 MB), `-gpgpu_mem_addr_mapping dramid@8;...`, `-gpgpu_memory_partition_indexing 6` ("Use IPOLY_Modulo for non-2^k partition count"), `-gpgpu_n_chiplet_partition 2`, `-inter_chiplet_queue_size 256`. `SM90_H200` differs only in `-gpgpu_n_mem 48` (→ 96 sub-partitions, and with the same dl2 line, 60 MB of modelled L2).

### Table C

| claim | source | tag | confidence |
|---|---|---|---|
| H100 SXM: 5 HBM3 stacks, 10 × 512-bit MCs | H100 WP p18 | documented | high |
| H100: 10 FBPs | GemAtlas T.III | measured | medium-high |
| 1 FBP per 512-bit MC on H100 | count match (WP + GemAtlas) | inferred | medium |
| 80 HBM3 channels on H100 / 8 per FBP | JEDEC 16 ch/stack (secondary) + arithmetic | inferred | medium |
| H200: 6 stacks, 12 MCs, 96 channels | WP p18 (full GH100 only) | inferred | low–medium |
| Routing granule is 256 B | GemAtlas Eq. (1) (H20) | measured | medium-high (H20 only) |
| Partition interleave 4 KB (hash bit 12) / 8 KB alternation | GemAtlas, DGNA, Accel-Sim 2.0 | measured | medium |
| 1 KB interleave | — | **unsupported** | — |
| Accel-Sim uses 256 B channel interleave | Accel-Sim config `dramid@8` | documented (simulator) | high |
| Slice-to-channel routing is not a full crossbar | US11429534 | documented (patent embodiment) | medium for product |

---

## D. The GPC uplink: what Jin et al. measured, and how it squares with our H200 result

### What the paper says, exactly

Jin Sec. IV-A, p7: "Speedup was measured as the bandwidth (performance) ratio of x SMs
compared to one SM, with the SMs sending traffic to all L2 slices. For TPC speedup, x = 2
while for GPCl, x is the number of TPCs and for GPCg, x is the number of TPCs multiplied by
the number of SMs per TPC." Footnote 6: "GPCl is local speedup between the TPC and GPC
hierarchy while GPCg is global speedup when all SMs within the GPC are sending traffic."

Jin p7: "For TPC speedup, full bandwidth is provided for Reads across all GPUs (i.e., TPC
speedup of 2); however, for Writes, a speedup of only 1.09× is achieved on the V100. For A100
and H100, full bandwidth is also achieved for the TPC Writes. For GPCl, achieving full
bandwidth requires a speedup of 7, 8, and 9 for the V100, A100, and H100, respectively. V100
reaches about 50% of this speedup while the H100 approaches 85%, with a speedup close to 8.
... In particular, GPCg provides additional speedup compared to GPCl, especially for A100 and
H100. For H100, CPC speedup is also shown and results show that the added hierarchy does
not have any impact on Read accesses but when all SMs within the CPC are utilized for
Writes, there is a slight performance degradation as only a speedup of approximately 4.6 is
provided (compared to a speedup of 6 needed for full bandwidth)."

Fig. 10 (p7), figure-read, H100: TPC read ≈ 2, write ≈ 2; CPC read ≈ 5.4, write ≈ 4.5;
GPCl read ≈ 7.8, write ≈ 9; **GPCg read ≈ 9, write ≈ 12.4** (needed: 18).

Test conditions (Sec. II-C-2, p3): Algorithm 2 ("ldcg(&D[index])" per thread, element size not
given), "Sequential, strided memory access pattern", "L1 cache was bypassed", "L2 cache was
warmed up to ensure all accesses hit in the L2", "Unless otherwise stated, read accesses were
performed". Footnote 2: "we measure the interconnect or fabric bandwidth to/from the L2."
**No absolute per-SM GB/s is given for the speedup baseline on any GPU.** The only per-SM
absolute numbers are to *one slice*: V100 "approximately 34 GB/s", A100 near ~39.5 / far ~26
GB/s, H100 single peak ~40–45 GB/s (Fig. 13b, figure-read).

So the correct reading of "85%" is: **GPC_l on H100, i.e. x = 9 (number of TPCs; the paper
does not say which SM of each TPC was used, presumably one), L2-resident reads to all slices,
reaches ~7.8× a single SM's bandwidth.** It is *not* "85% of the sum of all 18 SMs". With all
SMs of the GPC (GPC_g) reads reach ≈ 9× a single SM (figure-read), ~50% of 18; writes ≈ 12.4×.
Our notes' "needs 9×, gets ~8×" is right for GPC_l; "provisioned at about 85% of its SMs' sum"
is wrong.

### Additional Jin results relevant to the uplink

- p8, Fig. 15(b,c) (V100): "Contiguous SM places all SMs in 1 GPC (for 14 SMs) or 2 GPCs (for 28 SMs) and compared to distributed SM ... there is significant performance degradation – approximately 62% when 28 SMs are accessing a single MP – caused by the limited GPC speedup." "with contiguous SM, as the number of MP destinations increased from 1 to 4, performance increased by 218% – thus, demonstrating that some of the GPC speedup is provided in space (i.e., additional connectivity) and not entirely in time (i.e., more bandwidth)." (Observation #11, p8.)
- p8, Fig. 14 (A100, one slice): "At approximately 8 SMs, the bandwidth saturates regardless of whether the L2 is on the near partition or the far partition." (~195 GB/s per slice, figure-read.)
- p7 (V100): "to saturate a single L2 slice bandwidth, a minimum of 4 SMs are needed." "When all SMs within a GPC send traffic to a single L2 slice, the achievable bandwidth is approximately 85 GB/s".

### Our measurement vs theirs

Ours (H200): a GPC returns at most ~1.28 TB/s from DRAM, reached with ~6 SMs at ~210 GB/s
each (16/32 KB TMA or 16 B loads); both SMs of a TPC give 2×.

How both can be true — the assumptions that differ:

1. **Ratio vs absolute.** Jin reports multiples of one SM's own rate in *their* kernel. If their
   single-SM L2-hit rate on H100 was B1, the GPC delivered ≈ 7.8·B1 (9 SMs, GPC_l) or ≈ 9·B1
   (18 SMs, GPC_g). Our cap equals ≈ 6 × 210 GB/s. The two agree if B1 ≈ 140 GB/s
   (9 × 140 ≈ 1.26 TB/s). Jin never states B1, so this is an arithmetic consistency check, not
   a finding. If their B1 were ≈ 210 GB/s, GPC_g ≈ 9 would imply ≈ 1.9 TB/s and would
   disagree with us.
2. **Per-SM demand.** Their single-SM baseline is whatever one SM achieves with `ld.cg`
   per-thread loads; an SM issuing 16 B vector loads or TMA bulk copies likely pulls more,
   which would make fewer SMs saturate the same uplink (our ~6 vs their "~8–9 SM-equivalents").
   Unverified: the element width in Algorithm 2 is not given.
3. **L2-resident vs DRAM.** Theirs is L2 hit ("fabric bandwidth"); ours is DRAM-sourced, so it
   also includes DRAM and, for roughly half the lines (those homed in the far partition), the
   partition crossing. A DRAM-limited cap would show up below an L2-hit cap; our 1.28 TB/s is
   therefore an upper bound on nothing L2-side and may be lower than the GPC's L2-hit uplink.
4. **Read vs write.** H100 writes get more GPC speedup (≈ 12.4×) than reads (≈ 9×) because a
   read reply carries data on the return path. Our test is a read; compare it with their read bars.
5. **Destinations ("space").** Jin shows GPC bandwidth grows with the number of destination
   MPs. Our DRAM stream spreads over all FBPs, i.e. the most favourable case; their speedup
   test also targets "all L2 slices", so this does not separate the two.
6. **Device.** H100 (SXM or PCIe, ambiguous in Table I) vs our H200 (same GH100 die, more
   HBM). No source measured an H200.
7. **TPC 2×** agrees exactly with Jin's TPC read speedup of 2 on all three GPUs.

Rough per-clock view (arithmetic, ours): 1.28 TB/s at 1.98 GHz ≈ 650 B/clk per GPC; 210 GB/s ≈
106 B/clk per SM. For comparison Jin Fig. 9(a) H100 aggregate L2-fabric ≈ 7 TB/s (figure-read),
i.e. ≈ 0.9 TB/s per GPC when all 8 GPCs run at once — below our isolated-GPC cap, so with all
GPCs active the bottleneck would be L2/fabric, not the GPC uplink.

### Table D

| claim | source | tag | confidence |
|---|---|---|---|
| H100 GPC_l (x = 9 TPCs) read speedup ≈ 7.8 of 9 needed ("approaches 85%") | Jin p7 text + Fig. 10 | measured | high |
| H100 GPC_g (all 18 SMs) read speedup ≈ 9, write ≈ 12.4 | Jin Fig. 10 | figure-read | medium |
| TPC read speedup 2 on V100/A100/H100 | Jin p7 | measured | high |
| CPC: no read impact; writes ≈ 4.6 of 6 | Jin p7 | measured | high |
| GPC bandwidth partly "in space" (depends on # destination MPs) | Jin p8 Obs. 11 (V100) | measured / inferred | medium |
| Test is L2-hit reads to all slices, relative to one SM, no absolute B1 | Jin II-C-2, IV-A | documented by the paper | high |
| "GPC provisioned at about 85% of its SMs' sum" | our notes | **not supported** (85% is GPC_l, 9 SMs) | — |

---

## E. SM-to-SM network

### Answer

Documented scope: one GPC ("A dedicated SM-to-SM network for SMs in a GPC"). NVIDIA gives
no bandwidth or latency figures, only "about 7x" faster data exchange than via global memory.
The tuning guide says DSMEM "can be used by an SM simultaneously with L2 cache accesses".
Cluster limits: portable 8, non-portable 16 (tuning guide); WP table lists "Max Thread Blocks
/ Thread Block Clusters ... 16" (p41).

Jin measures **latency only**: 196 cycles within CPC0 up to ~213 cycles for CPC2 (H100,
Fig. 7b, p5). Luo measures latency 180 cycles and aggregate throughput 3.27 TB/s (cluster 2)
/ 2.65 TB/s (cluster 4) on H800 PCIe with "one block per SM" (114 SMs), i.e. ≈ 29 GB/s per SM
on average (≈ 16 B/clk at 1.755 GHz; my arithmetic) — with a ring of remote register-add
operations, not bulk copies.

Forum (rumour-grade, author "Greg", not marked as NVIDIA staff on the page): "Each GPC has an
internal crossbar for DSMEM operations independent of GPU NOC used to access the L2." "Each
SM can store or read at 32B/cycle." "The recive [sic] port is 32B/cycle. This arbitrates vs.
the fill return ports for the memory crossbar." "The GPC internal crossbar is not detailed in
the profilers or documentation." Robert_Crovella (also unmarked): "I don't believe there are
published details to answer your questions."

### Quotes

- H100 WP p29: "A dedicated SM-to-SM network for SMs in a GPC provides fast data sharing between threads in a Cluster."
- H100 WP p30: "The dedicated SM-to-SM network for Clusters ensures fast, low latency access to remote DSMEM. Compared to using global memory, DSMEM accelerates data exchange between Thread Blocks by about 7x."
- H100 WP p22: "Distributed Shared Memory allows direct SM-to-SM communications for loads, stores, and atomics across multiple SM shared memory blocks."
- Hopper Tuning Guide 1.4.1.3: "Distributed Shared Memory can be used by an SM simultaneously with L2 cache accesses." "accesses to Distributed Shared Memory should be coalesced and aligned to 32-byte segments, if possible." "The maximum portable cluster size supported is 8" "NVIDIA Hopper H100 GPU allows for a nonportable cluster size of 16 by opting in". 1.4.1.2: TMA transfers "between the shared memory regions of different SMs in the same cluster".
- Jin III-C, p5: "latency is lowest (196 cycles) when two SMs within CPC0 communicate but increases to around 213 cycles for communications between SMs in CPC2 which is likely farthest away."
- Luo IV-E, p10: "SM-to-SM network latency is 180 cycles, a 32% reduction compared to L2 cache." "A peak throughput of nearly 3.27 TB/s is observed with a cluster size of 2, reducing to 2.65 TB/s with a cluster size of 4." Methodology III-D: "We launch one block per SM and gather them into clusters. In each cluster, we arrange threads in each block ranked by R to add their register values to those of the block ranked by (R + 1)%CS".

### Table E

| claim | source | tag | confidence |
|---|---|---|---|
| SM-to-SM network scope = one GPC | H100 WP p29 | documented | high |
| DSMEM concurrent with L2 accesses | Hopper Tuning Guide 1.4.1.3 | documented | high |
| Max cluster 8 portable / 16 opt-in | Tuning Guide | documented | high |
| SM-to-SM latency 196–213 cycles by CPC pair | Jin p5 | measured | high (H100) |
| SM-to-SM latency 180 cycles | Luo p10 | measured (H800) | medium-high |
| 3.27 / 2.65 TB/s aggregate (≈ 29 GB/s/SM average) | Luo p10 | measured; per-SM is my arithmetic | medium |
| 32 B/clk per SM, receive port arbitrates with fill returns | forum post | rumour | low |
| Topology not published | forum (two posters), Jin footnote 4 | documented absence | high |

---

## F. Per-SM numbers to compare with ours

| quantity | value | GPU | source, location | tag |
|---|---|---|---|---|
| L1 bandwidth | 125.8 B/clk/SM (FP32), 124.1 (FP32.v4) | H800 PCIe | Luo Table V p7 | measured |
| L1 bandwidth | 99.5 / 106.8 B/clk/SM | A100 PCIe | Luo Table V | measured |
| Shared-memory bandwidth | 127.9 B/clk/SM | H800 | Luo Table V | measured |
| Shared-memory bandwidth | 128.0 B/clk/SM | A100 | Luo Table V | measured |
| L2 bandwidth (whole GPU) | 4472.3 B/clk (FP32), 3942.4 (v4) → ≈ 39 B/clk/SM averaged over 114 SMs (my arithmetic) | H800 | Luo Table V | measured |
| L2 read bandwidth (whole GPU) | 5120 B/clk (A100), 2048 B/clk (V100) → 64 B/clk per slice (my arithmetic: /80, /32) | A100, V100 | A100 WP p35 | documented |
| Global memory bandwidth | 1861.5 GB/s (91% of 2039) | H800 PCIe | Luo Table V | measured |
| L1 hit latency | 40.7 cyc (Luo); 31 cyc (GemAtlas SASS) | H800 / H20, H100 | Luo Table IV p6; GemAtlas T.III | measured (method differs) |
| Shared-memory latency | 29.0 cyc | H800 | Luo Table IV | measured |
| L2 latency | 263.0 cyc (Luo, average) | H800 | Luo Table IV | measured |
| Global latency | 478.8 cyc | H800 | Luo Table IV | measured |
| L2 hit near/far | 295 / 470 (DGNA H100); ~276 / ~446 (GemAtlas H20) | H100, H20 | see B | measured |
| One SM → one L2 slice | ~34 GB/s (V100); ~39.5 near / ~26 far (A100); ~40–45 (H100, figure-read) | | Jin p7, Fig. 13 | measured |
| One GPC → one L2 slice | ~85 GB/s | V100 | Jin p7 | measured |
| One L2 slice saturates at | ~8 SMs, ~195 GB/s (figure-read) | A100 | Jin Fig. 14 p8 | measured |
| Aggregate L2 fabric / DRAM | V100 ~2 / ~0.8; A100 ~5.7 / ~1.7; H100 ~7 / ~2.8 TB/s (figure-read); "approximately 2.4× to 3.5× higher" | | Jin Fig. 9(a), p6 | measured |
| DRAM reaches | "approximately 85-90% of the peak" | V100/A100/H100 | Jin p6 | measured |
| H100 PCIe L2 | ">5.5 TB/s" near-footprint; 3.8 TB/s over 50 MB | H100 PCIe | Chips and Cheese | measured (OpenCL) |
| Per FBP ("memory-side controller") | "more than 300 GiB/s" | H20 | GemAtlas V-B p6 | measured |
| DSMEM | 180 cyc; ≈ 29 GB/s/SM average at full-GPU ring load | H800 | Luo p10 | measured / arithmetic |

---

## G. What matters for a simulator model

Levels the sources show as **bandwidth-limiting** (with conditions):

1. **GPC uplink**: GPC_l ≈ 7.8/9 and GPC_g ≈ 9/18 of one-SM rate for H100 reads (Jin);
   bandwidth partly "in space" — depends on how many MPs/FBPs a GPC targets (Jin Obs. 11).
   Model it as several ports/links, not one pipe.
2. **L2 slice input**: one slice saturates at ~4 SMs (V100) or ~8 SMs (A100, ~195 GB/s);
   whole-GPU L2 read is 64 B/clk/slice on A100/V100 (WP arithmetic).
3. **L2 fabric aggregate**: 2.4–3.5× DRAM (Jin); H800 ≈ 4472 B/clk (Luo).
4. **DRAM**: 85–91% of peak achievable (Jin, Luo).
5. **CPC (writes only)**: ≈ 4.6/6 (Jin).
6. **SM-to-SM network**: aggregate few TB/s (Luo); the receive port may share bandwidth with
   L2 fills (rumour only).
7. **LRC**: per-GPC request coalescer, "up to four read requests into a single transaction"
   from SMs in the same GPC (Accel-Sim 2.0 paper II-C, measured) — reduces L2 request traffic,
   so L2 traffic is not proportional to L1 misses.

Levels the sources show as mainly **latency-adding**:

- **TPC mux** (reads): full 2× bandwidth, no measurable read cost.
- **SM position within GPC / slice position within FBP**: up to ~70% latency spread (Jin),
  bandwidth "(mostly) uniform" (Obs. 8).
- **Partition crossing**: +161–175 cycles (DGNA), ~170 (GemAtlas). Bandwidth effect only
  through Little's law at low SM counts: A100 far slice is "up to 28% lower" for 1–2 SMs, equal
  at ≥ 8 SMs (Jin p8). Plus capacity effect from replicated lines (GemAtlas V-E, Accel-Sim 2.0).
- **H100 sub-NUMA**: +~32 cycles (DGNA).
- **CPC position for DSMEM**: 196 → 213 cycles (Jin).

Simulator state (Accel-Sim):
- `SM90_H100`: intersim `topology = fly; k = 212; n = 1` (132 SMs + 80 sub-partitions),
  `internal_speedup = 4.0`, 256-entry buffers, `flit_size = 40`, 2 subnets; 40 channels × 2
  sub-partitions; 256 B channel interleave with IPOLY_MODULO; `-gpgpu_n_chiplet_partition 2`,
  `-inter_chiplet_queue_size 256` (latency default 100 cycles, `gpu-sim.cc`); LRC enabled.
- `addrdec.cc fold_to_chiplet`: "H100 has 8 SMs per GPC. GPC abstraction is not yet
  implemented, so we hardcode 8 here to group SMs into chiplet-local GPC-sized clusters."
  (Note the comment's "8 SMs per GPC" is not the hardware's 16–18.)
- So: **current Accel-Sim `dev` (2.0, merged 2026-08-25) does model two L2 partitions, an
  inter-partition queue, the LRC, TMA multicast and DSMEM** (PR #143: "threadblock clusters
  with cluster-aware CTA scheduling and distributed shared memory", "a chiplet / uGPU
  partitioned L2"). It still has no TPC or GPC uplink level and a single flat crossbar.
- GemAtlas's baseline was Accel-Sim **v1.3.0** + Huerta et al.; its H100 MAPE 37.7% → 21.3%
  (Table V, p10) is against that older baseline. Largest gain came from the slice count
  (48 → 96 on H20), then hash, then partition model (VII-E p10–11).

---

## URLs fetched

Fetched and read:
- https://people.ece.ubc.ca/aamodt/publications/papers/realgpu-noc.micro2024.pdf (full; pages 6 and 9–13 have overlapping text layers, read from rendered page images)
- https://arxiv.org/abs/2611.13860 and https://arxiv.org/pdf/2611.13860 (full)
- https://arxiv.org/pdf/2607.19922 (DGNA; key pages 2, 9, 11–14)
- https://www.hpctech.co.jp/assets/images/info/catalog/pdf/gtc22-whitepaper-hopper_v1.02.pdf (H100 WP v1.02 mirror; searched in full)
- https://images.nvidia.com/aem-dam/en-zz/Solutions/data-center/nvidia-ampere-architecture-whitepaper.pdf (A100 WP; searched in full)
- https://docs.nvidia.com/cuda/hopper-tuning-guide/index.html
- https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html (characters 0–200,000 of 237,141)
- https://patents.google.com/patent/US12020035 (characters 0–100,000 of 221,541)
- https://patents.google.com/patent/US11429534 (characters 0–~193,887, in two passes)
- https://arxiv.org/pdf/2402.13499 (Luo; full)
- https://chipsandcheese.com/p/nvidias-h100-funny-l2-and-tons-of-bandwidth
- https://forums.developer.nvidia.com/t/what-is-the-inter-sm-linkage-of-dsm-cluster/300533
- https://github.com/NVIDIA/open-gpu-kernel-modules (sparse clone, commit a4b95f6a2d)
- https://raw.githubusercontent.com/accel-sim/gpgpu-sim_distribution/dev/configs/tested-cfgs/SM90_H100/{gpgpusim.config,config_hopper_islip.icnt}
- https://raw.githubusercontent.com/accel-sim/gpgpu-sim_distribution/dev/configs/tested-cfgs/SM90_H200/gpgpusim.config
- https://raw.githubusercontent.com/accel-sim/gpgpu-sim_distribution/dev/src/gpgpu-sim/{addrdec.cc,addrdec.h,l2cache.cc,gpu-sim.cc,shader.cc}
- https://api.github.com/repos/accel-sim/gpgpu-sim_distribution/pulls/143
- https://arxiv.org/abs/2608.22602 and https://arxiv.org/pdf/2608.22602 (Accel-Sim 2.0; Sec. II-C)

Search results used, not fetched in full (secondary):
- https://www.nvidia.com/h200 (search snippet: 141GB, 4.8TB/s, "Preliminary specifications")
- https://www.synopsys.com/designware-ip/interface-ip/hbm/hbm3-phy.html, https://www.theregister.com/2022/01/28/jedec_pushes_out_hbm3_memory/ (HBM3: 16 × 64-bit channels, 32 pseudo-channels)

Not fetched / not available:
- NVIDIA V100 whitepaper (V100 counts taken from Jin Table I only).
- No NVIDIA document giving H200 stack count, controller count, L2 size or slice count was found.
- No GH100 values for `LITTER_NUM_*` (FBP, LTC, LTS, CPC) are published in open-gpu-kernel-modules.
- Remaining text of the Nsight guide (last ~37k chars) and of US12020035 (last ~121k chars).
