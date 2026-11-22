# Related work: GPC/partition placement, per-SM bandwidth, SM sharing (2024-2026)

Compiled 2026-11-21. Read-only survey against our H200 measurements
(`notes/tpc-placement-and-borrowing.md`, `notes/open-threads.md`).
Tags: **documented** (vendor states it), **measured** (paper measured on hardware),
**inferred** (paper reasons from measurements; or my reading, marked "mine"),
**figure-read** (value read off a plot by eye, ±10%), **unsourced** (paper asserts it
without measurement or citation).

Our observations used for comparison (H200, from our notes):
- O1 one SM's ingest ceiling ~205-219 GB/s from DRAM (TMA 32 KB, 16 B loads; 239 GB/s
  plain 16 B in one run); staged-then-read ~157-170 GB/s.
- O2 a GPC returns at most ~1.28 TB/s of DRAM data, reached at ~6 SMs; L2-resident
  1.29-1.62 TB/s. No TPC-pair limit, no CPC level.
- O3 16 memory-bound SMs packed 1.28 TB/s vs spread over 8 GPCs 3.1-3.5 TB/s (2.4x);
  chip roof 4.63 TB/s.
- O4 MPS appears to pack a client's SMs into few GPCs.
- O5 software borrowing (thread-block clusters / helper CTAs in the same GPC) adds no
  bandwidth.
- O6 a memory-bound stream co-resident with a compute kernel on the same SMs keeps
  0.6-22% of its rate (greedy-then-oldest-like issue).
- O7 a cuBLAS FP16 GEMM uses 98% of an SM's registers (64,512/65,536).
- O8 `bar.sync` does not wait on other CTAs' loads.
- Topology: SMs 124-131 never join clusters > 2; SM ids interleave across GPCs
  ({2g+16t,+1}); our die has GPCs of 16,16,16,16,16,18,18 SMs plus one 8+8.

---

## 1. Fan, Wang, Cui, Zhao, Huang, Zhou, Qiu (NVIDIA), Sun, He, Chen, Guo, "Dissecting How Die Scaling Breaks GPU Fine-grained Scheduling", arXiv 2609.24270v1 (21 Sep 2026), 16 pp.

### (a) GPUs and method
- **GPUs:** H200 and B200 primary ("We use H200 and B200 as our primary testbeds",
  §4); 28 H200s for topology statistics (§5.2.1); H100 PCIe (App. A); V100, H100,
  RTX 5090 for L2 capacity (Fig. 7); MI300X (App. G).
- **SM→GPC mapping (§5.1):** Phase 1 "Cluster probing": launch with cluster sizes 3-8 via
  `cudaLaunchKernelEx`, log `%smid`; "SM IDs that co-occur in a cluster belong to the
  same GPC." Phase 2 "L2 bandwidth probing" for the rest: "we exploit the observation
  that SMs sharing a GPC contend on the GPC's L2 Xbar bandwidth [25]" (ref [25] = Jin et
  al. MICRO'24). "If adding s causes a bandwidth improvement smaller than a single TPC's
  bandwidth, s is contending with g's SMs". Validated "against nvdebug ... 100%
  agreement across all tested cards"; "runs in under 1 minute per GPU".
- **Pinning:** they do **not** use TPC masks for their experiments; they use persistent
  thread blocks ("one block pinned to each SM", §7.1.2), Green Contexts + MPS (§7.2,
  §7.3), MIG half-instances with a modified driver (§7.2). libsmctrl is only cited (§3).
- **SM→partition:** single-thread latency probe from every SM over ~100k addresses
  (`discard.global.L2` + `ld.cg`, `clock64()`, App. C.1).

### (b) Observations vs ours
- **Per-GPC L2 bandwidth knee at 6 SMs on H200 (matches O2's knee).** §5.1.2: "the L2
  bandwidth grows linearly with the number of SMs when the SMs are located in different
  GPCs. However, this trend does not hold for SMs within the same GPC. When the number of
  SMs exceeds 6 on H200 and 16 on B200, their L2 bandwidth becomes lower than that of SMs
  in different GPCs." Fig. 2(a), figure-read: different-GPC ≈ 0.25 TB/s per SM
  (≈ 4.5 TB/s at 18 SMs); same-GPC ≈ 1.5 TB/s at 6, ≈ 1.65 at 8, ≈ 2.0 at 12,
  ≈ 2.55 at 16, **≈ 2.8 TB/s at 18 SMs**. Test: "a kernel that repeatedly reads a buffer
  sized to exceed L1 but fit within L2, forcing all accesses to hit the L2 cache."
  - **Contradiction with O2 (L2-resident 1.29-1.62 TB/s, flat after ~6 SMs):** their
    same-GPC curve keeps rising past the knee to ~2.8 TB/s. Possible causes (mine,
    unverified): every SM reading the same small buffer lets the per-GPC L2 request
    coalescer merge requests (Accel-Sim 2.0 paper measured "up to four read requests
    into a single transaction ... exclusively when those requests originate from SMs
    within the same GPC"), which would inflate apparent same-GPC bandwidth; or their GPC
    is an 18-SM GPC vs our measured GPC; or a different load width. Their per-SM rate
    (~250 GB/s, L2-hit) is above our DRAM-sourced 205-219 GB/s (O1) but close to our
    239 GB/s plain-load run.
  - They report no DRAM-sourced per-GPC limit and no per-SM ceiling as such; Fig. 2 is
    used only as a topology probe.
- **Random SMs / clusters (matches our topology).** §5.1.1: "on H200 ... SMs with IDs
  0–123 (62 TPCs across 8 GPCs) ... reliably appear in cluster launches, while SM IDs
  124–131 (4 TPCs) never appear at cluster sizes >2." Takeaway 3: "Random SMs cannot be
  used for Thread Block Cluster with cluster size larger than 2." Our SMs 124-131
  observation is the same.
- **GPC sizes (matches our die).** §5.2.1: "Of the 28 H200 GPUs measured, 23 each have
  six 16-SM GPCs and two 18-SM GPCs." Others have a 12-, 14- or 10-SM GPC. Ours is the
  majority layout.
- **SM-id interleave (matches).** Fig. 3(a): GPC 0 holds SMs 0,16,32,48,...; GPC 1
  holds 1,17,33,... (figure).
- **Green Context placement (related to O4, not the same).** §5.3.1: "By default, the
  driver allocates SMs in GPC-aligned chunks of 8, tiled across all GPCs so that no
  allocation concentrates within a few GPCs." App. B: "MAX_POTENTIAL_CLUSTER_SIZE mode
  ... allocating GPC-aligned chunks of 8 but concentrating them within a few GPCs."
  Takeaway 4: Green Contexts give normal SMs to earlier contexts, random SMs to the last.
  **MPS SM placement is not measured.**
- **NUMA.** HBM local/remote ∼490/∼655 cycles on H200 (Fig. 4); L2 ∼309/∼466 (Fig. 5).
  Partition granularity 4 KB: "On H200 and B200, 16 bits participate in the hash, with
  bit 12 as the lowest" (§6.1.2). Takeaway 8: "Remote HBM bandwidth is bottlenecked by the
  LTC fabric that connects the two L2 cache partitions, not by the HBM itself." Fig. 8
  (figure-read): H200 local ≈ 2.4 TB/s, remote ≈ 1.3 TB/s at large working sets
  (annotation "Δ1.1 TB/s" applies to B200; the H200 gap reads ≈ 1.1 TB/s too). The SM set
  used for Fig. 8 is not stated. Effective L2 = 65.8% of physical on H200 (Takeaway 9,
  replication into the local partition).
- **Implication for O3 (mine):** with 4 KB partition interleave, any streaming job reads
  ~half its data from the remote partition, so a per-partition fabric limit (~1.3 TB/s
  remote on H200, figure-read) is a second chip-level cap besides the GPC cap.

### (c) Proposals and gains
- Characterization tools (SM→GPC, SM→partition, partition hash).
- **Full-GPU kernels:** NUMA-aware data placement ("Indirect allocation through
  remapping" within 2 MB pages; "Direct allocation through driver modification" on
  Blackwell via MLOPart code) + PTB assignment of work to SMs local to the data. "up to
  1.22×" (GroupGEMM on B200); some slowdowns from PTB/remap overhead (Fig. 10, 11).
  Workloads: FA3 (H200) / FA4 (B200) MHA/GQA/MLA prefill+decode, CUTLASS GroupGEMM.
- **Intra-app multiplexing:** prefill/decode on two MIG half-instances with cross-MIG
  shared memory mapping (driver change) in mini-SGLang, Qwen3-8B: decode +14.3% (H200),
  +10.4% (B200).
- **Inter-app co-location:** allocation order matters for cluster-dependent GEMMs: up to
  1.18× (H200) / 1.33× (B200) "variation".
- Recommendations (§7.4): tenants should declare SM-topology needs (cluster size);
  vendors should allow "flexible physical SM selection and per-allocation NUMA placement."

### (d) What it leaves open
- No placement policy based on **per-GPC bandwidth**: GPC contention is used only to
  map SMs. Packed-vs-spread bandwidth for a memory-bound job is not studied.
- No per-SM ingest limit, no SM co-residency, no hardware proposal. §8.2: simulator
  extensions "orthogonal ... We leave these extensions to future work."

### (e) Code
"We will open-source our code to enable reproduction of all our results." (§1). Not yet
released as far as the paper says.

---

## 2. Wang, Luo, Li, Liu*, Wang, Chen* (HKUST-GZ, Tsinghua), "A Runtime System for Mitigating Crossbar Port Contention on SM-Partitioned GPUs" (GPCPlace), arXiv 2611.24458v1 (18 Nov 2026), 7 pp. *(found by search; the closest prior art to O2-O4)*

### (a) GPUs and method
- 14 GPUs: A800, RTX 3060/3090, A6000, A40, RTX 4080/4090, L4, RTX 5000 Ada, **H20**,
  RTX 5070/5090, RTX PRO 6000, DGX Spark (Table I). **No H100/H200/B200.**
- Pinning: **per-launch TPC mask in the QMD** via a driver launch callback, "Following
  prior work [1]" (libsmctrl). TPC-bit→GPC map: "we test each bit in the TPC mask by
  disabling only that bit, launching a small kernel that reads the SM and GPC IDs from
  hardware special registers" (§III-C). Same mechanism as ours.

### (b) Observations vs ours
- Model (§II-A, **unsourced**): "An on-chip crossbar connects the GPCs to the L2 slices,
  with one crossbar port for each GPC ... Because each GPC port sends at most one request
  per cycle ... each GPC port maintains a separate queue for each L2 slice"; "an arbiter
  selects requests equally across their queues". Note Jin et al. measured that part of
  the GPC speedup is "provided in space" (V100), which is not a single-port picture.
- **Packing limits bandwidth (matches O3 qualitatively, on RTX 4090).** §II-C: decoding
  alone, "packing 1 to 6 TPCs into a single GPC ... reaching only 4.4× over 1 TPC at 6
  TPCs compared with 5.6× when spread"; next to a background job, "packed into 1
  dedicated GPC, it receives only 135 GB/s ... distributing its 6 TPCs across 2 to 6
  dedicated GPCs increases its bandwidth to 156–160 GB/s." No per-GPC cap in TB/s, no
  H200 numbers.
- **Shared-GPC queueing (new vs ours):** a latency-critical job sharing GPCs with a
  memory-heavy background job (disjoint TPCs) slows from 65.9 to 159.3 ms per step
  (2.42×) as background TPCs per GPC go 0→4; probe latency 193→386 ns (RTX 4090, Fig. 2).
- **Green Contexts spread** across GPCs on all 14 GPUs (Observation 1). MPS not studied.
- On HBM parts the effect is small: "packing provides little speedup on H20 and A800
  (0.98×–1.13×) due to their high HBM bandwidth"; GPCPlace gains "1.00×–1.01× on the
  high-bandwidth HBM GPUs (A800 and H20)".

### (c) Proposal and gains
GPCPlace profiles I = t_shared/t_separate, X, L = t_packed/t_spread, D, picks pack vs
spread for the latency-critical job, sets TPC masks at launch. "up to 1.15× on average
and 2.38× maximum over Green Contexts without degrading throughput" (STP median 1.00×).
Workloads: Llama-3-8B decode layers, FlashAttention decode, embedding+FFN, against 12
background jobs.

### (d) Open
No per-SM ingest limit, no co-residency on one SM, no hardware proposal, no datacenter
Hopper beyond H20, no DRAM-sourced per-GPC ceiling number.

### (e) Code
No release statement found.

---

## 3. Joo, Puthoor, Jayasena (AMD), Asgari, "Characterizing LLM Kernel Access and Memory Interaction in Multi-Partition NUMA GPUs", arXiv 2607.28824 (30 Jul 2026), 12 pp.

- **(a)** AMD MI300X (ROCm 7.1.0) for traces (Omniprobe instrumentation); performance
  from MGPUSim extended with an NVGIM-style multi-partition model and MCM-GPU local/
  remote latencies; 4 partitions, 4 KB page round-robin, WG i on partition i mod 4 (§5.1).
  No NVIDIA GPU, no SM pinning.
- **(b)** Partition-level only. Ideal (zero-cost inter-partition) vs default: weight
  projection "within 1.64–1.77×" (Fig. 5). Nothing on per-CU bandwidth or XCD uplinks.
- **(c)** Taxonomy of operand sharing (global / partial / private) and matching remedies
  ("per-workgroup pinning to subgroup-aware co-scheduling"). Conclusion: "the need for
  placement-aware WG scheduling, finer data interleaving granularity, and runtime support
  for partition locality". No implemented system; gains are simulated upper bounds.
- **(d)** No compute-cluster (XCD/GPC) uplink limit, no co-residency, no hardware design.
- **(e)** No release statement found.

---

## 4. DGNA (Liu, Chen, Carlson), arXiv 2607.19922 — what it proposes beyond measurement

- The abstract's "NUMA-aware memory allocation strategies used to maintain cache
  coherence" refers to **the hardware's own** address→NUMA-node mapping and first-touch /
  replication behaviour that DGNA *characterizes* (p13: "On the A100, the allocation
  alternates every 8KB"; H100 formula), not a software allocator that DGNA builds.
- Contributions are a measurement method (latency without vendor intrinsics + Gaussian
  mixture filtering) and findings (sub-NUMA nodes, read/write home/remote behaviour). No
  placement policy, runtime or hardware proposal; no bandwidth experiments.
- Code: "DGNA will be open-sourced" (conclusion).

---

## 5. Wang, Zhou, Jiang, Zhao (SciTix), "Characterizing CUDA Locality Domains on Blackwell GPUs", arXiv 2611.07535 (5 Nov 2026), 40 pp.

- **(a)** 8 × B300 (148 SMs, two dies, 70 SMs per locality domain). Compute pinned with
  **Green Contexts** on an LD SM partition; placement verified by sampling `%smid`. Memory
  placed with `cuMemCreate` + `CU_MEM_LOCATION_TYPE_DEVICE_LOCALITY_DOMAIN` (CUDA 13.4).
- **(b)** Die-level only. 256 MB streaming with a die-0-bound accessor: interleaved
  5229.8 GB/s vs one-die-local 3628.1 GB/s; unbound 148 SMs 6975.1 GB/s; "cross-die read
  and write both fall to about 73% of the same-die figure" (Fig. 2, §5.3). 70 SMs reach
  5.2 TB/s ≈ 75 GB/s per SM, so per-SM ingest is not approached. No GPC-level data.
- **(c)** Die-aware adaptations: NCCL AllGather up to +43.5%, nvbandwidth DtoD +111.3%,
  DeepGEMM EP +37%; a driver fix for LD allocation overhead (>32 MB) cutting it 7-10×
  (submitted as open-gpu-kernel-modules issue #1499 / PR #1500).
- **(d)** No GPC placement, no co-residency, no per-SM limits.
- **(e)** Driver patch submitted upstream; no separate code statement found.

---

## 6. Kuruppu, Dangi, Li, Bai, Mitra, "Demystifying the Impact of Physical Non-Idealities on Kernel Performance", arXiv 2610.37466 (27 Oct 2026), 13 pp.

- **(a)** H100 SXM5 (132 SMs), clocks fixed at 1980/2619 MHz for microbenchmarks;
  NVML for clock/power; `%smid` to count active SMs; Tenstorrent Wormhole.
- **(b)** Table 1: unique active SMs at cluster size 1/2 = 132; at 4/8 = **120 per
  iteration, 124 across iterations** — consistent with our "SMs 124-131 never join
  clusters > 2" (H100 vs our H200). Floorsweeping explained via NVIDIA's "Skyline" patent
  (US 12,449,473, virtual GPCs + "singletons"). TDP: DSMEM/multicast reduces data movement
  and "creat[es] a power headroom for higher SM frequencies" even for compute-bound
  kernels. No per-SM or per-GPC bandwidth measurement.
- **(c)** "wave-aware clustering" (choose TPC- vs GPC-level cluster size by wave count):
  FA4 (Hopper version) "up to 5.30% over the unclustered baseline".
- **(d)** Leaves "other non-idealities to future work". No placement by bandwidth, no
  co-residency, no hardware proposal.
- **(e)** No release statement found.
- **Relevance to measurement:** clock under power cap moves with data movement, so our
  per-SM numbers in GB/s depend on the SM clock during the run (see §11).

---

## 7. Pan, ..., Rogers, "Architecting the Next Generation of Asynchronous, Distributed GPUs for the AI Era" (Accel-Sim 2.0), arXiv 2608.22602 (23 Aug 2026)

- **(a)** H100 (and B200) microbenchmarks; Accel-Sim 2.0 simulator (13.4% MAE, 0.99
  correlation on H100 per abstract).
- **(b)** Measured: "we characterize the LRC as a per-GPC structure ... coalesces up to
  four read requests into a single transaction (a 4:1 merge ratio) exclusively when those
  requests originate from SMs within the same GPC" (§II-C); uGPU stride 8192 B; "local
  write-through, remote write-back" L2 policy. Relevant to how same-GPC bandwidth tests
  must avoid shared addresses (see §1b).
- **(c)** Simulation studies: L2-bandwidth scaling ("single-CTA clusters gain up to
  2.64× and saturate near 8× bandwidth"), mbarrier polling tax, chiplet topologies. In
  "Unconstrained Bandwidth" they scale L2/memory bandwidth but state "specialized MMA
  warps must still load operands through the internal shared memory".
- **(d)** Simulator code (`addrdec.cc`): "GPC abstraction is not yet implemented, so we
  hardcode 8 here" — no GPC uplink model, no per-SM ingest limit, no co-residency study.
- **(e)** Code merged into accel-sim `dev` (PR #143, 2026-08-25).

---

## 8. LithOS (Coppock et al., SOSP'25) — only the three questions

- **Which TPCs/GPCs?** It controls *how many* TPCs, not which GPCs for bandwidth. It uses
  per-launch QMD TPC masks: "we extend prior reverse-engineering work libsmctrl and add
  support for Hopper, including handling its new TPC masking layout" (§6), and makes
  "atoms ... always multiples of the cluster size." GPCs appear only as the MIG
  granularity it rejects: "MIG offers strong but coarse GPC isolation" (§4). No
  GPC-aware or bandwidth-aware TPC selection is described.
- **Memory bandwidth?** Only in power management: "memory-bound kernels saturate
  bandwidth early, while compute-bound kernels benefit from high frequencies" (DVFS).
  Right-sizing allocates TPCs "based on runtime scaling behavior", not on per-GPC
  bandwidth.
- **Two jobs on one SM?** Explicitly out of scope: "intra-SM control is best handled by
  hardware and compilers; OS intervention would break transparency and portability.
  LithOS instead adopts the TPC as its scheduling abstraction." Mosaic notes LithOS
  "partition[s] TPCs so intra-SM interference is not relevant."
- Evaluation testbed: A100 SXM4 (functionality verified on A30, A100, H100, L4).

---

## 9. Other 2025-2026 work found

| Topic | Work | What it says that touches ours |
|---|---|---|
| GPC packing for partitioning | Bakita & Anderson, "Hardware Compute Partitioning on NVIDIA GPUs for Composable Systems", ECRTS 2025 (nvtaskset) | "We uncover that SM ID to GPC mappings are arbitrary, and configured by the NVIDIA driver in a striping-like configuration by default" and "We recommend partitioning on GPC boundaries when possible, as NVIDIA's Thread Block Groups feature will only work for a task when it has access to at least one full GPC." (§5.x). Recommendation is for clusters/isolation, not bandwidth; evaluated on A100. MIG loses 9% of A100 TPCs, 6-15% on H100. Code: libsmctrl, nvdebug, gpu-microbench at rtsrv.cs.unc.edu. |
| Same-SM co-residency | Elvinger, Strati, Enright Jerger, Klimovic, "Understanding GPU Resource Interference One Level Deeper", SoCC'25, arXiv 2501.16909 | H100: a copy (memory-bound) kernel co-resident with an FP32 compute kernel at one block of each per SM: "the execution time of copy doubles under colocation", attributed to IPC: the compute kernel "already saturates the available IPC per SM issuing 3.99 inst/cycle/SM". RTX 3090: decode TBT 5.59→10.74 ms as co-resident compute IPC rises. Also: "MPS does not enforce mutual exclusion of SMs between MPS clients ... It only restricts the maximum number of SMs available to a client." Proposes (§5.2, no design): "Better intra-SM visibility", "programmer-friendly ways to partition SMs and DRAM channels". Code: github.com/eth-easl/gpuutil-interference. |
| Same-SM co-residency | Strati et al., "Mosaic", arXiv 2610.07504 (Oct 2026) | Interference predictor (H100, A100, RTX 3090, V100). States "Memory-bound kernels, however, are largely insensitive to pipeline contention" and "kernels with very low IPC due to global-memory stalls are less affected" by warp-scheduling contention — **opposite in direction to O6**. Scheduler MosaicSched admits BE kernels by predicted interference. |
| Per-SM resources | Siavashi et al., "Dissecting GPU Utilization for LLM Inference on Nvidia Hopper", arXiv 2609.12923 | H100 NVL: occupancy for FA3/cuBLASLt is "co-limited by registers and shared memory"; e.g. FA3 config with "256 registers per MMA thread". Consistent with O7 (no residency room). |
| TMA | Bose, Avalos, Pan, Rogers, "When and Why to Use TMA", arXiv 2610.34907 | H100 SXM5: whole-GPU HBM ceiling reached by TMA at "∼3.2 TB/s (96%) at 32 KiB BIF" per SM; TMA per-issue latency tax 2-3× LDGSTS below 8 KiB. "a dedicated hardware unit (the TMA engine, one per SM)". No single-SM ingest ceiling reported. |
| TMA / DSMEM | Luo et al., arXiv 2501.12084 (2025) | H800: DSMEM to own SM 205 GB/s vs 225 GB/s smem theoretical; TMA whole-GPU >1800 GB/s on 2 TB/s part. No single-SM DRAM ceiling. |
| Operand supply without smem | Liu, Leng, Zhou, Ding et al., "FIBER", arXiv 2608.19628 | Proposes thread–register decoupled execution ("fibers" share an SM's register file) as "a redundancy-free alternative for matrix operand supply"; simulated 1.8× (Hopper), 2.09× (Blackwell) end-to-end. Operands come from the register file, not straight from global memory; it targets scheduling/parallelism, not SM ingest bandwidth. |

Searches for a 2025-2026 hardware proposal that raises per-SM ingest, adds GPC egress /
cross-GPC paths, or gives memory warps issue priority on a shared SM found nothing
(searches recorded in the final section).

---

## 10. Synthesis

| our observation | already published by | what remains ours |
|---|---|---|
| O1 one SM ingests ~205-219 GB/s from DRAM (TMA/16 B), staged ~157-170 | Nobody reports a single-SM DRAM ceiling. Fan Fig. 2 implies ≈ 250 GB/s per SM for L2 hits (figure-read). TMA papers report only whole-GPU numbers. | The per-SM DRAM ingest ceiling as a number, its independence from mechanism (TMA vs 16 B), and the staged-path penalty. |
| O2 per-GPC cap ~1.28 TB/s from DRAM, knee at ~6 SMs, no TPC/CPC limit | **Knee at 6 SMs on H200: Fan et al. §5.1.2, Fig. 2** (L2 hits, used as a topology probe). GPC port contention: Jin (MICRO'24, ratios), GPCPlace (RTX 4090 etc.). TPC 2×: Jin. | The DRAM-sourced cap in TB/s; that it is flat (ours) vs still rising to ~2.8 TB/s L2-hit (Fan) — this conflict needs resolving; no CPC effect on H200. |
| O3 16 SMs packed 1.28 vs spread 3.1-3.5 TB/s (2.4×), roof 4.63 | Qualitatively: GPCPlace Obs. 2 (135 vs 156-160 GB/s, 4.4× vs 5.6× scaling, RTX 4090), Fan Fig. 2 (L2). | The H200 magnitude (2.4× at 16 SMs) and the "5-6 memory SMs per GPC" rule; GPCPlace finds the effect small on HBM GPUs (H20/A800) — ours shows it is large on H200 for a memory-bound job alone. |
| O4 MPS appears to pack a client into few GPCs | Not published. Fan: Green Contexts tile across GPCs by default; a cluster mode concentrates. GPCPlace: Green Contexts spread. Elvinger: MPS does not enforce SM exclusivity. Bakita: SM ids striped across GPCs. | MPS placement behaviour on H200 (and its bandwidth consequence). |
| O5 borrowing via clusters/helpers adds no bandwidth | Not published (no paper tries intra-GPC borrowing for bandwidth). | Entirely ours. |
| O6 co-resident memory stream keeps 0.6-22% | Partly: Elvinger (H100) copy kernel slows 2× beside a 3.99-IPC compute kernel; Mosaic claims memory-bound kernels are "less affected". | The starvation magnitude (to <3%), its dependence on launch order and compute ILP (greedy-then-oldest-like), and the contradiction with Mosaic. |
| O7 cuBLAS FP16 GEMM uses 98% of registers | Consistent with Siavashi (register/smem co-limits) and Elvinger (H100 decode kernels block co-location by registers). | The specific number; minor. |
| O8 `bar.sync` does not wait on other CTAs' loads | Not discussed (expected semantics; Accel-Sim 2.0 models mbarrier). | Minor. |
| SMs 124-131 never in clusters >2; GPC sizes; SM-id interleave | **Fan et al. §5.1-5.2** (same SM ids on H200; 23/28 H200s have our layout); Kuruppu Table 1 (H100: 120/124). | Nothing new here; cite Fan. |
| TPC-mask placement without root on Hopper | GPCPlace (QMD TPC mask + per-bit GPC mapping), LithOS (Hopper mask layout), Bakita. | Nothing new; cite them. |

### Assessment of the four directions

1. **GPC-aware placement policy — mostly taken.** GPCPlace (Nov 2026) already does
   pack-vs-spread selection with TPC masks and per-GPU profiling, and Fan et al. (Sep
   2026) do topology/NUMA-aware placement. What is left: a policy for a *memory-bound
   job alone or beside compute* on datacenter Hopper/Blackwell that sizes "memory SMs
   per GPC" from a measured per-GPC DRAM cap (GPCPlace's HBM results show ~1.00×,
   so they never saw the H200-scale 2.4× effect), and MPS's packing. That is an
   incremental, measurement-driven contribution, not a new direction.
2. **Hardware to lift the GPC limit / borrow other GPCs' uplink — open.** No paper
   proposes more GPC egress, cross-GPC paths, or uplink sharing. Fan, GPCPlace and Jin
   all treat the GPC port as fixed and route around it in software. Caveat: Fan's L2-hit
   curve (~2.8 TB/s per GPC) disagrees with our flat ~1.28-1.62 TB/s; the case for
   hardware depends on which is right for DRAM-sourced, non-shared streams.
3. **Hardware isolation so a memory-bound job can share SMs with a compute-bound one —
   open.** Elvinger measures the slowdown and asks for "better intra-SM visibility";
   Mosaic predicts it; LithOS declares intra-SM control out of scope. No issue-priority,
   reserved memory lane, or memory-warp QoS proposal was found. Our 0.6-22% starvation
   data (and its contradiction with Mosaic's "less affected") is the strongest
   motivation in this set. The practical obstacle (O7: GEMMs leave no residency room)
   must be part of the story.
4. **Feeding the tensor core without shared memory / raising per-SM ingest — open, but
   adjacent work exists.** FIBER changes operand supply via a shared register view
   (not global→TC), Accel-Sim 2.0 keeps operands going through shared memory, and no
   paper reports or attacks a per-SM DRAM ingest ceiling. Our own data say raising
   per-SM ingest matters only after the GPC cap is lifted (6 SMs fill a GPC today), so
   direction 4 is weakest on its own and strongest combined with 2.

---

## 11. What changes how we should model or measure

1. **Avoid shared addresses in same-GPC bandwidth tests.** A per-GPC LRC merges up to
   4 reads from SMs of one GPC (Accel-Sim 2.0, measured). Each SM must read a disjoint
   region, or the GPC cap is overstated. This may explain Fan's ~2.8 TB/s.
2. **Reproduce Fan's Fig. 2 on our die** (L2-resident, disjoint vs shared buffer, 2-18
   SMs in one GPC) to settle the ~1.3-1.6 vs ~2.8 TB/s conflict before using either
   number in SimX.
3. **Separate the partition-fabric cap from the GPC cap.** 4 KB XOR interleave sends
   ~half of any stream across the LTC fabric; Fan measures remote HBM ≈ 1.3 TB/s vs
   local ≈ 2.4 TB/s on H200 (figure-read; SM set unstated). A spread 16-SM job at
   3.1-3.5 TB/s may be partly fabric-bound. Run local-only and remote-only page sets per
   GPC (Fan's remap trick: pages 2j/2j+1 differ in bit 12).
4. **Record SM clock and power during bandwidth runs.** Kuruppu et al. show TDP-driven
   frequency changes with data movement; per-SM GB/s at different clocks is not
   comparable. Lock clocks (they used 1980/2619 MHz) or log NVML clocks.
5. **Per-die variation is real.** Fan: 5 of 28 H200s have a 10-, 12- or 14-SM GPC.
   Record the GPC layout of each die we measure (we did for two).
6. **SimX/Accel-Sim modelling:** a per-GPC egress limit and a per-GPC LRC are needed;
   GPCPlace's "one request per cycle per GPC port" is unsourced, and Jin shows part of
   the GPC capacity is in parallel paths ("in space"), so model the GPC uplink as several
   ports whose usable bandwidth depends on how many L2/FB destinations are targeted.
7. **Co-residency results depend on launch order and ILP** (ours) and are contradicted
   by Mosaic's generalisation. Report both orders and the compute kernel's IPC (Elvinger
   used IPC 3.99/SM as the explanatory metric).

---

## Sources read

Fetched in full (PDF text; key figures rendered and read):
- https://arxiv.org/abs/2609.24270 , https://arxiv.org/pdf/2609.24270 (Fig. 2, 7, 8 rendered)
- https://arxiv.org/pdf/2611.24458 (GPCPlace, full)
- https://arxiv.org/pdf/2607.28824
- https://arxiv.org/pdf/2607.19922 (DGNA; proposal/conclusion sections re-read)
- https://arxiv.org/pdf/2611.07535 (intro, §5.3-5.4, overview; 40 pp. not read line by line)
- https://arxiv.org/pdf/2610.37466
- https://arxiv.org/pdf/2608.22602 (§II-C, §III)
- https://www.pdl.cmu.edu/PDL-FTP/BigLearning/lithos_sosp25.pdf (searched for the three questions)
- https://www.cs.unc.edu/~jbakita/ecrts25.pdf (searched)
- https://arxiv.org/pdf/2501.16909, https://arxiv.org/pdf/2610.07504, https://arxiv.org/pdf/2609.12923,
  https://arxiv.org/pdf/2610.34907, https://arxiv.org/pdf/2501.12084, https://arxiv.org/pdf/2608.19628 (skimmed for the listed topics)

Searches run: per-SM bandwidth / GPC placement on Hopper; intra-SM co-location /
warp priority isolation; tensor-core operands bypassing shared memory / TMA
architecture; nvtaskset ECRTS'25; GPU NoC / GPC crossbar hardware proposals at
MICRO/ISCA/HPCA 2025-2026. None found a hardware proposal for GPC egress, per-SM ingest
or memory-warp issue priority. Venue proceedings were not searched directly.
