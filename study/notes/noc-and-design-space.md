# The SM-to-memory network, and how a memory-bound job on few SMs could use more of the memory

Written 2026-11-21. Repository state: `vortex` branch `feature/dxa-borrowing` at
3ae4586ae. No simulation was run for this report. Every code claim below comes
from reading the code at that commit and is cited as file:line. External claims
carry a URL and a tag:
- **FACT**: vendor documentation or a peer-reviewed measurement.
- **INFERENCE**: an arXiv paper, a reverse-engineering result, or arithmetic done for this report.
- **RUMOR**: a forum post or an unsourced claim.

Section 9 lists what is unverified. Diagram specifications are in
`noc-and-design-space-figures.md`.

---

## 0. Answer first

1. **Vortex has no network today.** There is no topology, no distance and no
   hop-by-hop latency.
   - Each SM (a socket of one core in `h200s`) has 4 memory ports. Each port is a
     private fixed-delay pipe, 94 cycles each way and one 64 B line per cycle,
     into one flat L2.
   - The L2 has about 90 inputs and 16 banks. Each bank arbitrates its inputs
     round-robin, in a two-level tree that the fan-out limit of 8 forces. Banks
     reach the 16 memory channels through omega networks.
   - So the "NoC" is one fixed delay (`L2_LINK_LATENCY` = 94) plus the L2's
     input arbitration.
   - The ~659-cycle memory round trip decomposes as: 2 × 94 link + 304 cycles
     of fixed "PHY/NoC" delay at the DRAM model + about 167 cycles of pipelines
     and DRAM timing.
   - I found no 256-cycle latency anywhere in the configuration. 256 is the DRAM
     scheduler window (bursts), the DXA in-flight lines, and the L2 MSHRs per bank.

2. **The real GPU is hierarchical.** The levels are SM → TPC (2 SMs behind a mux) →
   CPC (2–3 TPCs, undocumented) → GPC (7–9 TPCs) → crossbar → L2 slice (about 80 on
   H100, in 2 partitions) → FB partition / memory controller. There is also a
   separate SM-to-SM (DSMEM) network inside each GPC.
   - The levels and the per-level bandwidth "speedups" were measured by Jin et
     al., MICRO 2024. The GPC uplink on H100 is provisioned at about 85% of its SMs' sum.
   - The two L2 partitions add about 160–175 cycles to "far" accesses.

3. **A network model matters for only one question, and the minimal model answers it.**
   - The one question: **where does the borrowed data enter the consumer SM, and is
     that entry point shared with the SM's normal L2 response port?**
   - A minimal hierarchical model is enough: group switches with per-level latency
     and bandwidth caps, plus an explicit per-SM ingress port with a
     "shared / separate" knob.
   - A routed mesh with flow control is not needed for this research.
   - Costs, for an expert with AI help:
     - Minimal model in SimX + RTL, with parity: about 2–3 weeks.
     - Routed network: about 2–3 months.

4. **The design space has three resources per SM, not one.**
   - **R, request machinery**: issue slots, MSHR/LSU slots, DXA/TMA workers and
     in-flight lines, and the SM's L2 port(s).
   - **S, staging**: shared memory (LMEM) capacity and its 128 B/clock of bank bandwidth.
   - **C, consumption**: tensor core / FMA throughput and issue.

   TMA borrowing adds R to the few memory SMs. It helps only until S or C binds on
   those same SMs. Two facts make that ceiling close:
   - (a) In Vortex the LMEM's 128 B/clock is shared by DXA writes and every read
     (`VX_lmem_unit.sv:30-34`). Bytes staged in LMEM and then read by the consumer
     cost 2 × their size. The ceiling is therefore about 64 B/clock ≈ 127 GB/s per SM,
     against today's 51 B/clock (101 GB/s). That is **at most about 1.25× per SM**
     for a staged consumer.
   - (b) On NVIDIA there is a rumoured but unconfirmed statement that the SM's
     DSMEM receive port competes with L2 fill returns. If true, data pushed in from
     a neighbour enters through the same per-SM ingress that already caps it, and
     borrowing gains nothing.

   Write-only copies (the `bwstream_dxa` / `tmacopy` microbenchmark) do not hit
   ceiling (a). They will overstate any gain that real decode kernels would get.

5. **Software-only baselines are strong, and a reviewer will ask for them first.**
   - On Hopper today, a CTA may legally issue a TMA whose destination is another
     CTA's shared memory in the same thread-block cluster (FACT, PTX ISA). So
     "TMA borrowing" can be done in software today, but only:
     - inside one grid,
     - inside one GPC,
     - with the helper CTA resident on the lending SM.
   - MPS lets CTAs of different processes share an SM (FACT).
   - Our own H200 data:
     - A plain-load stream that shares SMs with a register-only FMA kernel keeps
       0.29× of its rate.
     - The FMA kernel keeps 0.97× (`results_gpu/h200_corun.txt`).
   - Even at 0.29×, spreading the memory job thinly over all 20 compute SMs adds
     more bandwidth than borrowing can deliver into 2 SMs under ceiling (a).
   - A TMA-based co-resident stream needs one issuing thread, so it may keep
     much more than 0.29×. This is unmeasured.
   - What hardware still adds:
     - Borrowing without residency (the compute GEMM often leaves no registers or shared memory).
     - Isolation (no issue-slot interference).
     - Reach beyond one GPC.
     - No need for fused kernels.

6. **Recommendation.** Before building anything, run four cheap measurements.
   Each one can kill an option:
   - **E1** (real H200, ~1 day): per-SM bandwidth with L2-resident data vs
     DRAM-resident data. This tells an in-flight limit from a port/ingress limit.
   - **E2** (real H200, 2–3 days): software TMA borrowing inside a
     thread-block cluster. 1 consumer CTA plus 1–7 helper CTAs, whose TMA writes
     into the consumer's shared memory. Measure the consumer's ingest. This is the
     decisive test of the borrowing idea on real silicon.
   - **E3** (real H200, ~1 day): a TMA copy co-resident with the FMA kernel, and
     with a CUTLASS GEMM. This is the software co-residency baseline.
   - **P0** (SimX, ~1 day): two zero-code checks.
     - The consumption roof of `gemm_block` with weights already in LMEM. Today
       the decode GEMM reaches only 25 GB/s per SM while the DXA copy reaches 101.
       If it stays there, Vortex will show no gain from any fetch-side mechanism.
     - DXA with 16 workers on 4 memory ports, using the existing SimX knob
       `VX_CFG_DXA_GMEM_PORTS`.

   Then prototype in SimX, in this order:
   - (P1) Remote DXA writing into a borrower's LMEM over a group switch (option 1a),
     static pairing, 2 SMs.
   - (P2) Co-resident memory-role CTAs on the compute SMs (option 6b), as the baseline.
   - (P3, if E1 shows an in-flight limit) Lender L2-prefetch (option 1b/4b), using
     the reserved `L2PROMO` descriptor field.

   Go to RTL only for the mechanism that beats P2 by a clear margin, at realistic
   consumption, in SimX.

---

# Part 1 — The network between SMs and memory

## 1a. What Vortex has today, from the code

### The levels and what they are in `h200s`

| Vortex level | Knob | h200s value | Real-GPU analogue | Code |
|---|---|---|---|---|
| core | — | one per socket | SM | `VX_socket.sv` instantiates cores |
| socket | `VX_CFG_SOCKET_SIZE` (default 1) | 1 core. 22 sockets = `NUM_CORES / SOCKET_SIZE` | none exactly. A socket of >1 cores **shares one L1 dcache/icache and one DXA core** — not like a TPC, whose SMs keep private L1s | `VX_config.toml:6`, `:327-328` (`NUM_DCACHES = up(SOCKET_SIZE/4)`), `VX_gpu_pkg.sv:146` |
| cluster | `VX_CFG_NUM_CLUSTERS` | 1 | not a GPC. A cluster owns a **private L2**; with >1 cluster the L2 is not the shared LLC (`L2_SHARED_LLC = L2_IS_LLC && NUM_CLUSTERS == 1`) and an L3 or a passthrough sits on top. Closer to an AMD XCD than to a GPC | `VX_config.toml:4`, `profiles/h100.json:39`, `VX_gpu_pkg.sv:2394-2396`, `Vortex.sv:139-170` |
| L2 | `VX_CFG_L2_*` | 8.65 MB, 16 banks, 33 ways, pseudo-LRU, 256 MSHR/bank | the L2 (both partitions merged into one) | `VX_cluster.sv:225-263`, `profiles/h200s.json:18-23`, `profiles/h100.json:88` |
| memory ports | `VX_CFG_L2_MEM_PORTS` | 16 = min(L2 banks, memory channels) | FB partitions / channels | `VX_config.toml:368` |
| DRAM | `VX_CFG_PLATFORM_MEMORY_*` | 16 HBM3E channels, 800 GB/s | one HBM3E stack | `profiles/h200s.json:25-33` |

### How one socket's requests reach the L2

**Ports.** A socket has `L1_MEM_PORTS` = min(dcache banks, memory channels) = min(4, 16) = 4 L2-facing ports
(`VX_config.toml:343`, `VX_gpu_pkg.sv:2075`). Each port carries one 64 B line
(`VX_CFG_MEM_BLOCK_SIZE = 64`, `VX_config.toml:78`) per cycle per direction.
At 1.98 GHz that is 126.7 GB/s per port.

**Port 0.** Port 0 is a priority arbiter over the icache, dcache bank-port 0,
tcache, rtcache and the DXA's global-memory port, in that order. "Icache first,
DXA last" (`VX_socket.sv:933-1005`, arbiter at `:974-988` with `ARBITER("P")`).
SimX mirrors it (`sim/simx/socket.cpp:254-294`).

**Ports 1–3.** These carry dcache bank-ports 1–3 straight through
(`VX_socket.sv:1006-1014`). In SimX only, `VX_CFG_DXA_GMEM_PORTS > 1` puts extra
DXA ports behind the dcache on ports 1..N-1 (`socket.cpp:296-311`,
`dxa/dxa_core.cpp:38-41`). The RTL fixes the DXA at one port
(`VX_socket.sv:800-803`, `GMEM_OUT_PORTS(1)`).

**Unit MMU.** Under VM, unit ports (DXA, tcache, rtcache) pass through a socket unit
MMU before the arbiter (`VX_socket.sv:853-927`).

**The link to the L2.** Each socket port crosses a **private** fixed-latency link to
its own L2 input: `VX_mem_bus_delay` with `L2_LINK_LATENCY` cycles in each
direction (`VX_cluster.sv:306-325`; SimX `cluster.cpp:125-144`, `189-196`).
- The SimX `DelayBuffer` passes one packet per cycle at full throughput. Its
  capacity is 2^ceil(log2 L) + 2 packets, which is 130 for L = 94
  (`sim/simx/delay_buffer.h:23-66`).
- No link is shared between sockets.
- `h200s` inherits `L2_LINK_LATENCY = 94` from `h100`. It was calibrated so a
  pointer chase hits the L2 in 273 cycles (`profiles/h100.json:46`, `:86`).

**The L2's inputs.** `L2_CACHE_REQS` = 22 sockets × 4 ports (`L2_SOCKET_REQS`,
`VX_gpu_pkg.sv:2318`) + the page-table walker + the command-processor client,
about 90 in all (`VX_gpu_pkg.sv:2334-2353`; SimX `constants.h:115-143`).

**Inside the L2.**
- **Bank selection.** By address bits XOR a hash (`VX_cache.sv:349-355`,
  `L2_BANK_HASH = 1`, `VX_config.toml:367`).
- **Request crossbar.** All ~90 inputs to the 16 banks, one `VX_stream_xbar` with a
  round-robin arbiter per bank (`VX_cache.sv:521-544`).
  - Because `MAX_FANOUT = 8` (`VX_platform.vh:168`), each bank's 90-input arbiter
    is split into a registered tree: 12 slices of 8, then a join arbiter, one level
    per cycle (`VX_cache.sv:118-133`; SimX `mem/cache.cpp:36-50`).
  - That tree is the only "hierarchy" in the path today, and it is an artefact
    of the fan-out limit.
- **Banks.** Each bank accepts one request per cycle. The bank pipeline latency is
  `L2_LATENCY = 4 + log2(size/1 MB)` = 8 (`VX_config.toml:354`).
- **Banks to memory ports.** A radix-2 omega network (`VX_cache.sv:876-893`), and an
  omega network back for fills (`:264-284`). Bank responses go to the requesting
  input either through an omega network or through per-output arbiters
  (`:690-766`). SimX models the omega networks as registered stages
  (`cache.cpp:56-58`, `3256-3330`).

**L2 to DRAM.**
- The L3 level is a passthrough when disabled (`Vortex.sv:151-170`).
- A round-robin crossbar sends each memory port to the channel chosen by
  `DramSim::Config::locate()`: 1 KB interleave with a channel/bank hash (SimX
  `mem/memory.cpp:63-65`; rtlsim per-port queues `sim/rtlsim/processor.cpp:443-459`).
  This crossbar sits in the simulator harness, not in the RTL.
- Each channel is a Ramulator HBM3E controller with a 256-entry scheduler window.
- A read response is released `PLATFORM_MEMORY_LATENCY` = 304 cycles after
  Ramulator completes it (`sim/common/dram_sim.cpp:280`, `:386`;
  `profiles/h100.json:55`, `:87`).

**Responses.** Responses retrace the same path. The tag carries the arbiter-select
bits pushed at every arbiter, in both RTL and SimX (`VX_mem_bus_arb`;
`sim/simx/types.h:1806-1912`, tag shift at about `:1848`, pop at about `:1908`).

### The DXA's path

**Structure.**
- There is one `VX_dxa_core` per socket, serving the socket's cores
  (`NUM_REQS = SOCKET_SIZE`) (`VX_socket.sv:776-815`).
- It has `VX_CFG_NUM_DXA_CORES` workers (8 in `h200s`, `profiles/h200s.json:12`).
  The default formula `max(1, NUM_CORES/8)` is a leftover from the cluster-level
  pool (`VX_config.toml:442`; upstream candidate #8).
- Each worker holds `VX_CFG_DXA_MAX_INFLIGHT` lines (256 in `h200s`).

**Global-memory side.** The workers merge through one `VX_mem_bus_arb` onto the
single global-memory port (`VX_dxa_core.sv:191-199`). That port enters socket port 0
at the lowest priority.

**LMEM side.**
- Workers merge through another arbiter (`VX_dxa_core.sv:207`) onto one LMEM
  write stream of `DXA_LMEM_WORD_SIZE` = 16 banks × 8 B = 128 B per beat
  (`VX_dxa_pkg.sv:24`).
- That stream is routed to the destination core by the core id in the tag,
  through a `VX_mem_bus_switch` (`VX_socket.sv:817-850`).
- **This socket-local switch is the only SM-to-SM-capable data path in Vortex.**
- At the core, DXA writes and TCU reads share one LMEM DMA port, round-robin
  (`core/VX_mem_unit.sv:100-127`).
- **Completion is detected at the destination LMEM** from the attributes of the
  write beats (`VX_mem_unit.sv:129-148`).
- **A DMA access takes whole bank rows and blocks the LSU's passes**
  (`mem/VX_lmem_unit.sv:30-34`, `:501-510`). LMEM bandwidth is shared, not dual-ported.

**History.** Before `7d6950de5` (2026-07-11) the DXA core sat in the cluster, with its
own L2 ports and a per-socket LMEM switch (`git show 7d6950de5^:hw/rtl/VX_cluster.sv`,
lines ~185-300). That is a ready template for a cluster-level pool.

**Multicast** writes several CTAs **of the same core** (`sw/kernel/include/vx_dxa.h:156-157`).

**No cross-core shared memory.**
- Vortex thread-block clusters are co-resident on one core
  (`docs/designs/cta_dispatch_architecture.md:130-133`).
- There is no cross-core DSMEM in RTL or SimX. A grep for dsmem/remote LMEM finds
  only design docs. Cross-core DSMEM is listed as unbuilt in
  `docs/designs/dxa_async_copy_multicast.md` §7 item 6.
- Item 1 of the same list is stale: it says the DXA is still cluster-scoped.

### Latency a request sees today (h200s, unloaded)

| Segment | Cycles | Source |
|---|---|---|
| core → dcache bank pipeline | `DCACHE_LATENCY` = 2 + log2(256 KB / 16 KB) = 6; `L1_LINK_LATENCY` = 0 | `VX_config.toml:310`, `:318` |
| socket arbiter + registered socket boundary | a few (REQ/RSP_OUT_BUF 3, one RegSlice each way in SimX) | `VX_socket.sv:974-988`, `socket.cpp:235-251` |
| SM → L2 link | 94 | `profiles/h100.json:46` |
| L2 input arbiter tree (2 levels) + bank pipeline 8 | ~10 | above |
| L2 hit back to SM | 94 + pipeline | |
| **L2 hit round trip (calibrated)** | **273** | `profiles/h100.json:85-86` |
| L2 miss: bank → omega → memory crossbar → Ramulator array timing (tRCD/CL/burst) | tens | `dram_sim.cpp` |
| fixed "PHY/NoC" delay on the read response | 304 | `profiles/h100.json:55`, `:87` |
| **DRAM round trip (calibrated)** | **658.7** | `profiles/h100.json:62`, `:87` |

So 658.7 = 188 (link) + 304 (fixed) + about 167 (all pipelines plus DRAM timing).
The 273 and 658.7 targets are a pointer chase on the H100 PCIe part
(Blackwell-dissection paper cited in `profiles/h100.json:85`). In `h200s` they
are inherited, not measured. There is **no "256-cycle part"** in this
configuration. 256 appears only as:
- the DRAM scheduler window (`PLATFORM_MEMORY_SCHED_WINDOW`),
- the DXA in-flight lines,
- the L2 MSHR entries per bank.

### Is there any topology?

**No.**
- Every socket port has the same private 94-cycle pipe to the same flat L2.
- There is no per-SM-group bandwidth cap, no near/far L2, and no SM-to-SM path
  beyond the socket-local DXA switch.
- Contention exists only at the L2 bank arbiters, the memory crossbar and the DRAM.

Consequences:
- SM placement cannot matter in today's model.
- "Neighbour" has no meaning except "same socket".
- Any mechanism that needs a path from one SM to another must add one.

### Knobs that already exist

- `VX_CFG_NUM_CLUSTERS` (private L2 per cluster), `VX_CFG_SOCKET_SIZE` (cores sharing
  L1 and DXA), `VX_CFG_L1_LINK_LATENCY`, `VX_CFG_L2_LINK_LATENCY`,
  `VX_CFG_L1_MEM_PORTS`, `VX_CFG_L2_MEM_PORTS`, `VX_CFG_L2_NUM_BANKS`,
  `VX_CFG_L2_BANK_HASH`, `VX_CFG_PLATFORM_MEMORY_LATENCY`,
  `VX_CFG_PLATFORM_MEMORY_INTERLEAVE_SIZE`, `VX_CFG_NUM_DXA_CORES`,
  `VX_CFG_DXA_MAX_INFLIGHT`, `VX_CFG_DXA_GMEM_PORTS` (SimX only).
- `VX_CFG_DXA_MEM_PORTS` (`VX_config.toml:443`) is not read by any RTL or SimX file. It is stale.
- `ci/testcases/dxa.yaml:368-422` pass `-DVX_CFG_NUM_DXA_UNITS=4` in 10 cases.
  That macro exists nowhere in the config, RTL or SimX, so those cases run with the
  derived worker count. This is a small stale-test finding, worth adding to the
  upstream candidates.
- The same cases do exercise `SOCKET_SIZE` = 2 and 3 with a shared DXA on SimX and
  rtlsim, so the multi-core-per-engine path is tested.

### Reusable components already in the tree

| Component | What it is | Used today |
|---|---|---|
| `hw/rtl/mem/VX_mem_bus_arb.sv` / SimX `TxRxArbiter` (`types.h:1806`) | N→M arbiter, tag-routed responses | socket port 0, everywhere |
| `hw/rtl/mem/VX_mem_bus_switch.sv` | 1→N by a select signal | DXA LMEM fan-out (`VX_socket.sv:835`) |
| `hw/rtl/mem/VX_mem_xbar.sv` | N:M crossbar with per-input select, tag-routed responses | **not instantiated anywhere** |
| SimX `MemCrossBar` = `TxRxCrossBar` (`types.h:1926`) | N:M crossbar | memory ports → channels (`memory.cpp:63`) |
| `hw/rtl/libs/VX_stream_xbar.sv`, `VX_stream_omega.sv`, `VX_stream_xpoint.sv`, `VX_stream_arb.sv` | generic stream crossbar, radix-k omega network, arbiter tree | inside `VX_cache` |
| `hw/rtl/mem/VX_mem_bus_delay.sv` / SimX `DelayBuffer` | fixed-latency elastic pipe | L1 and L2 links |
| `hw/rtl/mem/VX_mem_axi_xbar.sv`, `libs/VX_mm_axi_xbar.sv` | AXI crossbars | platform side |
| `hw/unittest/stream_arb`, `cache`, `dxa_core`, `lmem_unit` | unit-test harnesses | |
| `ci/profile_check.py` | pointer-chase latency calibration | profiles |

No router, no credit-based flow control, no virtual channels and no flit-level
packetization exist.

## 1b. What is known about real GPU interconnects

### NVIDIA Hopper: the levels

| Level | What is known | Tag and source |
|---|---|---|
| SM | 228 KB unified L1 / shared memory, about 128 B/clk shared-memory bandwidth | FACT: H100 whitepaper. INFERENCE: Luo et al. measure L1 ~126 B/clk and smem ~128 B/clk on H800 (arXiv 2402.13499, https://arxiv.org/html/2402.13499v1) |
| TPC | 2 SMs; "within a TPC, two SMs are connected by a multiplexer"; read "speedup" 2, so the mux does not halve per-SM read bandwidth | FACT, measured: Jin et al., "Uncovering Real GPU NoC Characteristics", MICRO 2024, https://people.ece.ubc.ca/aamodt/publications/papers/realgpu-noc.micro2024.pdf |
| CPC | 2–3 TPCs. Not documented by NVIDIA. Seen in SM-to-SM latency steps (minimum 196 cycles within CPC0) and named in NVIDIA's multicast patent | INFERENCE: Jin et al.; patent US12020035 https://patents.google.com/patent/US12020035 |
| GPC | 7–9 TPCs (H100 SXM: 8 GPCs, 66 TPCs, 132 SMs; full GH100 144 SMs). Uplink provisioned at about 85% of its SMs' sum on H100 (needs 9×, gets ~8×) | FACT: whitepaper. FACT, measured: Jin et al. |
| SM-to-SM network | "A dedicated SM-to-SM network for SMs in a GPC". Thread-block clusters run "concurrently across SMs within a GPC". DSMEM "can be used by an SM simultaneously with L2 cache accesses". Max portable cluster 8, 16 non-portable | FACT: H100 whitepaper v1.02 (mirror https://www.hpctech.co.jp/assets/images/info/catalog/pdf/gtc22-whitepaper-hopper_v1.02.pdf); Hopper Tuning Guide https://docs.nvidia.com/cuda/hopper-tuning-guide/index.html |
| DSMEM performance | SM-to-SM latency 180 cycles; aggregate 3.27 TB/s (cluster size 2), 2.65 TB/s (cluster size 4) on H800 | INFERENCE: Luo et al. |
| DSMEM port | "Each GPC has an internal crossbar for DSMEM … independent of GPU NOC used to access the L2"; about 32 B/clk per SM per direction; **"receive port competes with L2 fill returns"**. An NVIDIA employee on the same thread says the topology is not published | RUMOR: forum user, https://forums.developer.nvidia.com/t/what-is-the-inter-sm-linkage-of-dsm-cluster/300533 |
| Crossbar | "partitioned crossbar structure" connecting GPCs to L2 partitions. Nsight units: `xbar`, `gxc` (GPC XBAR compressor), `lrc` (L2 request coalescer, which "serves programmatic multicast requests"), `ltc`, `ltcfabric` ("communication fabric for the L2 cache partitions"), `lts` (slice), `fbpa` (memory controller) | FACT: whitepaper; Nsight Compute Profiling Guide https://docs.nvidia.com/nsight-compute/ProfilingGuide/index.html |
| L2 | 2 partitions. About 80 slices on H100 (5 FBPs per partition). Near/far latency gap about 160–175 cycles (A100 161, H100 174–175; sub-NUMA ~32 cycles). Far access "nearly twice as long". L2 bandwidth >5.5 TB/s near-footprint, 3.8 TB/s over 50 MB on H100 PCIe | INFERENCE: GemAtlas arXiv 2611.13860 https://arxiv.org/html/2611.13860; DGNA arXiv 2607.19922 https://arxiv.org/pdf/2607.19922; Chips and Cheese https://chipsandcheese.com/p/nvidias-h100-funny-l2-and-tons-of-bandwidth |
| Slice to channel | "less than a full crossbar" between memory channels and LLC slices | FACT that it is claimed: patent US11429534 https://patents.google.com/patent/US11429534 |
| Multicast data path | request to a per-slice LRC; read once; one response packet duplicated at separation points; crossbar multidrop to CPCs; a GPC-arbiter multicast switch delivers to SMs; the receiving SM writes SMEM at the carried offset and updates the barrier | FACT that it is claimed: patent US12020035 |
| Per-SM bandwidth | V100: one SM to one slice ~34 GB/s; a GPC to one slice ~85 GB/s. A100: near slice 39.5 vs far 26 GB/s per SM. Bandwidth to slices is "(mostly) uniform"; latency varies up to ~70% with location | FACT, measured: Jin et al. |
| Isolation | Only MIG isolates "on-chip crossbar ports, L2 cache banks, memory controllers, and DRAM address busses" | FACT: whitepaper (A100/H100 MIG) |

### Is it hierarchical, and at which levels?

**Yes.** The request path is SM → TPC mux → CPC → GPC uplink → crossbar → L2
slice in one of 2 partitions → FBPA/memory controller → HBM channel. There is a
second network, SM-to-SM within the GPC. The partition crossing is a further
level (`ltcfabric`).

How solid each part is:
- The SM/TPC/GPC/partition levels and their bandwidth ratios are measured
  (Jin et al.). That is FACT.
- The internal switch structure (crossbar vs mesh vs ring per level) is not
  published. That is INFERENCE at best.

### Simulators

**GPGPU-Sim / Accel-Sim** (FACT, from the source and configs).
- `-network_mode 1` is intersim2 (BookSim-derived). `-network_mode 2` is `LOCAL_XBAR`.
- `LOCAL_XBAR` assumes one-flit packets (flit 40 B), 2 subnets (request and reply),
  and iSLIP arbitration. Its buffers are `-icnt_in/out_buffer_limit`.
- `SM90_H100` uses intersim `topology = fly; k = 212; n = 1`. That is one flat
  crossbar between 132 SMs and 80 sub-partitions, with internal speedup 4.0 and
  256-entry buffers.
  (https://github.com/accel-sim/gpgpu-sim_distribution/tree/dev/configs/tested-cfgs/SM90_H100)
- **Stock Accel-Sim has no TPC/GPC uplink, no L2 partitions and no SM-to-SM
  network.** GemAtlas added partitions and GPC affinity and cut H100 error from
  37.7% to 21.3% MAPE (INFERENCE).

**Other simulators.**
- MGPUSim/Akita: a coarse tree network (INFERENCE, https://github.com/sarchlab/mgpusim).
- gem5 Ruby: Simple or Garnet networks (FACT).

**Classic GPU-NoC papers.**
- Bakhoda et al., MICRO 2010 (https://icn.kaist.ac.kr/~jjk12/papers/2010MICRO.pdf):
  many-to-few-to-many traffic; the reply network is the bottleneck.
- Kim et al., PACT 2020 SRC (https://people.ece.ubc.ca/~aamodt/papers/gpu-noc.pact2020.pdf):
  the NoC-to-memory interface, not bisection bandwidth, is the bottleneck.

### AMD and edge GPUs

**AMD MI300X** (INFERENCE, Chips and Cheese https://old.chipsandcheese.com/2024/06/25/testing-amds-giant-mi300x/).
- 8 XCDs × 38 CUs, a private 4 MB L2 per XCD, 4 IODs.
- A 256 MB Infinity Cache (~218 ns, 11.9 TB/s measured) in front of HBM3.
- The hierarchy is CU → XCD L2 → Infinity Fabric → Infinity Cache/memory.
- Vortex's multi-cluster mode (a private L2 per cluster, an L3 on top) resembles
  this more than it resembles a GPC.
- No per-CU bandwidth limit is published.

**Qualcomm Adreno X1** (Snapdragon X Elite; INFERENCE, Chips and Cheese https://chipsandcheese.com/p/the-snapdragon-x-elites-adreno-igpu).
- 6 SPs (my count from 1536 FP32 lanes).
- 3 MB GMEM, 3 × 128 KB cluster caches, 6 MB system-level cache (~211 GB/s).
- 128-bit LPDDR5X.
- Local memory is ~128 B/clk per SP.
- Adreno 690 DRAM latency is over 400 ns (https://chipsandcheese.com/2024/04/24/sizing-up-qualcomms-8cx-gen-3-igpu/).
- Per-SP DRAM bandwidth: **not published or measured anywhere I found.**

**Arm Mali/Immortalis G720** (FACT, vendor, https://developer.arm.com/community/arm-community-blogs/b/announcements/posts/arm-gpus-built-on-new-fifth-gen-architecture).
- Shader cores, then 2 or 4 L2 slices, then the system interconnect.
- No per-core bandwidth data.

**Apple M2 Pro / M3 Pro.**
- 200 / 150 GB/s unified memory behind a system-level cache (INFERENCE,
  https://chipsandcheese.com/2023/10/31/a-brief-look-at-apples-m2-pro-igpu/).
- No per-core data.

**Edge summary.** The hierarchy is shallower: a few cores → L2 slices or a cluster
cache → a system-level cache shared with the CPU and NPU → LPDDR. DRAM latency is
higher (≥400 ns vs ~330 ns on H200).

## 1c. Does a network model matter for our question?

Only where the mechanism moves data or requests **between SMs**, or where SM
placement changes a cap. Per mechanism (numbering as in Part 2):

| Mechanism | Does topology / arbitration change the answer? | What has to be modelled |
|---|---|---|
| 1a remote engine writes into borrower's LMEM | **Yes, decisively.** It needs an SM-to-SM path. Its bandwidth, and **whether it shares the borrower's ingress with L2 responses**, set the gain. On NVIDIA the path exists only inside a GPC (cluster ≤ 8/16). | group membership; SM-to-SM path width; an ingress knob (shared / separate) |
| 1b lender prefetches into L2, borrower pulls | Little: data comes back over the borrower's normal port. Near/far L2 partition latency changes the in-flight need. | L2 hit latency; optionally near/far |
| 1c/2 cluster pool of engines/MSHRs/ports | Yes. The pool sits at a group level, so the group uplink width is the cap (H100 GPC ≈ 8 SMs' worth). | group uplink bandwidth cap |
| 3 priority/ports for designated SMs | Priority only matters under contention. In our scenario the compute job uses 1.7% of the memory, so the network is not contended and priority gains ~0. Extra ports matter only if the SM port is the cap. | per-SM port count/width (exists) |
| 4 relay through a neighbour's L1/LMEM | As 1a, plus the lender's own port. | as 1a |
| 5 L2/memory-side push | Needs a delivery path from L2 to the SM's LMEM. The per-SM ingress is the cap. | ingress knob |
| 6 co-residency / scheduling | No: each SM uses its own port. | nothing new |

**Cheapest model that would not mislead us.**
- One group level ("GPC") of G sockets with a configurable uplink: P ports and
  L_g cycles.
- The existing L2 link, split into SM→group and group→L2 latency.
- An optional SM-to-SM path inside a group, for DXA LMEM writes only.
- One explicit knob: does traffic arriving over the SM-to-SM path consume the
  destination socket's L2-response ingress slot (shared) or not (separate)?
- Running both settings brackets the real part, whose behaviour is unknown.
- Near/far L2 partitions are a second-order refinement. They shift latency by
  ~170 cycles, which matters only if the per-SM limit is in-flight-bound (E1 decides).
- A routed mesh adds nothing to these questions.

## 1d. Implementation plan

Effort assumes an expert with AI assistance and the repository's rules
(AGENTS.md): SimX/RTL in lockstep, `model_parity` cases, `logic_gate`
(beacon) for any RTL that can move timing, and the `gpu` beacon group (~30 min,
~95 GB) for a cluster or L2 change.

### (A) Minimal: hierarchical arbiter model with per-level latency and bandwidth caps

**Structure.** Socket ports (4 per SM) → group arbiter (G sockets × 4 inputs → P_g
outputs) → group link (L_g) → L2 inputs (now `NUM_GROUPS × P_g` instead of
`NUM_SOCKETS × 4`) → L2 banks (unchanged) → memory (unchanged).

**Default: cycle-identical.** With `VX_CFG_L2_GROUP_SIZE = 0` (off) the cluster keeps
the direct per-port links of `VX_cluster.sv:306-325` / `cluster.cpp:189-196`,
untouched.

**Knobs** (new, in `VX_config.toml` under `[l2]`):
- `VX_CFG_L2_GROUP_SIZE` (sockets per group, 0 = off).
- `VX_CFG_L2_GROUP_PORTS` (uplink ports per group; default `GROUP_SIZE × L1_MEM_PORTS`,
  meaning no concentration).
- `VX_CFG_L2_GROUP_LATENCY` (SM→group cycles; the remainder of `L2_LINK_LATENCY`
  is group→L2).
- `VX_CFG_L2_GROUP_ARB` (round-robin or priority).
- Optional `VX_CFG_L2_GROUP_SEL` (how a request picks an uplink port: by bank bits,
  so ordering per address holds, or by source port).

**RTL.**
- `hw/rtl/VX_cluster.sv`: a generate block per group with one `VX_mem_bus_arb`
  (NUM_INPUTS = G × L1_MEM_PORTS, NUM_OUTPUTS = P_g, TAG_SEL_IDX as today) plus
  `VX_mem_bus_delay` before and after it.
- `hw/rtl/VX_gpu_pkg.sv`: `L2_SOCKET_REQS` becomes `NUM_GROUPS × P_g`, and
  `L2_TAG_WIDTH` grows by `ARB_SEL_BITS(G × 4, P_g)`.
  - The tag-width ripple is the main risk. It is the same class of bug as upstream
    candidate #12, the DXA tag that did not fit.
  - Check every `ASSIGN_VX_MEM_BUS_IF_EX` width and the L2 MSHR tag storage.
- Size: about 150–250 lines.

**SimX.**
- `sim/simx/cluster.cpp`: build a `MemArbiter` (`TxRxArbiter`, same tag convention)
  per group plus `DelayBuffer`s, instead of `bind_l2_link`.
- `sim/simx/constants.h`: `VX_CFG_L2_DEMAND_REQS` / `L2_CACHE_REQS`.
- Size: about 100–200 lines.

**What it changes besides the intent.** Fewer L2 inputs means the L2 bank arbiter tree
can drop a level (90 → e.g. 24 inputs: 3 slices instead of 12). That moves the L2
latency by ~1 cycle. It is inherent, so report it and re-run `ci/profile_check.py`.

**Tests.**
- **Default build.** The whole existing suite, plus perf_gate baselines unchanged
  to the cycle. That is the cycle-identity proof.
- **New `model_parity` cases** in `ci/testcases/core.yaml` and `dxa.yaml`:
  `bwstream -b32`, `sgemm`, `bwstream_dxa` at 8 cores, `--l2cache`, GROUP_SIZE=4,
  PORTS=2 and 4.
- **A directed concentration test** using an existing app (`bwstream` on all SMs of
  one group vs spread over groups): per-group bandwidth must cap at P_g × 64 B/clk.
- **Unit level.** `hw/unittest/stream_arb` already covers the arbiter.

**Effort.**
- SimX 2–4 days.
- RTL 4–7 days, including beacon/lint and rtlsim parity at 8 cores.
- Total about 1.5–2 weeks.

**(A+) Option for the research: an SM-to-SM path inside a group.**
- Turn the per-socket DXA LMEM switch (`VX_socket.sv:817-850`) into a group-level
  `VX_mem_xbar` (exists, unused) from the G sockets' DXA LMEM outputs to the G × cores
  LMEM DMA ports.
- Add a small request crossbar for `VX_dxa_req_bus_if` from every core to every DXA
  core in the group.
- Add the ingress knob: a shared mode, in which a remote write must also win the
  destination socket's response-port slot, and a separate mode.
- SimX: a `DxaGroupNet` SimObject with channels only (the Cardinal Rule).
- Effort: about +1–1.5 weeks with parity tests.
- This is what option 1a needs. See Part 2 for its other changes.

### (B) Fuller: routed network with flow control

**Structure.** Network interfaces turn `VX_mem_bus_if` packets (64 B data plus a tag)
into flits (16–32 B). Routers sit at sockets, L2 slices and memory ports, as a
crossbar or a 2D mesh. They need:
- credit-based flow control,
- separate request and reply subnets (deadlock freedom for request/response,
  as Accel-Sim does),
- XY routing,
- an SM-to-SM traffic class,
- configurable link width and router latency.

**New modules.**
- RTL: `VX_noc_router`, `VX_noc_ni_req/rsp`, `VX_noc_link`, a topology generator in
  `VX_cluster.sv`.
- SimX: `NocRouter`, `NocNi`, `NocBuilder` SimObjects.
- Reuse: `VX_stream_arb` for switch allocation, `VX_fifo_queue`/`VX_elastic_buffer`
  for VC buffers, `VX_stream_omega` as a reference multistage network.

**Parity.** Every router's arbitration must match cycle for cycle. Build a SimX/RTL
trace-diff harness per router (the AGENTS.md "SimX-as-oracle" pattern), plus
directed tests: hot-spot, uniform random, SM-to-SM transpose. Add `logic_gate`
DUTs for the router.

**Effort.**
- SimX 3–5 weeks (1,500–2,500 lines).
- RTL 6–10 weeks (2,500–4,000 lines), including verification and synthesis.
- An alternative is to plug BookSim into SimX only, the way Accel-Sim does
  (about 1 week). It breaks the RTL parity rule, so it is only usable as a side study.

**Risks.** Deadlock or ordering bugs (responses to one line from different paths),
tag growth, Fmax, and the size of the `gpu` beacon/synthesis runs.

### What the research needs

| Needed | Why |
|---|---|
| (A) with group uplink caps | So that pooling at a group level hits a realistic uplink limit, and "neighbour" has a meaning |
| (A+) SM-to-SM DXA path with the ingress knob | Options 1a, 4 and 5 cannot be evaluated without it |
| Near/far L2 partitions | Only if E1 shows the per-SM limit is in-flight-bound |
| (B) routed network | **Not needed.** None of the questions turns on routing, flow control or hop counts |

---

# Part 2 — Design space: how a memory-bound job on few SMs can use more of the memory

## 2.0 What limits one SM to ~100 GB/s

**Common numbers.**
- 1.98 GHz, so 101 GB/s = **51 B/clock** and 107 GB/s = 54 B/clock.
- Real H200: 4.8 TB/s over 132 SMs = 36 GB/s fair share per SM.
- `h200s`: 800 GB/s = 404 B/clock peak; streaming reaches 714–734 GB/s.
- The compute job uses ~1.7% of the memory, which is ~14 GB/s on `h200s`.
  So the memory job can have about **P ≈ 716 GB/s**.

### Real H200 (measured by us; the causes are inferred)

**What was measured.**
- One SM streams 107 GB/s with plain loads (8 SMs under MPS: 858 GB/s).
- A TMA write-only copy reaches 101 GB/s with four 16 KB transfers outstanding,
  98.5 with two 32 KB, and 85 with one 64 KB (`results_gpu/h200_tmacopy.txt`).
- With 4 KB tiles the copy stops at 29.5 GB/s whatever the depth (8 KB: 58.5).
  - So below 16 KB the limit is the rate of transfers (about one per ~275 cycles
    per issuing thread/SM), not bytes.
  - Above 16 KB both paths converge at ~100–107 GB/s.

**Inferred limits.** TMA and plain loads hit the same ~51–54 B/clock cap. The copy
only writes shared memory (it reads one word per tile). So **the cap is not
shared-memory bandwidth** (the copy uses ~51 of 128 B/clock). It is upstream. Two
candidates, both INFERENCE:
- (i) **Bytes in flight per SM.** By Little's law at the unloaded 659-cycle (333 ns)
  round trip, the cap is ~34 KB, about 260 × 128 B lines. At a loaded 600–800 ns
  (the research agent's figure) it is 60–80 KB.
- (ii) **The SM's ingress port.** 64 B/clock would be 127 GB/s; at ~80–85%
  efficiency that gives the 101–107 GB/s seen.

**Not the limit.**
- The TPC mux: read speedup 2 (Jin et al.).
- For a few SMs, the GPC uplink: ~8 SMs' worth.

The rumoured 32 B/clock DSMEM port is below 51 B/clock, so it cannot be the
L2-fill width.

**E1 separates (i) from (ii).** L2-resident data at 273 cycles would raise the
per-SM rate ~2.4× under (i) and ≤1.25× under (ii). This measurement is not in our data.

**Correction to the notes.** MPS placed "3%" and "6%" both on 8 SMs
(`results_gpu/h200_fullsm.txt`, `pct=3` and `pct=6` → `sms=8`). So the "5% of SMs"
decode point in `corun-workloads.md` is probably 8 SMs: 831/8 ≈ 104 GB/s per SM,
not "6–7 SMs, 125 per SM". Likewise 10% is probably 14 SMs (88 per SM).

### Vortex h200s (configured; mostly measured)

| Limit | Value | Ceiling it implies | Measured | Binding? |
|---|---|---|---|---|
| dcache MSHR / LSU pending | 512 lines × 64 B = 32 KB (`profiles/h200s.json:6-14`) | Little: 32 KB / 659 = 49.7 B/clk = 98 GB/s (DRAM); 120 B/clk at the 273-cycle L2 hit | plain stream 77 GB/s on 1 SM (block shape, FWD_HOLD=1) | yes for plain loads (plus open losses) |
| dcache banks / response | 4 banks × 32 B words = 128 B/clk; 2-entry bank response queue held (`VX_cache_bank.sv:1356-1360`) | 128 B/clk | bank count ±4% | no at DRAM latency; possibly at L2-hit latency |
| socket L2 ports | 4 × 64 B/clk; the dcache uses all 4, **the DXA only port 0, at lowest priority** | DXA: 64 B/clk = 127 GB/s | DXA copy 101 (8 workers), 111 (16 workers); 4 workers 84 / 90 / 94.5 with 1 / 2 / 4 ports | **yes for the DXA** |
| DXA workers × in-flight | 8 × 256 lines = 2,048 lines, against 659 lines needed at 64 B/clk | ≫ port | in-flight 256 is the smallest that does not bind | no |
| LMEM bank bandwidth | 16 × 8 B = 128 B/clk, **shared** by DXA writes, TCU reads and LSU passes (`VX_lmem_unit.sv:30-34`; `VX_mem_unit.sv:111-125`) | write-only copy ≤128 B/clk; staged-and-consumed ≤ ~64 B/clk = 127 GB/s | not measured at its limit | not yet; it is the next ceiling |
| issue / tensor core (consumer) | `gemm_block` DXA mode 25.7 GB/s per SM at 1 SM; load mode 16 | — | — | **yes for the decode GEMM today**: it is consumption-bound at 1/4 of the fetch cap |

**Two consequences for the study.**
- (1) In Vortex the per-SM fetch cap for the DXA is the **one 64 B/clk port**, not the
  engines. Pooling engines without ports cannot help.
- (2) Vortex's decode GEMM cannot consume even its own SM's fetch cap. Any
  fetch-side mechanism shows **zero** gain on `gemm_block` until the consumer is fixed
  (P0). Only the copy microbenchmark would show gains, and those overstate real
  kernels (ceiling of 2.5).

### The ceiling every "more bytes into the same SM" scheme meets

Data used once (decode weights) is written into LMEM/shared memory and read once
by the consumer. If both use the same 128 B/clk banks (true in Vortex; unknown on
NVIDIA), the SM can ingest at most ~64 B/clk ≈ 127 GB/s. That is **≤ 1.25× today's 101**.
- On NVIDIA the wgmma operand path may be wider than 128 B/clk. A dense wgmma GEMM
  appears to need ~74 B/clk of operand reads alone (my arithmetic), which hints that
  it is wider, so this ceiling may not hold there.
- The DSMEM receive port may share the L2-fill ingress (RUMOR).
- Either way **the borrower SM's ingress is the quantity the research must measure
  first** (E2).

### Symbols used below

- b: SMs of the memory job; k: lenders per borrower; c = 101 GB/s per-SM fetch cap.
- I: per-SM ingest ceiling. I = 127 GB/s staged-and-consumed (shared LMEM),
  253 GB/s write-only or with a separate read path, 101 if remote data shares the
  borrower's L2 ingress.
- r_co: per-SM rate of a memory job co-resident on a compute SM.
- P = 716 GB/s.

**Baseline** B(b) = min(b·c, P): b = 1: 101 (14% of P), 2: 202 (28%), 4: 404 (56%), 7+: saturated.

---

## Option 1 — Borrow idle DXA/TMA engines of other SMs

### 1a. Engine writes directly into the borrower's shared memory over a new SM-to-SM path

**Mechanism.**
1. A borrower warp issues a DXA instruction as today.
2. The descriptor-level request (slot, coordinates, LMEM address, barrier id,
   requester core id; ~32 B) goes to a lender's DXA core instead of the local one.
3. The lender's workers fetch through **the lender's** L2 port, at the lender's
   socket arbiter's lowest priority, and send the 128 B LMEM write beats over a
   group-level SM-to-SM switch to the borrower's LMEM DMA port.
4. Completion is detected at the borrower's LMEM, exactly as today
   (`VX_mem_unit.sv:129-148`), because the attributes travel with the data.

**Limit removed.** The borrower's single DXA port (64 B/clk) and its engine count.

**Hardware.**
- New, per group: a request crossbar (cores → DXA cores) and a data crossbar
  (DXA cores → LMEM DMA ports).
- Reused: the existing switch-by-core-id, generalized (`VX_socket.sv:817-850`),
  and `VX_mem_xbar`.
- Per SM: nothing new except one more input on the LMEM DMA arbiter.
- Static pairing: a DCR "lender list" per core. Dynamic: a credit counter per DXA core.

**Data path of one 16 KB transfer** (256 lines).
1. Borrower warp → DxaUnit.
2. Group request crossbar → lender `VX_dxa_req_arb` → dispatch → worker.
3. 256 line reads → lender unit MMU → lender socket port 0 → lender L2 link → L2 →
   (miss) DRAM → back on the lender's port 0.
4. Lender worker smem writer (1 beat/cycle) → group data crossbar → borrower
   `lmem_dma_arb` → borrower LMEM banks.
5. The last beat carries `notify_smem_done` → the borrower's barrier.

**What software sees.**
- A transparent version: the hardware picks an idle lender, so the ISA is unchanged.
- A descriptor-field version: an "allow remote" bit or a lender hint.
- The runtime/scheduler decides the lender set: the SMs of the other job.
- One transfer goes to one engine. A 64 KB stage can be split as 4 × 16 KB to 4
  engines, with the barrier expecting 4 arrivals.

**Interference with the lender.**
- The lender's port 0 carries the borrowed traffic at the lowest priority, so the
  lender's own icache/dcache always win.
- The lender's LMEM, registers and issue slots are untouched.
- The lender's DXA workers are shared. Bound them with a per-lender cap on borrowed
  workers or in-flight lines (a DCR).
- The compute job's L2 hit rate is protected by pseudo-LRU (`h200s`, candidate #15).

**Gain bound.** Per borrower min((1+k)·c, I).

| I | b = 2, k = 1 | b = 2, k ≥ 2 | Notes |
|---|---|---|---|
| 127 (staged consumer, shared LMEM) | 254 vs 202 | 254 | 1.26×, saturates at k = 1 |
| 253 (write-only / separate read) | 404 (2.0×) | 506 (2.5×) | needs k = 2 |
| 101 (remote data shares L2 ingress) | 202 | 202 | 1.0× |

So the best case is 2.5× at b = 2, and **only if the borrower can ingest 128 B/clk
and consume it.**

**Cost.**
- Area: two group crossbars, each 128 B wide on the data side. Small next to an SM,
  but the wiring scales with G².
- Hopper already pays for an SM-to-SM network in each GPC.

**What changes in Vortex.**
- RTL: `VX_socket.sv` (export the DXA request input and the LMEM output to the
  cluster), `VX_cluster.sv` (group crossbars), `VX_dxa_core.sv` / `VX_dxa_req_arb.sv`
  (remote inputs, borrowed-work cap), `VX_dxa_pkg.sv` (requester id in the
  request), `core/VX_mem_unit.sv` (one more DMA input or a shared one).
- SimX: `socket.cpp`, `cluster.cpp`, `dxa/dxa_core.cpp`, `dxa/dxa_unit.cpp`, and a new
  `DxaGroupNet`.
- Effort: SimX ~1–1.5 weeks; RTL ~2–3 weeks; parity cases in `dxa.yaml`.

**Risks.**
- The ingest ceiling (above).
- Barrier semantics when one stage is split across engines.
- Address translation: the lender's UTLB must translate the borrower's addresses.
  This is fine within one kernel; it needs ASIDs across processes.
- Deadlock: if the lender's LMEM DMA path and the borrower's both wait on each
  other. They are unidirectional here, so the risk is low.

**Closest prior work.**
- Hopper TMA with `.shared::cluster` destination (FACT: "the address dstMem can be in
  the shared memory of any of the CTAs within the current cluster", PTX ISA,
  https://docs.nvidia.com/cuda/parallel-thread-execution/index.html).
- TMA multicast and NVIDIA's multicast patent US12020035.
- The research agent found no paper that lends one SM's memory-request capacity to
  another. That gap needs one more check before the proposal is written.

### 1b. Engines stage into L2, and the borrower pulls

**Mechanism.** A lender DXA runs a **prefetch-only** transfer: read requests whose
responses are dropped, or better, an L2 prefetch command that fills the L2 and returns
only an ack. The borrower then reads the same lines with its own DXA/loads and hits
the L2.

**Limit removed.** In-flight-bound limits only. The borrower's round trip drops from
659 to 273 cycles. A port-bound limit is untouched.

**Hardware.**
- Lender DXA: a prefetch mode. The descriptor field `L2PROMO` is already reserved,
  2 bits, wired nowhere (`VX_dxa_pkg.sv:41-47`; design doc §7 item 5).
- L2: a prefetch request type that allocates without a core response.

**Data path.**
1. The lender issues 256 prefetches through its port 0.
2. L2 misses go to DRAM; fills land in the L2; acks return (no 64 B data on the
   lender's response path).
3. Later the borrower's 256 reads hit the L2 at 273 cycles and arrive on the
   borrower's port.

**Software.** A helper CTA, or the hardware runs "prefetch k tiles ahead". Timing
matters:
- The window must stay well under the L2 capacity (8.65 MB in `h200s`; 50 MB on H200).
- At 700 GB/s, a 4 MB lead is ~6 µs.

**Interference.** L2 capacity. Pseudo-LRU protects the compute job's hot lines only
partly against a deliberate prefetch stream. A cap on prefetched lines per set may
be needed.

**Gain bound in Vortex.**
- Plain loads are MSHR-bound: 512 × 64 B / 273 = 120 B/clk, limited by the 128 B/clk
  dcache, so ~230 GB/s per SM. That is **~2.3–3×** over the measured 77.
- DXA is port-bound: ≤ 127 GB/s per SM. That is **≤ 1.25×** over 101.
- On the real H200 the bound depends on E1.

**Cost.** Small: an L2 request type and a DXA mode.

**What changes in Vortex.**
- `VX_cache_bank.sv` / `cache.cpp`: a prefetch op.
- `VX_dxa_gmem_req.sv` / `dxa_core.cpp`: a mode that drops data.
- Effort: SimX 3–5 days, RTL 1–2 weeks.
- No new network.

**Risks.**
- If the per-SM cap is a port/ingress limit (E1), the gain is ≤ 1.25×.
- Timeliness: early prefetches are evicted, late ones are useless.

**Prior work.**
- Software: `cp.async.bulk.prefetch.L2` exists on Hopper (FACT from the PTX ISA; I
  did not re-read that section for this report).
- L2 prefetching is old. Cross-SM prefetch for another job's benefit is not something
  I found.

### 1c. Static pairing vs dynamic pool vs a per-group shared pool

- **Static pairing** (borrower b ↔ lenders L(b), set by the runtime through DCRs) is
  simplest. It is enough for a 2-SM first experiment (the user's plan).
- **A dynamic pool** sends each request to the least-loaded DXA core in the group.
  It needs load credits on the request crossbar. It keeps lenders even when the
  compute job's own DXA use varies.
- **A per-group shared pool** (Virgo-style; Vortex's own pre-`7d6950de5` cluster DXA):
  - One big DXA core per group with its **own** L2 ports, fed by every core of the group.
  - It removes the per-SM port binding entirely. The group uplink (P_g ports)
    becomes the cap.
  - It is the same idea as option 2.

## Option 2 — Elastic per-SM request machinery: a group-level pool of in-flight slots / MSHRs / workers / ports

**Mechanism.** Provision the same total resources differently. Each SM keeps a small
private share, and a group pool (MSHR entries, DXA workers, **and L2 ports**) is
allocated on demand.
- A memory-bound SM can then hold, for example, 4× its private in-flight lines and
  issue through the pool's ports.
- A compute-bound SM uses almost none of the pool.

**Limit removed.** Whichever of in-flight lines, engines or ports binds. **The answer
depends on which one it is.**
- Vortex DXA: the port. So a pool of workers alone does nothing; a pool must include
  ports.
- Vortex plain loads: the MSHR (Little). A pooled MSHR would help.
- Real H200: E1 decides between in-flight and port/ingress.

**Hardware.** Per group: pooled MSHR storage (a second-level MSHR in the group switch)
or a pooled DXA core with its own uplink ports. Per SM: a smaller private share.

**Data path (pooled DXA).**
1. SM → group pool request.
2. The pool fetches on group uplink port j.
3. Data → group data crossbar → SM LMEM.

The same crossbars as 1a. The difference is ownership: there is no "lender".

**Software.** Transparent. An optional per-SM priority or quota in DCRs.

**Interference.** The compute SMs' quota guarantees their share. The worst case is
the guaranteed minimum.

**Gain bound.** Per SM min(pool share, I). The same ingest ceiling as 1a. Group-level
cap P_g × 64 B/clk; with an H100-like GPC that is ~8 SMs' worth.

**Cost.** It can be area-neutral (same total entries), but it adds a level of
latency (+L_g) for every SM. That costs the compute job's misses a few cycles.

**What changes in Vortex.**
- Network (A) + (A+).
- The pooled DXA can be rebuilt from `7d6950de5^` (cluster DXA with
  `DXA_L2_GMEM_PORTS`).
- A pooled MSHR is a new structure in the group switch. That is hard: the MSHR is
  inside `VX_cache_bank`/`cache.cpp` today.
- Effort: pooled DXA 2–3 weeks over (A); pooled MSHR 4–6 weeks.

**Risks.**
- The extra latency for everyone.
- Fairness.
- Under the ingest ceiling, the same small gain as 1a.

**Prior work.**
- DL-MSHR (ICS'19, pooling within one SM,
  https://web.engr.oregonstate.edu/~chenliz/publications/2019_ICS_Dynamcially%20Linked%20MSHRs.pdf).
- A shared second-level MSHR across SMs (arXiv 2007.16175, https://arxiv.org/pdf/2007.16175).
- DC-L1 (HPCA'21, L1s decoupled from cores and shared in clusters,
  https://adwaitjog.github.io/docs/pdf/decoupledl1-hpca21.pdf).
- Virgo (ASPLOS'25, cluster-level matrix unit, built on Vortex, https://arxiv.org/pdf/2408.12073).
- Novelty risk: **medium.** Cluster-shared L1/MSHR has been published. Cluster-shared
  DMA for co-run bandwidth has not.

## Option 3 — Network-side: priority, more or wider ports for designated SMs, bypass paths

**Mechanism.**
- (3a) Arbitration priority for the memory job's requests at the L2 bank arbiters and
  memory controller.
- (3b) Designated "fat-pipe" SMs with 2–4× the L2 ports/ingress, statically
  (heterogeneous SMs) or by switching a shared pool of uplinks between neighbouring
  SMs (a reconfigurable TPC-level mux).
- (3c) A bypass from memory partitions to an SM, as a dedicated return path.

**Limit removed.** 3a removes contention, which is not our limit: the compute job uses
1.7% of the memory. 3b/3c remove the per-SM port/ingress, if that is the limit.

**Hardware.** 3a: arbiter weights. 3b: per-SM extra ports (Vortex: the DXA on all 4
socket ports, the existing SimX knob). 3c: new wires.

**Data path (3b).** DXA worker → socket ports 0..3 (behind the dcache on 1..3) → 4 L2
links → L2 → back on 4 ports → LMEM. No other SM is involved.

**Software.** Transparent. The scheduler places the memory job on fat-pipe SMs.

**Interference.** None with other SMs (3b). 3a can starve the compute job's misses;
bound it with weighted round-robin.

**Gain bound.**
- 3a ≈ 1.0× in this scenario. It matters for prefill+decode, where both are heavy.
- 3b: per SM min(4 × 64, I) B/clk. In Vortex the DXA would go from 101 to up to
  ~253 GB/s per SM (write-only) or ~127 (staged-consumed).
  - **Untested combination: 16 workers × 4 ports.** P0 runs it with zero code.
  - Measured 4 workers × 4 ports = 94.5 is engine-bound.

**Cost.** 3b in silicon: more crossbar ports per SM, which is expensive on a 132-SM
crossbar ("quadratic escalation of the crossbar costs", patent US12020035).
Heterogeneous SMs fix the split ratio at design time.

**What changes in Vortex.** For 3b the SimX knob exists; RTL: `VX_socket.sv:800-803`
`GMEM_OUT_PORTS > 1` plus per-port arbiters on ports 1..3 (about 1 week). 3a:
`VX_cache.sv:521-528` arbiter type and a request class bit (1–2 weeks with parity).

**Risks.** 3b is "just provision more", which a reviewer may call trivial. Its
research value is only in showing the cap moves. 3a does not address the problem.

**Prior work.**
- Many NoC-QoS papers.
- Kim et al. PACT'20: the NoC–memory interface is the bottleneck.
- MIG for isolation.

## Option 4 — Forwarding/relay through a neighbour, and what works with NO hardware change

### 4a. Hardware relay

A neighbour SM's warps (or its DXA) fetch into its own LMEM/L1, and a forward
operation moves the data SM-to-SM into the consumer's LMEM.
- It needs the lender's LMEM, a slot of it, and lender issue. That is strictly worse
  than 1a, which skips the lender's LMEM.
- It only makes sense if the lender also **uses** the data (computes a partial result),
  which is option 6.
- Closest prior work: Ibrahim et al., PACT'19, "Analyzing and Leveraging Remote-core
  Bandwidth" (https://adwaitjog.com/docs/pdf/remote-core-pact19.pdf). L1 misses probe
  other cores' L1s over a mesh: +10–21%. It exploits **locality in remote L1s**,
  not spare request capacity. It is not the same idea, but reviewers will cite it.

### 4b. Software-only on today's hardware (the baseline a reviewer asks for)

**(i) Cluster-TMA borrowing (Hopper/Blackwell).**
- The decode kernel is launched with thread-block clusters of size 2–8. CTA 0
  consumes; CTAs 1..k are helpers with one thread each and ~0 shared memory. They
  issue `cp.async.bulk(.tensor)` with `.shared::cluster` destination = CTA 0's
  buffer and CTA 0's mbarrier.
- This is allowed (FACT, PTX ISA, quoted above). The helpers land on other SMs of
  the same GPC.

Constraints:
- Same grid and same GPC (FACT).
- Each helper needs a CTA slot, a warp and registers on the lending SM. A CUTLASS
  Hopper GEMM with 384 threads and ~64K registers per SM may leave **no** room.
  INFERENCE; check with the real Z-Image kernels' occupancy.
- Under MPS the helpers may land on the compute job's SMs (MPS allows different
  clients' CTAs on one SM, FACT, https://docs.nvidia.com/deploy/mps/pdf/CUDA_Multi_Process_Service_Overview.pdf).

**This is E2. It measures the consumer SM's ingest under borrowing on real silicon,
which is the single most important number for the whole idea.**

**(ii) Helper L2 prefetch.** Helper warps on other SMs issue
`cp.async.bulk.prefetch.L2` (no shared memory, no registers) k tiles ahead of the
decode SMs. **Does an L2 hit raise the per-SM limit?**
- In Vortex: yes for plain loads (MSHR-bound, ~2.3×), barely for the DXA (port-bound,
  ≤ 1.25×).
- On the real H200: **unknown. There is no L2-resident per-SM measurement in
  `results_gpu/`.** E1 answers it.

**(iii) A copy kernel on the compute SMs** (MPS). The compute job's SMs run a tiny
copy kernel moving weights into... nowhere useful. A copy to global memory does not
help the consumer. Only (i) or (ii) make sense.

**(iv) Simply give the decode job more SMs at low occupancy.** See option 6.

**Software-only gain bound.**
- (i): min((1+k)·c, I_real) per consumer SM. I_real is unknown, which is why E2 matters.
- (ii): per SM c × (659/273) if in-flight-bound, else ≤ 1.25×.

**What it costs us.** 2–4 days of CUDA (`gpu/tmacopy.cu` is the starting point).

## Option 5 — Move the data instead of the requests: push from L2 or a global engine

**Mechanism.** A stream engine at the L2 (one per L2 partition or group) is armed
with a TMA-like descriptor and a destination (SM, LMEM address, barrier). It reads
lines from the L2/DRAM using L2-side MSHRs (256 per bank in `h200s`) and **pushes**
them as write packets over the response network into the destination SM's LMEM.
A variant keys a memory-controller stream prefetcher by the descriptor.

**Limit removed.** All SM-side request limits: issue, in-flight lines, engine and the
SM's request port. Only the SM's response ingress remains. In Vortex that ingress is
**4 response ports**, while the DXA uses only port 0 today.

**Hardware.**
- Global/L2-side: a descriptor engine and a push packet type.
- Per SM: the socket must route push packets arriving on ports 0..3 to the LMEM DMA
  port. There is no path from socket response ports to LMEM today, so this is new.

**Data path.**
1. The consumer warp writes a "push descriptor" (via its DXA instruction with a
   "remote engine" bit) → request travels to the L2 engine.
2. The engine reads 256 lines from banks / DRAM.
3. Responses are tagged "push to core c, LMEM address a" → L2 response network →
   socket port (any of 4) → new socket push router → LMEM DMA port → barrier.

**Software.** A descriptor-field or engine-selection bit. The runtime has to decide
nothing else.

**Interference.** L2 bandwidth and MSHRs are shared with the compute job. Bound them
with a cap on engine MSHR use.

**Gain bound.** Per SM min(4 × 64 B/clk, I) in Vortex. Identical ceiling to 1a/3b,
without consuming any other SM's resources. This is the cleanest "data moves, not
requests" design. On NVIDIA it is akin to the L2 request coalescer that already
serves multicast (`lrc`).

**Cost.**
- L2-side engine: about one DXA core per L2 group.
- A push router per SM.
- No SM-to-SM network.

**What changes in Vortex.**
- `VX_cluster.sv` (engine instance), `VX_cache_wrap`/bank interface (an
  engine-injected read port, or the engine as one more L2 client), the response tag
  format, `VX_socket.sv` (push router to LMEM), and the matching SimX files.
- Effort: SimX ~1.5–2 weeks, RTL ~3–4 weeks.

**Risks.**
- A new packet class on the response path (ordering, deadlock with normal responses).
- The ingest ceiling.
- Edge GPUs may not have an L2-side engine budget.

**Prior work.**
- D2MA (PACT'14, hardware global→shared DMA, https://cccp.eecs.umich.edu/papers/jamshidi-pact14.pdf).
- DAC (ISCA'17).
- NVIDIA's multicast LRC path.
- CudaDMA (SC'11) as the software analogue.
- Novelty risk: **medium.** "L2-side push into shared memory for a co-running job" is
  not something I found, but DMA-into-scratchpad is old.

## Option 6 — Scheduling-level alternatives without new hardware (may undercut the hardware idea)

### 6a. Time-slicing

Give the decode kernels all SMs for their short bursts (7–9 ms of GPU time per Qwen
token step, against a 150 ms DiT step), then return the SMs.
- Decode then runs at the full-SM rate (65–68% of peak), but the compute job stops
  during those bursts.
- Over one DiT step with ~17 decode steps interleaved, that is ~140 ms of paused
  compute, i.e. no overlap gain.
- With preemption costs it is worse than space-sharing for both jobs.
- It is the right baseline only if decode latency matters more than throughput.

### 6b. Co-residency: the memory job's CTAs on every SM, beside the compute job's CTAs

A decode kernel needs very little compute per byte:
- BF16 GEMV at M = 4: 4 FLOP/byte. At 100 GB/s per SM that is ~0.4 TFLOP/s, ~5%
  of an SM's dense BF16 tensor rate (~7.5 TFLOP/s per SM).
- Padded to an n = 8 tile it is 8 FLOP/byte, ~11%.
- On FP32 CUDA cores at M = 4 it would be ~80% of the SM's FMA peak (507 GFLOP/s), so
  the consumer must use tensor cores.

So spreading the memory job thinly over all SMs uses each SM's **R and a sliver of S
and C**. Nothing has to move between SMs, and the ingest ceiling never binds.
- Real-GPU support: MPS clients share SMs (FACT). Within one process, streams already
  co-run.
- Green contexts partition SMs, not bandwidth (FACT,
  https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/green-contexts.html).
- POD-Attention (ASPLOS'25) runs prefill and decode on the same SM.

Our H200 data (`results_gpu/h200_corun.txt`, `gpu/corun.cu`):
- A 1024-thread plain-load stream block sharing an SM with a 1024-thread register-only
  FMA block falls from 50.8 to 14.7 GB/s (0.29×).
- The FMA kernel slows by 3% (63.4 → 65.2 ms).
- On separate SMs both keep 1.00 (`h200_corun_sep.txt`).
- The likely cause is warp-scheduler starvation of the load warps by always-ready
  FMA warps. INFERENCE, not verified.

Gain bound on `h200s`: B = min(b·c + (22 − b)·r_co, P).
- Plain loads with r_co ≈ 15–29 GB/s per compute SM (0.29× of one or two stream
  blocks): b = 2 → 202 + 300–580 = **500–716 GB/s, 2.5–3.5×**.
  - That is more than 1a's best case, and achievable today.
- TMA-based co-residency issues one instruction per 16 KB, so issue starvation should
  mostly vanish. If r_co approaches c, the memory saturates with any b. **Unmeasured (E3).**

Where it fails, and where hardware comes back:
- (1) **Residency.** A production compute GEMM (CUTLASS Hopper warp-specialized) can
  fill the register file and up to 227 KB of shared memory, leaving no room for a
  decode CTA. Then 1a (borrowing without residency) is the only way to use those SMs' R.
- (2) **Interference.** Issue slots (0.29×), shared-memory capacity, and tensor-core
  slots for the consumer.
  - Hardware fix, cheap: an issue-priority class for producer/memory warps, and a
    reserved small LMEM slice per SM for a guest job.
  - Call it a **"memory lane"**: a per-SM reservation of 1 warp, 32–64 KB of LMEM and
    the DXA, managed by the scheduler.
- (3) **Cross-process co-residency on edge GPUs.** Vortex runs one kernel at a time;
  emulate it with role-by-CTA in `gemm_block`.

**6c. More SMs, fewer warps each.** Equivalent to 6b when the other job is idle.
Against a compute job it is 6b by another name.

**Honest assessment.**
- If E3 shows a TMA stream co-resident with a compute kernel keeps ≥ 0.7× of its
  solo rate at ≤ 5% compute loss, software co-residency gets most of the available
  gain.
- The hardware contribution must then be framed as (1) residency-free borrowing, or
  (2) isolation for co-residency (the memory lane), not as "more bandwidth".

**What changes in Vortex.** Kernel only: `gemm_block` with roles assigned per CTA
within each core instead of per core. About 1–2 days. Hardware 6b (issue priority +
reserved LMEM) is ~1 week SimX, ~2 weeks RTL.

## Option 7 — The edge-GPU angle (Qualcomm)

Edge parts differ from the H200 in five ways:
- **LPDDR5X** at 77–135 GB/s (Snapdragon 8 Gen 3 ~77 RUMOR-grade; X Elite 128-bit
  LPDDR5X ~135).
- **Higher DRAM latency**, ≥400 ns on Adreno 690 (INFERENCE).
- **Few cores** (Adreno X1: 6 SPs, so a fair share of ~22 GB/s per SP).
- A **system-level cache** of 6–8 MB shared with the CPU and NPU.
- **Unified memory**.

**Per-core cap.** Little's law: needed in-flight bytes per core = BW × latency. At
400 ns, one core needs 40 KB in flight for 100 GB/s, more than an H200 SM.
- If edge cores provision in-flight capacity like desktop SMs, **one core may pull only
  ~1/3–1/2 of LPDDR**. A decode job on 1 of 6 cores then leaves memory idle. The
  problem survives on edge, in proportion.
- But this is unmeasured. Per-SP DRAM bandwidth on Adreno is published nowhere I found.
  Measure it with an OpenCL stream on a Snapdragon X laptop, restricted to one SP if
  the driver allows (open question).

Options that still make sense on edge:
- **6b co-residency** (cheapest, no SM-to-SM wires).
- **1b/4b L2/SLC prefetch.** A 6 MB SLC gives ~45 µs of lookahead at 135 GB/s; the
  latency is high, so an in-flight-bound core gains the most from hits.
- **5 push from the SLC/L2 side.**

Options that make less sense on edge:
- 1a needs an SM-to-SM network that small GPUs do not have. Adreno clusters share
  cluster caches, so "group" = cluster cache is natural.
- 3b (fat ports) is cheap on a 6-core crossbar, so it is plausible on edge.
- The NPU may take decode entirely. The proposal should say why the GPU still runs it:
  the model or operator is not supported on the NPU, or the NPU is busy.

---

## 2.8 Comparison

Gain is the upper bound on `h200s` with b = 2 memory SMs (baseline 202 GB/s, P = 716),
under the ingest ceilings I = 127 (staged) / 253 (write-only); "real" = what decides it
on the part.

| Option | Limit attacked | Hardware cost | Software visibility | Max gain (b = 2) | Interference | Vortex effort (SimX / RTL) | Novelty / scoop risk |
|---|---|---|---|---|---|---|---|
| 1a remote DXA → borrower LMEM | borrower port + engines | group req + data crossbars | none or a descriptor bit; runtime picks lenders | 1.26× / 2.5×; 1.0× if ingress shared | lender port at lowest priority, worker cap | 1–1.5 wk / 2–3 wk over (A) | software version exists on Hopper (cluster TMA); hardware residency-free version not found: medium |
| 1b lender L2 prefetch | in-flight (latency) | L2 prefetch op, DXA mode | helper or mode bit | loads ~2.3×, DXA ≤ 1.25×; real: E1 | L2 capacity | 3–5 d / 1–2 wk | software `cp.async.bulk.prefetch.L2` exists: high |
| 2 group pool (MSHR / DXA + ports) | in-flight or port, by provisioning | pooled structures, +latency | transparent | as 1a; group cap P_g | quota | 2–3 wk / 3–6 wk | DC-L1, shared L2-MSHR, Virgo: medium–high |
| 3a priority | contention | arbiter weights | transparent | ~1.0× | can starve compute | 2 d / 1–2 wk | crowded, and does not address the problem |
| 3b fat-pipe SMs | per-SM port | more ports per SM | scheduler placement | up to I | none | 0 (knob) / 1 wk | trivial-looking |
| 4a relay via neighbour LMEM | port | SM-to-SM path + lender LMEM | helper code | ≤ 1a | lender LMEM + issue | as 1a | PACT'19 remote-core: medium |
| 4b software only (cluster TMA, L2 prefetch) | port / in-flight | none | fused kernel, clusters, helpers | real: E1/E2 | lender CTA slot | 2–4 d CUDA | it is the baseline |
| 5 L2-side push | all SM-side request limits | L2 engine + push router | descriptor bit | up to I (4 ports) | L2 MSHRs | 1.5–2 wk / 3–4 wk | D2MA / DAC / LRC: medium |
| 6a time-slicing | — | none | scheduler | decode full-rate, compute paused | total | 0 | baseline |
| 6b co-residency (software) | spreads R over all SMs | none | MPS / streams / fused | 2.5–3.5× with loads (measured r_co); more with TMA (E3) | issue slots (0.29×), LMEM capacity | 1–2 d kernel | POD-Attention, MPS: it is the baseline |
| 6b+ "memory lane" hardware | isolation for 6b | warp-priority class, reserved LMEM | runtime reservation | as 6b at ~0 interference | bounded by reservation | 1 wk / 2 wk | Warped-Slicer, SMK (partitioning in an SM): medium |
| 7 edge | as above | — | — | unknown: per-SP cap unmeasured | — | — | — |

## 2.9 Recommended order of exploration

### Step 0 — four measurements that can kill options (about one week in all, no new hardware)

| Id | What | Where, how | Reading | Kills |
|---|---|---|---|---|
| E1 | Per-SM bandwidth, L2-resident (≤ 20 MB buffer, re-read) vs DRAM-resident, plain loads and TMA, 1 SM | H200; `gpu/bwsweep.cu` and `gpu/tmacopy.cu` with a small `--gib` and repeats | L2 ≥ 2× DRAM → in-flight-bound; ≤ 1.25× → port/ingress-bound | 1b/4b(ii) and the MSHR part of 2 if port-bound; 3b and 5 lose their premise if in-flight-bound |
| E2 | Cluster-TMA borrowing: cluster size 2/4/8, CTA 0 consumes (reads every word, i.e. staged), helpers TMA into CTA 0 via `.shared::cluster` | H200; new ~200-line kernel from `tmacopy.cu` | CTA 0 ingest vs k; with and without CTA 0 reading the data | 1a/2/4/5 on NVIDIA-like ingress if ingest stays ≈ 101 at k ≥ 1; if it scales, borrowing works in software today and the hardware story must be residency-free borrowing |
| E3 | TMA copy (1 thread) co-resident with the FMA kernel and with a CUTLASS GEMM (and the plain-load stream again) | H200; `gpu/corun.cu` + `tmacopy.cu`, MPS | stream rate vs solo; compute slowdown; whether the GEMM leaves room for the helper CTA | if r_co ≥ 0.7 c at ≤ 5% compute loss, the "more bandwidth" framing of the hardware idea is weak |
| P0 | (a) `gemm_block` consumption roof: weights already in LMEM, no memory traffic, 1 SM; (b) DXA copy with `NUM_DXA_CORES=16`, `DXA_GMEM_PORTS=4` on 1 SM; (c) L2-resident `bwstream -b32` and `bwstream_dxa` on 1 SM | SimX `h200s` | (a) below ~250 GB/s → fix the consumer first (tensor-core issue, staging), or every fetch-side result is a copy-only result; (b) shows whether Vortex's cap is just one port; (c) is the Vortex counterpart of E1 | — |

### Prototypes in SimX, smallest experiment first

1. **P1: option 1a with static pairing, 2 SMs** (borrower + 1 lender, then 1 + 3).
   - Needs (A+) only for a group of 2–4, SimX only (~1.5 weeks).
   - Experiments: `bwstream_dxa` (write-only) **and** `gemm_block -s1` after P0(a)
     (staged-consumed). Pair with the load-mode runs as the study rule requires.
   - Run the ingress knob both ways.
   - Kill: staged-consumed gain < 1.3× with the separate-ingress setting.
2. **P2: option 6b co-residency, software** (`gemm_block` roles per CTA within each
   core, 1–2 days).
   - The baseline P1 must beat.
   - Kill for P1: P2 reaches ≥ 80% of P1's memory-job rate with ≤ 5% compute-role
     loss, and compute kernels leave residency room (E3).
3. **P3, only if E1/P0(c) show an in-flight limit: option 1b** (lender L2 prefetch via
   `L2PROMO`, ~1 week SimX).
   - Kill: borrower DXA L2-hit rate ≤ 1.25× DRAM rate.

**What tells the three apart in one experiment.** Use 2 memory SMs + 20 compute SMs on
`h200s`, with three variants: P1 (borrow 1 lender each), P2 (co-reside), P3
(prefetch). Run each with the copy and with the staged decode GEMM. The six memory-job
rates and the compute-role slowdowns rank them directly.

**Go to RTL** only for the winner, after the minimal network (A/A+) is in RTL with
parity. Estimate 3–5 weeks to an rtlsim-verified prototype of one mechanism.

### What the hardware idea still adds if software gets most of the gain

- (1) It uses the compute SMs' request machinery **without** a resident CTA. That is
  the case for production GEMMs that fill registers and shared memory.
- (2) **No issue-slot interference.** The 0.29× effect disappears.
- (3) **Reach across GPCs and processes.** The software cluster path is limited to one
  grid and one GPC.
- (4) **Transparency.** No fused kernels or cluster launches are needed.
- (5) On edge GPUs, without TMA multicast/DSMEM, it may be the only path.

The proposal should lead with the measured E2/E3 numbers, and frame the hardware as
whatever remains between software and the memory roof.

---

## 9. Unverified or open

1. NVIDIA's per-SM cap: in-flight capacity vs ingress port (E1). The Little's-law
   in-flight figure is 34 KB unloaded (my arithmetic at 333 ns), or 60–80 KB at the
   research agent's assumed 600–800 ns.
2. Whether TMA/DSMEM writes and wgmma/ld.shared reads share the 128 B/clk shared-memory
   banks on NVIDIA. Whether the DSMEM receive port shares the L2-fill ingress (RUMOR).
3. The internal topology of the TPC/CPC/GPC switches and the crossbar (not published).
   Exact L2 slice count of H200 (inferred ~80 as H100). B200's partition count
   (sources disagree).
4. Why the co-resident stream fell to 0.29× (scheduler policy is a guess).
5. Whether a CUTLASS/cuBLAS Hopper GEMM in the Z-Image step leaves room for a helper CTA.
6. Per-core DRAM bandwidth on Adreno / Mali / Apple GPUs (nothing published found).
7. That no paper lends one SM's memory-request capacity to another. One agent search
   found none; repeat it before the proposal.
8. `cp.async.bulk.prefetch.L2`: named from memory of the PTX ISA, section not re-read.
9. Vortex details not run, only read:
   - the L2 latency change with fewer L2 inputs (arbiter tree levels);
   - the cost of a DXA LMEM access in LMEM cycles (whether a DMA write blocks passes
     for one or two cycles; `VX_lmem_unit.sv:30-32`);
   - that DCR writes reach every socket's DXA descriptor table identically
     (`VX_dcr_arb`, `VX_cluster.sv:276-284`, broadcast semantics not re-checked).
10. The "5% / 10% of SMs" decode points are probably 8 / 14 SMs (MPS granularity, see 2.0).
11. Stale items found while reading:
    - `VX_CFG_DXA_MEM_PORTS` (`VX_config.toml:443`) is unused.
    - `VX_CFG_NUM_DXA_UNITS` is passed by 10 `dxa.yaml` cases and defined nowhere.
    - `docs/designs/dxa_async_copy_multicast.md` §7 item 1 still says the DXA is cluster-scoped.
    - Worth adding to `upstream-pr-candidates.md` (not done; read-only task).
