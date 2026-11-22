# TPC placement and placement-dependent borrowing on a real H200

Status: done 2026-11-21. GPU: one H200 (node atl1-1-03-019-14-0, driver
615.71.09 = CUDA driver API 13040, toolkit 12.9.1), holder job 15954225
(`tpcmask_h200`): submitted 03:42:07, started 03:44:37, cancelled 04:24:45
(40 min). This is a different H200 die than the borrow_h200 agent's
(atl1-1-03-018-14-0); its unmasked cluster placement printed the same SM sets.

Tags: FACT = read in a source or measured here; INFERENCE = my reasoning
from facts; UNVERIFIED = not checked.

## 0. Answer first

**Per-launch TPC masks work on this H200 without root.** I reimplemented
libsmctrl's driver callback with all 128 mask bits (TMD V04). Every one of
the 66 live mask bits was checked against the `%smid` of the CTAs that ran.
libsmctrl's own tests fail here: they assume bit t = SMs 2t, 2t+1, and their
README warns that Hopper breaks this. Its stream-mask path does not support
driver 13.4.

**Topology (FACT, from masked cluster launches and bandwidth):**
- 8 physical GPCs: 8, 8, 8, 8, 8, 9, 9 TPCs plus one GPC of 4 + 4 TPCs.
- In that last GPC, 4 TPCs (SMs 124-131) never join a cluster larger than 2,
  but they share the GPC's bandwidth limit.
- Mask bits are GPC-major with 9 slots per GPC. TPC t of a GPC has SM ids
  {2g + 16t, +1} for its first four TPCs: SM ids interleave across GPCs.

**The main finding: a GPC returns at most about 1.28 TB/s of DRAM data**
(1277-1285 GB/s; L2-resident data 1.29-1.62 TB/s).
- One SM streams 205 GB/s (TMA, 32 KB x 6) or 239 GB/s (plain 16-byte
  loads). So the limit is reached at about 6 SMs (4 TPCs).
- No TPC-pair limit: both SMs of a TPC = 2 x 205. No CPC level either: any 3
  TPCs of a GPC give the same 1.19-1.20 TB/s.
- Results by placement (TMA, write-only):

| SMs | Packed | Spread over all GPCs |
|---|---|---|
| 16 | 1283 GB/s | 3084 GB/s |
| 32 | 2283 GB/s | 4557 GB/s |

- The chip's memory roof is 4.63 TB/s (all 132 SMs, TMA).
- 32 SMs spread 4 per GPC reach 4.45-4.56 TB/s (96-98%). 24 SMs spread 3 per
  GPC reach 4.03-4.05 TB/s (87%).

**Why the MPS sweep fell from 107 to 77 GB/s per SM** (INFERENCE, strongly
supported). Two causes:
1. bwsweep's 4-byte loads cap one SM at about 110 GB/s (an in-flight limit).
2. MPS gives a client SMs packed into few GPCs.

Evidence for the packing:
- A 6% MPS client with 16-byte loads gets 1272 GB/s from its 8 SMs. That is
  exactly one GPC's limit; 8 spread SMs get 1885-1893.
- At 32 SMs it gets 2563 GB/s: two GPC limits (spread: 4428).

**Answers to C.3**
- **Best placement on a real H200 today: disjoint SMs, spread over all 8
  physical GPCs, at most about 5-6 memory SMs per GPC.**
  - For a job that needs about 21-32 SMs at full rate: 3-4 SMs in every GPC
    (one SM per TPC or both SMs of a TPC does not matter).
  - Never fill one GPC first, which is what MPS appears to do.
  - Cost to the compute job: only the SMs it gives up. A disjoint FMA kernel
    keeps 0.997-1.001 of its solo rate on the same SMs.
- **"Spread memory SMs across GPCs": right, and it is the dominant effect**
  (2.4x at 16 SMs).
- **"Borrow within the GPC": borrowing adds nothing on the H200.**
  - Helper distance inside a GPC does not matter at all. A remote helper
    delivers 75.3 GB/s from any of the 9 TPCs, the sibling SM included.
  - A consumer fed by 3-7 helpers reaches 133-178 GB/s, below the 205 GB/s
    that one local thread gets with 32 KB x 6.
  - Every GPC is capped at 1.28 TB/s anyway, so borrowing inside a GPC can
    never lift a packed memory job above that.
- **Reclaimable bandwidth with b MEM TPCs, alone or beside a disjoint FMA job.**

| b (TPCs) | Spread: MEM gets | Spread: unused | Packed: MEM gets |
|---|---|---|---|
| 1 | 410 GB/s (9%) | 4.22 TB/s | 409 GB/s |
| 2 | 816 GB/s (18%) | 3.82 TB/s | 815 GB/s |
| 4 | 1.61 TB/s (35%) | 3.01 TB/s | 1.28 TB/s |
| 8 | 3.08 TB/s (67%; plain loads 3.54) | 1.5 TB/s | 1.28 TB/s |
| 12 (spread single) | 4.0 TB/s (87%) | | |
| 16 | 4.5 TB/s (97%) | | |

  - Software borrowing does not change these numbers.
  - Only more MEM SMs, or co-residency, can use the rest.
- **Shared SMs (MEM co-resident with compute) are fragile.** The warp
  scheduler behaves like greedy-then-oldest (INFERENCE).
  - If the compute kernel launched first, or its warps can issue every cycle
    (FP32 FMA with 4 independent chains), the co-resident stream keeps
    0.5-3% of its rate: 0.7-5.7 GB/s per SM. That holds for TMA and plain
    loads, with 8 to 32 compute warps.
  - Only with MEM resident first **and** compute ILP ≤ 2 does MEM keep 100%.
    Compute then loses 9-14% (FMA); 23-25% beside a 12-warp staged consumer.
  - At chip level (all 132 shared, ILP 2, MEM first): MEM 4.33 TB/s, compute
    0.86.
  - Compare disjoint: 32 spread MEM SMs give 4.46-4.51 TB/s, compute keeps
    100/132 SMs (0.76 of all-SM). With 24: 4.03 TB/s, compute 0.82.
  - A production GEMM leaves no residency room anyway (borrow agent: cuBLAS
    FP16 uses 64,512 of 65,536 registers). So disjoint and spread is the
    practical answer.
- **A real tensor-core GEMM on the other 100 SMs** (CUTLASS FP8, a second MPS
  client):
  - The 30-SM MEM stream drops from 4.49 to 3.66-3.73 TB/s, because the
    GEMM's own memory/L2 traffic competes.
  - The GEMM drops 10% (250.5 -> 226.0 TFLOP/s).
  - The masked GEMM got only 56% of its 132-SM rate on 76% of the SMs. A
    kernel sized for 132 SMs runs a second partial wave on 100. Whoever masks
    must also tell the kernel how many SMs it has, as LithOS does by
    rewriting `CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT`.

**What hardware would have to add beyond the software form** (INFERENCE):
- (1) Lending across GPC boundaries, or more GPC egress. A cluster stays in
  one GPC, and the GPC egress (1.28 TB/s) is the binding limit for any
  packed or GPC-local scheme.
- (2) Issue priority, or a reserved "memory lane", for a co-resident memory
  warp. Today it starves under a greedy compute warp.
- (3) No resident helper CTA: production GEMMs leave no registers or shared
  memory.
- (4) Cross-process lending without a common grid.
- Raising one SM's ingest (205-239 GB/s) matters only after (1): even 6 SMs
  at today's ingest fill a GPC.

## Part A — how per-launch TPC placement works

### A.1 LithOS (SOSP'25)

Coppock, Zhang, Solomon, Kypriotis, Yang, Sharma, Schatzberg, Mowry,
Skarlatos, "LithOS: An Operating System for Efficient Machine Learning on
GPUs", SOSP'25 (doi 10.1145/3731569.3764818). PDF
https://www.pdl.cmu.edu/PDL-FTP/BigLearning/lithos_sosp25.pdf , arXiv
https://arxiv.org/abs/2504.15465 , blog
https://www.cs.cmu.edu/~csd-phd-blog/2025/lithos/ .

What it does (FACT, from the paper text):
- **Interposition.** About 5000 lines of Rust that interpose the whole CUDA
  Driver API (a library "LibLithOS" that mimics libcuda, §5.1, §6). Kernel
  launches go into per-stream launch queues; dispatcher threads decide when
  and on which TPCs they run.
- **TPC Scheduler / quotas (§5.3).** Each application gets a TPC quota
  ("guaranteeing each application a specified number of TPCs when runnable
  work exists"). Outstanding work on the GPU is throttled to about 100 µs.
- **TPC stealing (§5.3).** Idle TPCs of one application are lent to
  another; per-TPC timers from a latency predictor avoid stealing TPCs that
  would block a newly arriving high-priority request; work on stolen TPCs
  gets lower hardware stream priority.
- **Kernel atomizer (§5.4).** A long kernel is launched several times
  ("atoms", up to 64), each with a block-index range; a "Prelude" entry
  point, injected by patching the program address in the QMD, skips blocks
  outside the range. Each atom can get a different TPC set. Disabled for
  kernels with cross-block synchronisation or persistent kernels.
- **Right-sizing (§5.5).** Per kernel, latency at all TPCs and at one TPC
  is measured and fitted with l = m/t + b (t = TPC count); the dispatcher
  picks the fewest TPCs whose predicted latency is within a "latency slip"
  factor k; an occupancy-based upper bound (blocks / blocks-per-TPC) caps it.
  Reported: about a quarter of GPU capacity saved at under 4% slowdown.
- **Mechanism for masks (§6, "TPCs and Atomization").** "we extend prior
  reverse-engineering work libsmctrl and add support for Hopper, including
  handling its new TPC masking layout ... we reimplement functionality to
  identify TPCs through the Queue MetaData (QMD) data structure and enable
  dynamic TPC allocation on kernel launch." So: a per-launch TPC mask written
  into the QMD/TMD, libsmctrl's mechanism. For thread-block clusters on
  Hopper: "We reverse-engineer these mappings and ensure atoms are always
  multiples of the cluster size." Across processes it "build[s] on top of
  MPS".
- **GPUs.** Functionality verified on A30, A100, H100, L4; all reported
  experiments on one A100-SXM4-40GB, CUDA 12.6 (§7).
- **Code.** Not public as far as I can find (no repository in the paper,
  the arXiv page or the blog; a web search found none).

What it does **not** do (FACT by absence, INFERENCE as to intent):
- TPC choice is a **count**, not a placement. Nothing in the paper selects
  *which* TPCs by GPC, CPC, L2 partition or memory distance. "GPC" appears
  only in the background and in the discussion of MIG's coarse GPC isolation.
- Memory bandwidth is not a scheduled resource. "bandwidth" appears only in
  the motivation (fleet utilisation) and in DVFS (memory-bound kernels are
  frequency-insensitive). Memory-bound kernels are handled only through the
  right-sizing curve (they saturate early, so they get few TPCs).
- No notion of a TPC's memory-request (TMA/LSU/MSHR) capacity as something
  that can be lent; a TPC is a unit of compute.

### A.2 libsmctrl (UNC, Bakita & Anderson)

Source: `git clone http://rtsrv.cs.unc.edu/cgit/cgit.cgi/libsmctrl.git/`
(worked; copy in `gpu/third_party/libsmctrl`, head c250928, 2025-05-05,
"De-duplicate CUDA version checks"). Paper: J. Bakita and J. H. Anderson,
"Hardware Compute Partitioning on NVIDIA GPUs", RTAS'23,
https://www.cs.unc.edu/~jbakita/rtas23.pdf . Follow-up on the GPU's
scheduling internals (channels, runlists, the nvdebug module): "Demystifying
NVIDIA GPU Internals to Enable Reliable GPU Management", RTAS'24,
https://www.cs.unc.edu/~jbakita/rtas24.pdf , artifact page
https://www.cs.unc.edu/~jbakita/rtas24-ae . Licence: none stated (no
LICENSE file; source headers say only "Copyright 2022-2025 Joshua Bakita").

How the mask is set (FACT, from `libsmctrl.c`):
- **Where.** In the TMD (Task MetaData, also called QMD), the structure the
  driver uploads per kernel launch. RTAS'23 found the fields
  `SM_DISABLE_MASK_LOWER/_UPPER` listed in NVIDIA's open headers and showed
  the "SM mask" is really a **TPC** mask (a set bit disables a TPC).
- **Global and next masks (per process / per launch).** libsmctrl gets a
  private driver table with `cuGetExportTable` (a fixed UUID), subscribes a
  callback (slot 3) and enables it for domain 0xb, cbid 0x1 (slot 6), which
  the driver calls with the TMD just before upload. The callback writes the
  mask. TMD version byte at offset 72: V01_06+ (Kepler to Ada) mask at bytes
  84/88 (64 TPCs); **V04 (Hopper) mask at 304/308/312/316 (128 TPCs) plus an
  enable bit (bit 31 of word 0)**. In V04 libsmctrl writes only the lower 64
  bits from the user and forces bits 64-127 to "disabled" ("Disable upper 64
  TPCs until we have ...next_mask_ext").
- **Stream mask.** A different mechanism: it writes into CUDA's private
  per-stream struct at a hard-coded offset per driver version (12.0: 0x4cc
  ... 12.7/12.8: 0x4fc), which CUDA then copies into each TMD. Supported only
  for listed driver versions; a `MASK_OFF` environment variable lets one
  search for the offset on a new version.
- **Precedence.** next > stream > global.
- **Privilege.** Pure user space, linked into the process (or LD_PRELOAD);
  no root, no kernel module. Only partitions **within one context**
  (README: "Only supports partitioning within a single GPU context").
- **Versions/architectures (README).** "compute capability 3.5 through 8.9",
  "CUDA 6.5 through 12.6" (stream masks to 12.8 x86). Hopper is listed as a
  known issue: "Mask bit indexes do not directly correlate to
  software-visible TPC/SM IDs in V4 TMD/QMDs (Hopper+ ...). The mask bit
  indexes instead appear to correspond to on-chip-units, including disabled
  ones; i.e. the set of pre-SM-ID-remapping and pre-floorsweeping TPCs."
  `libsmctrl_get_tpc_info_cuda` prints "TPC masking is untested on Hopper".
- **TPC -> GPC map.** `libsmctrl_get_gpc_info` reads
  `/proc/gpuN/{num_gpcs,gpc_mask,num_tpc_per_gpc,gpcK_tpc_mask}` from the
  **nvdebug kernel module** (cannot be loaded here: no root). RTAS'23 §III-B:
  floorsweeping makes the TPC->GPC map differ between dies of the same
  product, so it must be read per GPU. Without nvdebug libsmctrl has no way
  to print it.
- **Validation in its tests** (`libsmctrl_test_mask_shared.cu`): launch 142
  blocks of 1024 threads, record `%smid` per block, check that with one TPC
  enabled at most 2 distinct SMs appear and that they are SMs 2t, 2t+1. (The
  2t/2t+1 check is the pre-Hopper assumption that the README says is false
  on Hopper.)

### A.3 What we can and cannot control on this cluster

All FACT, measured in job 15954225 (raw: `results_gpu/h200_tpc_env.txt`,
`h200_tpc_probe.txt`, `h200_tpc_mps*.txt`).

| Control | Status here |
|---|---|
| Root, kernel modules (nvdebug) | no |
| Driver | 615.71.09, `cuDriverGetVersion` = 13040 (nvidia-smi "CUDA UMD 13.4"); toolkit module cuda/12.9.1 |
| Per-launch TPC mask (TMD callback) | **works**, TMD version byte 0x40, 128-bit field at 304-316 (`gpu/tpc_smctrl.h`); 194 of 195 launches masked in the probe (the unmasked one is the baseline) |
| Process-wide mask of an unmodified binary | **works** via LD_PRELOAD (`gpu/tpc_preload.so`, env `TPC_ENABLE_BITS`) — also inside an MPS client (1 TPC: 221 GB/s vs 603 unmasked, `h200_tpc_mps_mask.txt`) |
| libsmctrl stream masks | **no**: "Stream masking unsupported on this CUDA version (13040), and no fallback MASK_OFF set!" (not searched with MASK_OFF) |
| libsmctrl tests as shipped | next/global mask tests fail: "constrain all kernels to TPC 32 ... SM IDs as high as 83 and as low as 82 (range of 65 to 64 expected)" — the mask works, the test's bit->SM assumption is wrong on Hopper |
| TPC->GPC map from the driver | no (needs nvdebug); recovered from cluster launches (Part B) |
| SM-granular placement | yes, by a mask (TPC granularity) plus an SM filter in the kernel (`%smid`; CTAs on other SMs exit) |
| Cross-process concurrency | MPS daemon starts as a user (`nvidia-cuda-mps-control -d`); masks apply per client; **under an MPS active-thread percentage, `%smid` reads 0..n-1** (not physical; see C.1) |
| Clusters with masks | work; a cluster that cannot be placed inside the mask is never scheduled (hangs; the watchdog kills it after 10-15 s, the GPU stays usable) |
| L2 partition / address -> slice | not controllable (no address-to-slice map) |

## Part B — TPC masks on the H200

### B.1 Mechanism and what I changed

`gpu/tpc_smctrl.h` is libsmctrl's callback mechanism (same export-table
UUID, subscribe slot 3, enable slot 6, domain 0xb / cbid 0x1, TMD pointer at
in_params[4], version byte at +72). Changed: all four 32-bit words of the V04
mask are written from a 128-bit value (libsmctrl forces bits 64-127 off),
because on this die live TPCs sit at bits up to 83. The original library is
built and kept unchanged in `gpu/third_party/libsmctrl`.

### B.2 Validation (FACT, `h200_tpc_probe.txt`)

- For each bit b in 0..127 a 1024-thread, max-shared-memory (one CTA per SM)
  kernel of 396 CTAs ran with only bit b disabled; the missing SMs are the
  SMs of TPC b. Then a run with only bit b enabled: every CTA must land on
  those SMs.
- 66 live bits, 132 SMs covered, 0 mismatches.
- Live bits: 0-3, 9-16, 18-25, 27-34, 36-43, 45-52, 54-62, 63-71, 75, 78,
  81, 83.
- Bandwidth scales with the mask. Single-SM rates are 204-207 GB/s (TMA) and
  239 GB/s (loads) on every GPC. Pairs of the same TPC give exactly 2x.
- At 132 SMs the masked runs match unmasked bwsweep: 4334 vs 4338 GB/s
  (4-byte loads).

### B.3 Topology (`h200_tpc_gpc.txt`)

Method:
- (i) Masked cluster launches (1 CTA per SM). A cluster of 16 restricted to
  bits {9-16}, {18-25}, {27-34}, {36-43}, {45-52}, {54-61}, {55-62},
  {63-70}, {64-71} ran with all 16 CTAs on the 16 SMs. {0-3} held a cluster
  of 8.
- Negative controls never got placed (15 s timeout):
  - clusters of 4 on {9,18} and {0,9};
  - {0, x} for x in 75, 78, 81, 83;
  - pairs among 75/78/81/83;
  - {x, 9/54/63}.
- Each of 75/78/81/83 hosts a cluster of 2 (its own TPC). Raw:
  `h200_tpc_gpcmap*.txt`.
- (ii) Bandwidth cap (C.1): {0-3} alone 1279 GB/s, the four lone TPCs alone
  1279, together 1283 (`h200_tpc_network.txt`, "gpc0+singles"). So they share
  one GPC limit.

Map (mask bit -> SMs; GPC = physical group):

| GPC | mask bits | TPCs | SMs |
|---|---|---|---|
| 0 | 0-3 + 75, 78, 81, 83 | 4+4 | 0,1,16,17,32,33,48,49 + 124-131 |
| 1 | 9-16 | 8 | 2,3,18,19,34,35,50,51,64,65,78,79,92,93,106,107 |
| 2 | 18-25 | 8 | 4,5,20,21,36,37,52,53,66,67,80,81,94,95,108,109 |
| 3 | 27-34 | 8 | 6,7,22,23,38,39,54,55,68,69,82,83,96,97,110,111 |
| 4 | 36-43 | 8 | 8,9,24,25,40,41,56,57,70,71,84,85,98,99,112,113 |
| 5 | 45-52 | 8 | 10,11,26,27,42,43,58,59,72,73,86,87,100,101,114,115 |
| 6 | 54-62 | 9 | 12,13,28,29,44,45,60,61,74,75,88,89,102,103,116,117,120,121 |
| 7 | 63-71 | 9 | 14,15,30,31,46,47,62,63,76,77,90,91,104,105,118,119,122,123 |

Confidence:
- High for GPCs 1-7: positive clusters plus negative controls, and the
  bandwidth cap agrees.
- High that bits 0-3 and 75/78/81/83 share one bandwidth limit.
- Why those four TPCs cannot join larger clusters is unknown. INFERENCE:
  each sits alone in a cluster-scheduling unit (a CPC?) whose siblings are
  floorswept.
- Mask bit = 9 x GPC + slot fits bits 0-71. Bits 75-83 do not fit that rule
  (UNVERIFIED meaning).
- Cluster occupancy (7 x 16, 15 x 8, 30 x 4) matches: GPC 0 cannot hold a
  16-CTA cluster.
- No CPC structure visible in bandwidth: any 3-TPC subset of GPC6 gives
  1180-1199 GB/s from DRAM (`h200_tpc_gpcscan.txt`).

### B.4 What failed, with the exact evidence

- libsmctrl tests and stream masks: see A.3.
- **Managed memory** used by my first kernels: a kernel touching
  `cudaMallocManaged` memory while the other kernel ran slowed down by up to
  60x. TMA stream at 5.4 GB/s instead of 319, window 397 ms. Replaced by
  cudaMalloc plus explicit copies; partly fixed.
- **Polling host-mapped memory**: 130 FMA CTAs each reading a
  `cudaHostAllocMapped` stop flag every 33 µs cut a TMA stream on two
  *other* SMs to 3-6 GB/s (319 without). The same CMP kernel's own rate fell
  to 18 TFLOP/s from 53-55.
  - With 260 µs between polls, or a fixed chunk count, both were normal.
  - All later runs poll every ~260 µs. "cmp_fixed" runs confirm no effect.
  - The cause of the cross-SM effect is UNVERIFIED.
- **L1 carveout**: forcing the maximum shared carveout on a plain-load kernel
  caps it at 62-67 GB/s per SM. Default carveout: 239 (16-byte loads, 2 x
  1024 threads) or 110 (4-byte loads). Plain-load MEM therefore keeps the
  default carveout; co-resident ones set the FMA kernel's carveout to default.
- **mbarrier poll loop**: TMA write-only from one thread (32 KB x 6) gave
  205.8 GB/s in the build used for the network sweep (03:53). In later builds
  (after adding a nanosleep variant to the same wait function) the
  tight-loop try_wait gave 139-141. With `__nanosleep(20-100 ns)` between
  `test_wait` polls it gave 201-203.
  - The chip and borrowing runs use the 100 ns backoff ("WB").
  - The GPC1 co-residency runs are at the GPC limit (80 GB/s per SM), so
    unaffected.
  - Why code generation matters this much is UNVERIFIED.
- **Plain-load kernel per_sm=2 accounting under MPS / SM filter**: "check=FAIL"
  in `h200_tpc_mps.txt` and `h200_tpc_gemm.txt` counts unclaimed slices or
  consumers (e.g. 992 = 248 x 4 reps). No claimed unit had a wrong checksum.
  In the GEMM run 2 of 32 MEM SMs (mask bit 75, SMs 124-125) received no CTAs
  under MPS, so that MEM is 30 SMs (UNVERIFIED why).

## Part C — placement-dependent strategies

### Methods (every bandwidth number)

- Buffer 24 GiB of uint32 a[i] = i (16 GiB in the MPS GEMM run); L2 is 50 MB
  class. Each consumer or slice streams its own range once: 1024 MB per unit,
  or buffer/units if smaller (186-768 MB at 32-132 units).
  "L2-resident" = 24 MB window re-read, after a warm-up pass.
- Timing: `%globaltimer` per unit (start after setup, end after the last
  transfer is consumed). Aggregate = bytes / (latest end - earliest start).
  - 1 warm-up launch + 3 timed reps (50 in the GEMM run). The median is
    reported; ranges are in the raw files (`mem_gbs_range`), typically ±0.5%.
- Check: every tile's first/last word (TMA write-only), or the full sum of
  every word (staged, plain loads), against the closed form. All runs quoted
  have `check=ok` except the MPS accounting noted in B.4.
- The SMs used are printed per run (`mem_cta_sms`, `ld_sms`, `cta_sms`),
  along with the TPC mask (`mem_tpc_mask`).
- MEM kernels (`gpu/tpc_lab.cu`):
  - **W**: one thread, cp.async.bulk 32 KB tiles, 6 in flight, checks and
    re-issues (write-only upper bound).
  - **S**: 3 issuing warps x 4 x 16 KB, 12 consumer warps that read every
    byte (staged-then-consumed).
  - **L16**: plain uint4 loads, 2 x 1024 threads per SM, 4 per thread in
    flight.
  - **L4**: plain 4-byte coalesced loads (the bwsweep / MPS-sweep pattern).
- CMP: FP32 FMA loop (corun.cu muladd), 1 CTA of 256-1024 threads per SM,
  4 independent chains (ILP 4) unless stated. Per-chunk timestamps give its
  rate inside the MEM window; a CMP-alone pass of the same length on the same
  SMs gives the solo rate. Peak measured: 425 GFLOP/s per SM.

### C.1 Placement and the network (MEM alone, `h200_tpc_network.txt`, `h200_tpc_gpcscan.txt`)

Total GB/s. "pair" = both SMs of n/2 TPCs; "single" = one SM per TPC;
"packed" fills GPC6, GPC7, GPC1.. in turn; "spread" round-robins over 12
cluster groups, the 4 lone TPCs counted as groups.

**W (TMA write-only, 205 per SM alone)**

| n SMs | pair packed | pair spread | single packed | single spread |
|---|---|---|---|---|
| 2 | 408.9 | 410.2 | 411.4 | 407.8 |
| 4 | 815.0 | 815.9 | 812.8 | 819.2 |
| 8 | **1277.1** | 1611.1 | **1278.5** | 1621.7 |
| 16 | **1283.0** | 3084.1 | 2272.4 | 3090.1 |
| 21 | 1497.8 | 3639.7 | 2973.3 | 3747.3 |
| 32 | 2283.4 | 4069.4 | 3552.6 | 4557.0 |
| 48 | 2810.7 | 4110.7 | 3713.4 | 4604.8 |
| 64 | 2766.4 | 3934.5 | 4523.6 | 4631.8 |
| 96 | 3512.0 | 4338.5 | - | - |
| 132 | 4631.0 | 4626.2 | - | - |

**L16 (plain 16-byte loads, 239 per SM alone)**

| n SMs | pair packed | pair spread | single packed | single spread |
|---|---|---|---|---|
| 8 | 1274.1 | 1893.1 | 1275.2 | 1885.2 |
| 16 | 1282.5 | 3537.3 | 2267.0 | 3514.5 |
| 21 | 1497.3 | 3772.4 | 2968.0 | 3955.0 |
| 32 | 2282.6 | 4044.3 | 3559.7 | 4427.9 |
| 132 | 4548.2 | 4552.8 | - | - |

**S (staged)** at 8 / 16 / 32 SMs:
- pair packed: 1203 / 1283 / 2283
- single spread: 1264 / 2413 / 4085
- Per SM alone: 159.6.

**L4 (4-byte loads)** at 8 / 16 / 21 / 32 SMs:
- packed: 848 / 1245 / 1475 / 2241
- spread: 860 / 1639 / 2049-2062 / 2796-2810
- Per SM alone: 110.

Findings (FACT unless marked):
- **GPC limit 1277-1285 GB/s from DRAM.** It is reached at 4 TPCs and is
  the same for TMA and plain loads. GPC1 and GPC6 are identical. L2-resident
  data gets 1282-1548 at 8-18 SMs, and 1619 in one run with 18 SMs. That is
  about 650 B/clock at 1.98 GHz.
  - Jin et al. (MICRO'24; I re-read the PDF, section on input speedup): "the H100 approaches
    85%, with a speedup close to 8" of the 9 needed. That is an L2-bandwidth
    "input speedup" measured with their per-SM traffic, not a DRAM number.
  - Here the GPC limit is 6.2x one SM's TMA rate and 5.4x its plain-load
    rate. In "SMs' sum" terms, a GPC delivers 35-40% of what its 16-18 SMs
    could demand at their single-SM ceilings.
- **No TPC limit**: both SMs of a TPC give 2 x 205 (W) and 2 x 239 (L16).
- **No CPC level**: any three TPCs of GPC6 give 1180-1199.
- **Where memory becomes the limit**: about 4.55-4.63 TB/s with 32+ SMs
  spread (4.8 nominal; 96-97%). The per-SM rate falls once the total passes
  about 3 TB/s (16 spread SMs: 190 each) even below any GPC limit. INFERENCE:
  loaded latency.
- "pair spread" falls behind "single spread" from 24 SMs (e.g. 4069 vs
  4557 at 32). This is an artefact of my spread order: it treats the 4 lone
  TPCs as separate groups, so physical GPC0 gets 10 SMs and hits its limit.
  Spread by physical GPC.
- **The MPS sweep (107 -> 77 per SM between 8 and 22 SMs)**, reproduced
  under MPS in this job (`h200_tpc_mps.txt`).
  - L4: 855 (8), 1266 (14), 1697 (22), 2434 (32), 3266 (46), 3899 (66).
  - L16: 1272 (8), 1492 (14), 1763 (22), 2563 (32), 3163 (46), 3893 (66).
  - L16 at 8 SMs equals one GPC's limit (spread: 1885); at 32 it is two
    limits (spread: 4428).
  - INFERENCE: MPS packs a client into few GPCs, and the drop is the GPC
    limit. L4's own per-SM limit (110) hides it at small n.
  - Under a percentage `%smid` reads 0..n-1. If those were physical SMs 0-7
    (four GPCs) they would give about 1890, not 1272, so the ids are not
    physical.
  - With a TPC mask inside an MPS client, bits 0-3 (one GPC, 8 SMs) gave 852
    with L4, which equals the MPS 6% result.
- **"Which 21 SMs?"** 2-3 SMs in each of the 8 physical GPCs. Measured:
  21 spread 3747 (W), 3955 (L16); 24 spread 4030-4050 (W); 21 packed 1498.

### C.2 Co-residency (MEM and CMP on the same SMs, GPC1 = 16 SMs, `h200_tpc_coresid*.txt`)

MEM alone on GPC1 is at the GPC limit: 1284-1285 GB/s, 80.4 per SM, for
every shape.

**CMP launched first, ILP 4 (corun.cu-like)**: MEM GB/s on 16 SMs / CMP ratio.

| FMA warps/SM | W | W test_wait | W 4 issuers | S12 | S4 | L16 |
|---|---|---|---|---|---|---|
| 8 | 60.9 / 0.999 | 41.2 | 57.8 | 43.1 | 26.4 | 52.3 / 0.997 |
| 16 | 37.8 | 25.2 | 39.8 | 41.2 | 23.8 | 85.7 / 0.985 |
| 24 | 24.5 | 17.9 | 30.9 | 30.2 | 19.0 | 39.2 |
| 32 | 16.4 / 1.000 | 11.2 | 15.8 | 12.9 | 7.5 | 18.1 / 0.999 |

- MEM keeps 0.6-6.7% of its solo rate; CMP keeps 0.985-1.000.
- Fewer compute warps does not rescue it. With a fixed-length CMP (no
  polling) MEM shows 1.1 GB/s per SM over a 470 ms window = the CMP
  duration: MEM made almost no progress until CMP finished. It is starved,
  not slowed.

**Launch order and compute ILP (32 FMA warps, W)**: MEM GB/s / CMP ratio.

> CORRECTION 2026-11-21: do not quote the table below. Checked against
> `results_gpu/h200_tpc_coresid*.txt`: no `ilp=1` run exists in any raw file,
> and the 32-warp rows of `h200_tpc_coresid2.txt` read CMP first, ILP 2 =
> 25.0 / 1.003 (table: 38.9) and MEM first, ILP 4 = 12.1 / 1.000 (table: 20.5).
> Where the table's other cells came from is not known. Raw values, W kernel,
> by FMA warps 8 / 16 / 24 / 32: MEM first ILP 4 = 40.8 / 60.0 / 42.6 / 12.1
> GB/s; CMP first ILP 2 = 1283.0 (cmp 0.920) / 22.7 / 35.1 / 25.0. So with 8
> FMA warps at ILP 2 the memory job keeps its full rate even when launched
> last. To be re-measured with 1 s windows and >= 5 trials.

| | ILP 4 | ILP 2 | ILP 1 |
|---|---|---|---|
| CMP first | 17.5 / 0.999 | 38.9 / 1.003 | 83.9 / 0.998 |
| MEM first | 20.5 / 0.999 | **1284.4 / 0.894** | **1284.5 / 0.868** |

- All FMA variants reach the same solo rate (6.6-6.8 TFLOP/s on 16 SMs),
  so ILP 2 is not a weaker compute kernel.
- INFERENCE: the scheduler is greedy-then-oldest. A warp that can issue every
  cycle (ILP 4) never yields; an older ready warp wins otherwise. So a
  memory warp is served only if it is older and the compute warps stall
  sometimes.

**MEM first, ILP 2, vs FMA warps** (MEM GB/s / CMP ratio):

| FMA warps | W | W + 500 ns backoff | S12 | L16 |
|---|---|---|---|---|
| 8 | 1283 / 0.884 | 1284 / 0.894 | 1285 / 0.774 | 1280 / 0.121 (outlier, unexplained) |
| 16 | 1283 / 0.878 | 1284 / 0.894 | 1284 / 0.754 | 1279 / 0.870 |
| 24 | 1283 / 0.891 | 1283 / 0.909 | 1284 / 0.764 | 1279 / 0.882 |
| 32 | 1284 / 0.890 | 1284 / 0.909 | 1285 / 0.765 | 1280 / 0.884 |

- With CMP first instead, ILP 2: 1283 / 0.920 at 8 warps, but 22.7-35.1 GB/s
  at 16-32 warps.
- With 2 SMs (no GPC limit) and MEM first: W 138 GB/s per SM (0.67 of solo),
  CMP 0.852.
- The ~10% compute cost of a co-resident one-thread TMA stream is
  UNVERIFIED in cause: not idle warps (1 or 4 warps same), and only partly
  polling (backoff: 0.893 -> 0.909).

**Disjoint split inside GPC1** (x MEM SMs, 16-x FMA SMs, ILP 4):

| x | W GB/s | L16 GB/s | CMP rate (of 16-SM 6799) | CMP ratio vs its solo |
|---|---|---|---|---|
| 2 | 401 | 473 | 5948 (0.875) | 1.000 |
| 4 | 812 | 949 | 5099 (0.75) | 1.000 |
| 8 | 1281 | 1280 | 3396 (0.50) | 0.999 |
| 12 | 1285 | 1284 | 1697 (0.25) | 0.998 |

**Whole chip, shared vs disjoint** (`h200_tpc_chip.txt`, two passes, both
listed; CMP all-132 solo = 56.0 TFLOP/s at ILP 4, 54.2 at ILP 2):

| Configuration | MEM GB/s | CMP TFLOP/s | CMP of all-132 |
|---|---|---|---|
| shared: W on all 132 + FMA on all 132, MEM first, ILP 2 | 4331-4334 | 46.7-46.8 | 0.86 |
| shared, same but ILP 4 | 142-150 | 56.0 | 1.00 |
| disjoint W 8 spread + FMA 124 | 1584-1589 | 52.7 | 0.94 |
| disjoint W 16 spread + FMA 116 | 3004-3038 | 49.3 | 0.88 |
| disjoint W 24 spread + FMA 108 | 4030-4050 | 45.8 | 0.82 |
| disjoint W 32 spread + FMA 100 | 4498-4508 | 42.4 | 0.76 |
| disjoint W 48 spread + FMA 84 | 4512-4550 | 35.6 | 0.64 |
| disjoint W 16 pair packed + FMA 116 | 1278-1283 | 49.2 | 0.88 |
| disjoint W 32 pair packed + FMA 100 | 2279-2283 | 42.4 | 0.76 |
| disjoint L16 16 / 24 / 32 spread | 3468-3509 / 4174-4183 / 4306-4372 | 0.88 / 0.82 / 0.75 | |

- In every disjoint row CMP keeps 0.986-1.002 of its solo rate on its own
  SMs. The GPU's SM clock read 1980 MHz with no throttle reasons active.
- INFERENCE: shared SMs beat disjoint only if the co-resident memory job is
  guaranteed issue priority: 4.33 TB/s at 0.86 compute, vs 4.03 at 0.82 or
  4.50 at 0.76 disjoint.
  - Today that priority depends on launch order and on the compute kernel's
    instruction mix. A production GEMM leaves no residency room at all.
  - Disjoint and spread is the robust answer; the "memory lane" (issue
    priority plus a reserved slot) is what a hardware proposal must deliver
    to beat it.

**Tensor-core GEMM as CMP** (`h200_tpc_gemm.txt`):
- Setup: CUTLASS example 54, FP8 2048x2048x1024, in one MPS client, masked
  to 50 TPCs (100 SMs) with LD_PRELOAD. MEM (W) in a second client on 16
  TPCs = 2 per physical GPC; 30 SMs got CTAs, see B.4.
- GEMM: 444.6 TFLOP/s on 132 SMs; 250.5 masked to 100 SMs (persistent grid
  sized for 132, see §0).
- With MEM streaming: 226.0 (0.90).
- MEM alone 4489 GB/s; with the GEMM running 3664 (W) / 3730 (L16), i.e.
  0.82. INFERENCE: the GEMM's own DRAM/L2 traffic.

### C.3 Borrowing, short confirmation (`h200_tpc_borrow2.txt`, two identical passes)

Consumer SM 12 (GPC6), helpers write into its shared memory with
`cp.async.bulk.shared::cluster`, 16 KB tiles, 100 ns poll backoff, no CMP.
- **1 helper, consumer does not issue**: 75.3 GB/s for the sibling SM 13
  (same TPC) and for a helper in each of the 8 other TPCs (SMs 28, 44, 60,
  74, 88, 102, 116, 120). Write-only and staged give the same: distance is
  irrelevant inside a GPC.
- 3 helpers: 177.9 (self 0) / 176.0 (self 1). 1 helper + self: 144.3.
  7 helpers + self, depth 1: 133.4.
  - The 46.5 rows are an artefact: a ring of 13 or 7 slots forces 1 consumer
    warp.
  - Local, no helpers: 201-205 (32 KB x 6, one thread).
- 3 helpers running FMA as well: CMP first, ILP 4 -> 1.0 GB/s (helpers
  starved); MEM first, ILP 2 -> 176.3 GB/s with CMP 0.913.
- The borrow agent's E2 result holds with explicit placement: the ingest
  ceiling, not the issuing SM, limits; helpers add nothing over a local
  32 KB x 6 issuer.
- S2-S5 as originally specified were dropped at the coordinator's request.

### C.4 Unverified / open

1. Why the 4 TPCs at bits 75/78/81/83 cannot join clusters larger than 2,
   and what bits 72-83 index.
2. Whether the GPC limit is the GPC's return port, crossbar ports or
   something else. It is 1.28 TB/s for DRAM, only a little higher from L2.
3. Why polling host-mapped memory from many SMs stalls TMA on other SMs.
4. Why the tight mbarrier poll loop's speed depends on the build (205 vs 140).
5. The cause of the ~10% compute cost of a co-resident one-thread stream.
6. That MPS packs GPCs: inferred from bandwidth, not observed (`%smid` is
   virtual under MPS). Green contexts' SM choice was not tested.
7. Near/far L2 partition: not controllable. DRAM streams hash over both
   partitions; per-SM single rates are 204-207 on every GPC.
8. No CUTLASS co-residency test (no room by the borrow agent's register
   count; not re-measured).

### C.5 Files

Sources/scripts (`/storage/project/r-hkim358-0/echung67/vortex-exp/gpu/`):
- `tpc_smctrl.h` (mask mechanism), `tpc_lab.cu` (probe / cluster / run:
  MEM W, S, L16, L4, role-based cluster TMA, FMA CMP), `tpc_preload.cpp`
  (LD_PRELOAD mask), `tpc_bwsweep` (bwsweep.cu built unchanged).
- `tpc_partb.sh`, `tpc_gpcmap.sh`, `tpc_gpcmap2.sh`, `tpc_gpcscan.sh`,
  `tpc_mps.sh`, `tpc_mps_mask.sh`, `tpc_gemm.sh`.
- `tpc_partc.py` (experiments: network, coresid, coresid2, chip, borrow2),
  `tpc_summarize.py`, `tpc_srun.sh`, `tpc_holder.sbatch`.
- `third_party/libsmctrl` (UNC source, c250928, built, unmodified).

Raw outputs (`results_gpu/`):
- `h200_tpc_holder.out`, `h200_tpc_env.txt`, `h200_tpc_probe.txt`,
  `h200_tpc_map.txt`, `h200_tpc_gpc.txt`, `h200_tpc_gpcmap.txt`,
  `h200_tpc_gpcmap2.txt`, `h200_tpc_gpcscan.txt`.
- `h200_tpc_network.txt`, `h200_tpc_mps.txt`, `h200_tpc_mps_mask.txt`,
  `h200_tpc_coresid.txt`, `h200_tpc_coresid2.txt`, `h200_tpc_chip.txt`,
  `h200_tpc_borrow2.txt`, `h200_tpc_gemm.txt`.
