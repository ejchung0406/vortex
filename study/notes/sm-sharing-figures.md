# Figure specifications: sharing an SM without starving the memory job

Companion to `sm-sharing-design-space.md`; numbers come from there.

The specifications are written for a later drawing pass (TikZ or SVG).

**Conventions, used in every figure:**
- **Node shapes:**
  - rounded box = hardware unit;
  - plain box = storage (SRAM, register file, queue);
  - ellipse = software entity (kernel, warp, CTA);
  - cylinder = DRAM / L2.
- **Colours:**
  - the compute tenant ("lender") = blue;
  - the memory tenant ("lane") = orange;
  - shared or neutral = grey;
  - **red outline** = new in that mechanism;
  - **dashed red** = the contended point being fixed.
- **Edges:**
  - solid = data;
  - thin = control or credit;
  - double = an issue grant;
  - labels give rate (GB/s or B/clock) or share (% of slots).
- One SM is always drawn the same way:
  - 4 SMSP columns (warp scheduler -> dispatch), left;
  - MIO/LSU queue -> L1/shared memory (128 B/clock, 32 banks drawn as 4 groups), centre;
  - TMA engine and L2 ingress port (~110 B/clock), right;
  - tensor core under the SMSPs.

---

## F0. Starvation anatomy (the motivating figure)

**Purpose.** Show that two phenomena exist, where each one bites, and which measurements give each away.

**Layout.** Three panels side by side.

**Panel (a) — the SM, with the eight candidate contended resources R1-R8 tagged.**
- Nodes:
  - SMSP0-3, each holding warp slots: blue FMA warps and one orange lane warp in SMSP1;
  - warp scheduler (per SMSP);
  - MIO queue, LG queue;
  - L1/shared-memory banks;
  - TMA engine;
  - L2 ingress;
  - register file (65,536 × 32 b), drawn as a bar 98% blue (cuBLAS 64,512) with a 1,024-register grey sliver;
  - shared memory (228 KB) as a bar: 189 KB blue, ~38 KB grey;
  - GPC egress -> L2 -> HBM (cylinders).
- Edges:
  - lane warp -> scheduler (double edge, labelled "picked once per ~60 cycles = 1.7% of SMSP");
  - FMA warps -> scheduler (double, "IPC 4.00 per SM");
  - TMA -> L2 ingress -> shared memory ("11.1 GB/s per SM co-run; 101-205 solo");
  - neighbour LDG -> MIO/L1 -> probe LDS (dashed red, "LDS 121 -> 1,500 cycles, ×12").
- Tags R1-R8 as small grey circles on each resource. R1 is in red, labelled "established (FMA lender)". R2/R3 are amber: "established for plain-load neighbours; TMA untested". R6 is amber: "gate for production GEMMs".
- Footnote under the panel: "beside a tensor-core GEMM: not measured (X0)".

**Panel (b) — evidence matrix.**
- Rows: the 11 signatures of §1.3, short labels:
  - lender unaffected;
  - starved not slowed;
  - order × ILP;
  - rate ∝ 1/warps;
  - depth independence;
  - instructions per transfer;
  - plain loads hurt the lender;
  - shared memory ×12;
  - MEM-first 10% loss;
  - block interleaving beside cuBLAS;
  - residency leftovers.
- Columns R1-R8.
- Cells: ● consistent, ○ inconsistent, blank = no information.
- R1's column is filled for rows 1-6. Row 10 points to R6 (phenomenon A).

**Panel (c) — the two phenomena, timelines of 4 SMs over ~5 ms.**
- Top, "A: block-scheduler interleaving (cuBLAS + 65 KB MEM blocks)". SMs alternate whole bars blue/orange; caption "GEMM 0.25, MEM 0.63-0.78, never on the same SM".
- Bottom, "B: warp starvation (FMA + 1-warp TMA)". Every SM is blue and carries thin orange ticks (one TMA issue per 2,920 cycles); caption "MEM 0.6-6.7%, FMA 0.985-1.000".

**What is new.** The separation of A and B, and the quantified per-SM shares.

**Inset plot (optional).**
- MEM GB/s vs FMA warps (8, 16, 24, 32), ILP 4, CMP first: 60.9, 37.8, 24.5, 16.4.
- Overlay a fit c/n (c ≈ 550).
- A second series, MEM first ILP 4: 40.8, 60.0, 42.6, 12.1 (no 1/n trend). It shows the policy is not yet understood.

---

## F1. The lane's budget per SM

**Purpose.** Show the asymmetry: tiny issue and TC need, a large buffer.

**Layout.** Horizontal stacked bars, one per SM resource, each 0-100% of capacity:
- issue: 0.7% producer + ~2.5% consumer (orange), rest blue/grey;
- tensor-core time: 4-8% (orange, hatched range);
- shared-memory port: 14% landing + 14% re-read (orange; re-read hatched "only if staged");
- L2 ingress: 35 of ~210 GB/s (17%), plus the GEMM's ~57 GB/s (blue, hatched "INF");
- registers: 1,024-6,000 (1.6-9%);
- buffer: three bars:
  - "one-stage TMA: 51-86 KB" (22-38% of 228 KB);
  - "two-stage (L2 prefetch): 27-34 KB" (12-15%);
  - a vertical marker "cuBLAS leaves ~38 KB".

**Annotations.**
- "35.1 GB/s = 17.7 B/clock = fair share of 4.63 TB/s over 132 SMs".
- A batch note: "valid for decode B ≲ 16; B = 64 needs ~39 SMs of tensor throughput".

**What is new.** The Little's-law buffer bar and the ~38 KB marker: the argument for g1.

---

## F2. Mechanism (a): issue-share floor ("memory lane" bit + token bucket)

**Nodes, one SMSP enlarged:**
- warp slots W0..W15 (blue) and W_lane (orange, with a red "L" bit);
- the existing greedy selector (grey);
- **new:** a credit counter (red outline; "+s per cycle, depth B, s = 1/16");
- **new:** the OR of "lane ready" (red);
- **new:** a 2:1 override mux (red) between the selector and dispatch.

**Edges:**
- ready bits -> selector;
- lane-ready + credit ≥ 1 -> override select (thin red);
- the override mux -> dispatch (double edge, "lane gets ≤ s = 6.25% of slots when ready");
- the credit decrements on a lane issue (thin).

**Side box: "who sets it".** Driver -> QMD/launch attribute -> per-CTA lane bit; optional `setlane` instruction.

**Labels.**
- "Lender bound: ≥ 1 − s".
- "A lane producer at 49 instructions per 32 KB needs 0.66%".
- "s = 1/16 supports up to 81 GB/s per lane".

**What is new.** A floor, not a cap: contrast it in a small inset with Wang et al. ISCA'17, an epoch quota capping the others.

**Vortex callouts (small grey text):**
- `VX_warp_sched.sv:377-398` (select);
- `VX_scoreboard.sv:344-354` (issue arbiter, sticky matrix);
- SimX `scheduler.cpp:203`, `core.cpp:911`.

---

## F3. Mechanism (b): residency carve-out

**Nodes:**
- CTA dispatcher (grey) with **new** reserve registers {R_lane, S_lane, W_lane} (red);
- SM resources as three bars: register file, shared memory, warp slots.

**Bars.** Blue = lender occupancy computed against (total − reserve); orange = lane CTA admitted only into the reserve; red dashed line = reserve boundary.

**Two variants drawn:**
- (i) one-stage: S_lane = 51-86 KB. It overlaps the cuBLAS 189 KB, shown as a red "does not fit" overlap.
- (ii) two-stage: S_lane = 34 KB, which fits (189 + 34 = 223 ≤ 227).

**Edge.** Occupancy API -> compiler (thin, "occupancy computed on reduced budget").

**What is new.** The intra-SM reserve, which Green Contexts and MIG do not provide.

**Callouts:**
- Vortex `VX_cta_dispatch.sv:482-502, 600-607` (one stride per kernel today);
- no register budget (`VX_opc_unit.sv:49`);
- one kernel at a time (`VX_cp_launch.sv:7-17`).

---

## F4. Mechanism (c): autonomous stream engine with descriptor ring

**Nodes:**
- **new** stream-context registers (red): base, bytes, tile/tensor map, ring base, N slots, full[]/empty[] addresses, L2 lead d;
- **new** sequencer FSM (red);
- **new** credit counter (red);
- existing TMA/DXA address generator and request queue (grey);
- L2 ingress;
- the ring in shared memory: N slots, orange;
- full/empty mbarriers;
- the consumer warp (orange ellipse) -> tensor core.

**Edges:**
1. Driver/kernel -> stream context, once ("stream.start", thin).
2. Sequencer -> request queue ("next tile when credit > 0"), no warp involved.
3. L2 -> ring slot i (data, "35 GB/s").
4. Completion -> full[i] (thin).
5. Consumer waits full[i], reads, arrives empty[i] -> credit +1 (thin red loop).

**Labels.**
- "Producer issue: 0".
- "Consumer issue: ~2-3% (mma.sync) or ≪ 0.1% (single-thread tcgen05-style)".

**What is new.** The self-sequencing loop and the credit return. Contrast panel: Hopper today, a producer warp issuing every TMA, which is CudaDMA in hardware form but still a warp.

**Vortex callouts:**
- `VX_dxa_core.sv:55-85` (insert the sequencer before the request arbiter);
- `VX_dxa_desc_table.sv:38-45` (descriptor table exists, written by the host);
- `VX_dxa_completion.sv:28-31` -> txbar (add a credit-return input).

---

## F5. Mechanism (c1)/(c2): who computes the memory job's math

**Two sub-panels sharing the engine of F4.**

**(c1) Private buffer + tensor core.**
- **New** 32 KB SRAM landing buffer (red), next to the tensor core, outside the shared memory.
- **New** operand mux on the tensor core (red): shared memory / private buffer.
- The consumer warp issues MMAs with operand A from the buffer.
- Label: "lender's shared memory and port untouched".

**(c2) TC backfill.**
- No consumer warp.
- **New** background MMA sequencer (red), feeding:
  - **new** accumulator context (8-16 KB, red);
  - **new** activation buffer (4 KB, red: one K-chunk × M = 16).
- The TC arbiter (red): lender first, background fills idle cycles, minimum share m.
- Edge labels: "lane needs 4-8% TC time"; "lender keeps ≥ 1 − m".
- Caption: "depends on the TC idle time of a tuned GEMM (unmeasured)".

**What is new.** A second tenant context on the tensor core.

---

## F6. Mechanism (d): fetch on shared SMs, compute on the memory job's own SMs

**Nodes:**
- 8 GPC boxes. Each holds ~3 orange consumer SMs and ~13 blue lender SMs, every lender SM with a small orange engine.
- GPC egress (≤ 1.28 TB/s each);
- L2 / HBM.

**Edges:**
- lender-SM engines -> L2 -> their GPC's consumer SMs over the SM-to-SM path ("75 GB/s per helper (MEAS)");
- consumer ingest marked with a red cap ("≤ 215 write-only / 157-170 staged (MEAS)").

**Callout box.** "Consumers needed for 4.63 TB/s: 22-29 vs disjoint ~32 → saves 3-9 SMs".

**Dashed red** on the cross-GPC link: "not available today (clusters ≤ 1 GPC)".

**What is new.** Cross-SM write from a lane engine. The figure shows why the ingest cap bounds the gain.

---

## F7. Mechanism (e): time multiplexing

**Two timelines, one SM, 0-40 µs.**
- **(e1) context-switched:**
  - blue compute slices of ~30 µs;
  - grey switch bars of 1.8 µs each way (red label "445 KB state at 128 B/clock");
  - orange memory slices of ≥ 16% duty, needing 215 GB/s bursts, which hits the ingest cap;
  - red X: "≤ 0.84 compute, no better than disjoint".
- **(e2) resident micro-slices:**
  - blue background with an orange 64-cycle window every 1,024 cycles (P, w labelled);
  - the lane issues a batch of D transfers per window;
  - label: "buffer +18 KB; lender ≥ 1 − w/P = 0.94".

**What is new.** (e2) as a timer-based variant of (a): one timer per SM instead of per-SMSP counters.

---

## F8. Mechanism (f): shared-memory / LSU isolation

**Nodes:**
- the MIO/LSU input queue, split **new** into two virtual channels (red): lender VC, lane VC;
- **new** round-robin arbiter (red);
- L1/shared-memory banks;
- DMA landing port (from TMA/DXA) with a **new** token-bucket limiter (red, "≤ x% of port cycles while LSU requests pending");
- option (f2): a separate SRAM macro for the lane ring (red, same as c1).

**Edges:**
- neighbour LDG and probe LDS into the queue: today a single FIFO with a red dashed head-of-line block, "×12";
- after: two VCs.

**Vortex inset.** `VX_lmem_unit.sv:506-515` "pass_en = ~dma_valid": the DMA has absolute priority over LSU passes. Label "Vortex today: the lane's landing writes block the lender's shared-memory loads ≥ 26% of cycles".

**What is new.** Per-role VCs plus a DMA rate limiter.

**Prior-work tag.** Dai et al. HPCA'18 (in-flight memory-instruction limits).

---

## F9. Mechanism (g1): two-stage fetch

**Nodes:** HBM (cylinder), L2 (cylinder, an orange "prefetched window" band of 4.6-7 MB chip-wide), SM ring (orange), consumer.

**Edges:**
1. Engine -> HBM -> L2: `prefetch.L2` d tiles ahead (thin orange, "DRAM latency 1-2 µs hidden in L2").
2. Engine -> L2 -> SM ring: "pull, L2 latency ~0.3-0.5 µs".

**Side-by-side bars:** ring size "one-stage 51-86 KB" vs "two-stage 27-34 KB", with the cuBLAS-leftover marker at ~38 KB.

**Risk callout.** "Lender evicts prefetched lines → use evict_last hints; X7 measures survival".

**What is new.** Moving the in-flight bytes from the SM to the L2, so the lane fits beside production GEMMs. **Software-testable on H200 today.**

---

## F10. Pareto frontier (the headline result figure)

**Axes.** x = memory-job TB/s (0-4.7); y = compute job's fraction of its solo all-132-SM throughput (0-1.05).

**Series:**
- **Disjoint spread (MEAS)**, line through:
  - (1.58, 0.94), (3.01-3.04, 0.88), (4.03-4.05, 0.82), (4.50, 0.76), (4.53, 0.64);
  - labels "8/16/24/32/48 MEM SMs".
- **Disjoint packed (MEAS):** (1.28, 0.88), (2.28, 0.76).
- **Shared today (MEAS):**
  - "MEM first, ILP 2": (4.33, 0.86), star;
  - "ILP 4": (0.15, 1.00).
  - Annotation "+8-9 points over disjoint, luck-dependent".
- **Beside cuBLAS (MEAS, phenomenon A):** chip-level points from e3 (132 MEM blocks). MEM 2.92 / 2.40 / 3.22 TB/s with the GEMM at 0.25-0.26, plotted with a distinct marker and caption "SMs alternate, no co-residency".
- **Lane (SimX, to come):** band from optimistic (4.5, 0.95-0.97) to pessimistic (4.5, 0.86-0.88).
- **Type-D lender (measured DiT):** a grey dotted "disjoint" curve, flat ~0.97-1.0 down to 24-32 MEM SMs. Caption "headroom ≈ 0 for SM-insensitive lenders".

**What is new.** The gap between the lane band and the disjoint curve at equal TB/s. Show it honestly beside the type-D curve.

---

## F11. Go/no-go experiment X0 (methods figure)

**Nodes:**
- one SM, with a blue DiT-shaped wgmma GEMM CTA (registers and shared memory drawn to scale from the kernel's resource usage) and an orange lane CTA (1 warp, 32 registers, 2 × 16 KB ring) in the leftovers;
- HBM;
- the instrumentation: `%globaltimer` stamps (lane, per 64 tiles), NVML sampler (10-20 ms), CUPTI PC sampling.

**Matrix inset.** 2 × 2: {lane first, GEMM first} × {same process, MPS}, plus a fused-kernel backup cell.

**Decision box** (four outcomes with thresholds):
- KILL: lane ≥ 0.95 and GEMM ≥ 0.97 everywhere;
- GO-issue: lane < 0.5 with `not_selected` dominant;
- GO-residency: the lane cannot be resident, or only at a > 5% GEMM cost;
- GO-protection: the lane runs at rate but the GEMM loses > 5%.

**What is new.** The precise falsification test that decides whether the hardware track continues.
