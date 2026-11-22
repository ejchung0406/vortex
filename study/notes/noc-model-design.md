# The SM-to-memory network and L2/memory organisation for Vortex: design

Revision 3, 2026-11-21 (evening).

What this revision adds:
- the measured H200 L2 structure (`h200-l2-structure.md`);
- the answers to the independent verification (`noc-design-verification.md`, §9);
- the user's decisions: reference shape `h200g2` (2 GPCs, one per partition, 2 HBM3E
  stacks) and the exact-ratio shape `h200g4` (64 SMs).

This is a design. The build happens later on a new branch. The only code is a SimX
prototype of step 1 (the GPC switch, plus a launch core mask) in `vortex-exp/src_noc`.
That is HEAD 540b3dad2 + `notes/noc_model.patch`; runs come from the frozen copies
`src_noc_run1..4`. Code citations are `file:line` at HEAD 540b3dad2.

Source tags:
- **meas.**: measured on our H200 (`h200-l2-structure.md`, `tpc-placement-and-borrowing.md`,
  `open-threads.md` E1–E4, raw files in `results_gpu/`);
- **lit.**: literature (named; see `gpu-noc-sources.md`);
- **scaled**: derived for a smaller shape;
- **mine**: my inference or arithmetic.

Units:
- "Line" is 64 B (Vortex `L1_LINE_SIZE` = the part's DRAM fetch).
- Rates are **bytes per SM clock**. The H200 ran 1.98 GHz in latency scans and fell to
  1.57–1.85 GHz under its 560 W power cap in DRAM-saturated runs (meas.), so GB/s is
  quoted only with the clock it was measured at.

---

## 0. Summary

**Today.** Every SM has 4 private 94-cycle pipes into one flat L2.
- The L2 has 16 banks, selected by address.
- The banks reach 16 memory ports over omega networks, and a harness crossbar picks a
  DRAM channel by a second, independent address function (1 KB unit).
- There is no group, no partition and no shared link.

**The part, as measured on our H200:**
- **Partitions.** 8 GPCs in 2 partitions of 4 (sides {0,1,5,7} / {2,3,4,6}).
- **Slices.** 96 L2 slices, 8 per FBP × 12 FBPs, 60 MiB (640 KiB each).
- **Partition hash.** PA12⊕PA13⊕PA15⊕PA17⊕PA19⊕(page bits): a 4 KB granule. The slice
  granule is 256 B (medium confidence).
- **Latency.** Near/far L2 hit 290/467 cycles. DRAM miss with the home near 546, far
  733, plus a 7–10% class at ≈ 970.
- **Replication.** A remote read leaves a copy in the reader's partition.
- **Per-GPC limit.** 648 B/clk (1.28 TB/s) of **unique lines**. It is the same for DRAM
  and L2-resident data and for local, remote or mixed pages, and is reached at ~8 SMs.
  Same-line reads inside a GPC merge, so a shared buffer reaches 2.75 TB/s.
- **Inter-partition fabric.** 1.28 TB/s **per direction**.
  - Saturation total = min(Σ GPC caps, 2.56 per half, ~4.6).
  - One side: local 2.31, remote 1.28, mixed 2.57.
- **Per-SM ingress.** One ingress per SM, shared by TMA and loads, ≈ 110–116 B/clk.
  It is not shared-memory bandwidth.
- **Bystanders.** A saturated GPC slows SMs in every other GPC: TMA 192 → 88 (same half)
  / 106 (other half) GB/s; loads 196 → 93 / 132. So **the limits are not independent**.
  There is shared queueing downstream.

**The design (target 144 SMs, §1):**
1. **GPC switch with a return-side unique-line gate.** 18 SMs × 4 ports → U lanes.
   The gate counts responses, after an optional per-GPC coalescer.
2. **Two partitions**, each with its own crossbar from its 4 GPC uplinks and the fabric
   to its slices.
3. **A bidirectional fabric** (W lanes, B_x = 648 B/clk per direction, L_x ≈ 88 each
   way), with a **local-first lookup that leaves a copy** in the reader's partition.
4. **Slices = L2 banks, each bound to its own HBM channel.** Address → partition (4 KB
   hash) → slice (256 B hash) → that slice's channel → bank/row.
5. **A per-SM ingress limit** (110–116 B/clk) shared by the DXA and the dcache. It is
   enforced on the SM's **request issue**, not by holding responses: held responses
   would block the GPC's shared response lanes, while the part shows no slowdown inside
   an unsaturated GPC (6 SMs: 195 GB/s each vs 196 alone).
6. **The bystander effect** comes from return-side gating whose backlog waits in the
   slices' **single shared response queues** and the fabric queue. It is validated
   against the E-1 table (§1.5).

**How it maps onto Vortex (§2).**
- A slice is an existing `VX_cache_bank` with a private memory port.
- Both partitions live inside one L2 instance (one Vortex cluster).
- The switches are built from `VX_stream_xbar` with a `VX_mem_bus_arb`-style tag
  insert, `VX_mem_bus_delay`, and one new rate gate.

**Steps (§3.3, order decided 2026-11-21, §8).**
0. Per-SM rates: an SM issue limiter at 110–116 B/clk shared by TMA and loads, and a
   second 64 B DXA port.
1. GPC switch with the return-side gate: SimX prototype done.
2. Slice-channel binding and the unified address map.
3. Partitions and fabric, then 3b replication.
4. Placement (core mask): SimX prototype done.

The coalescer, buffer depths and slice granule come after the bystander re-measurement.

Defaults keep today's code path. Cycle identity is checked for step 1 (§7.1).

**Shapes (§4).** The user decided to use both `h200g2` and `h200g4`, plus `h200g4x8` as a
third shape. §4.5 has the side-by-side knob tables and which study uses which profile.
- **`h200g2` (reference).** 2 partitions × 1 GPC × 16 SMs × 16 slices × 1 HBM3E stack:
  32 SMs, 1.6 TB/s, 20 MiB.
  - The GPC cap binds: packed in one GPC 1.28 vs spread 1.54 TB/s (≤ 1.2×).
  - The fabric (kept at B_g) never binds, because one side's memory (0.77) is below it.
  - "Packed in one GPC" = "packed in one partition".
- **`h200g4x8`.** The same cost, 4 GPCs × 8 SMs. It separates the GPC effect from the
  partition effect.
- **`h200g4` (exact ratio).** 64 SMs = 4 GPCs × 16, 2 per partition, 3 stacks.
  - Memory per SM, SMs to fill memory (17%), L2 per SM and the GPC/memory ratio per
    side match the part.
  - The fabric still does not bind: it needs ≥ 3 GPCs per side, as on the part.

**Measurements so far (§7).**
- **Default is cycle-identical (§7.1).**
- **Fences, flushes, atomics and same-word order pass with the switch on (§7.6).**
- **Placement effects are visible**: `h200g4x8` one GPC vs spread 1.46×; `h200g4` 1.32×
  at 16 SMs, against the part-like 1.8×.
- **A capped GPC reaches 93% of its cap with 32 lanes** (83% with 8, because of lane
  head-of-line blocking).
- **The return-side gate reproduces the bystander effect (−30%; the part −45 to −54%);
  the request-side gate does not (−3%).**
- **Per-SM rates in Vortex are still about half the part's** (DXA on one 64 B port,
  loads MSHR-bound). Every GPC knee is at ~14 Vortex SMs instead of ~6–8 until the
  per-SM ingress is calibrated.

---

## 1. Target structure at 144 SMs

### 1.1 Block diagram and parameters

```
           ┌────────────────────── partition A (GPCs 0,1,5,7) ─────────────────────────┐        ┌── partition B (GPCs 2,3,4,6) ──┐
 SM ×18 ─4 ports─► [SM issue limiter, I_sm B/clk of requested lines, shared by DXA and dcache] │        │                    │
      │ L_sm                                                                            │        │                                │
      ▼                                                                                 │        │                                │
 GPC switch (72 → U lanes) ─► [coalescer, LRC_ENTRIES] ─► [return-side gate B_g] ─ L_up ─► partition router ─┬─► crossbar A (4U + W → S) ─► slices A ─► channels A
                                                                                                              └─► fabric A→B (W lanes, B_x, L_x) ─► crossbar B ─► slices B ─► channels B
 responses: slice ─► [one response queue per slice, HoL] ─► crossbar ─► (fabric B→A) ─► merge per lane ─► gate ─► switch ─► SM port
 far reads: local-first lookup in the reader's partition (same slice index); a miss is forwarded home and the fill leaves a copy locally
```

| Parameter | Meaning | 144-SM value | Source |
|---|---|---|---|
| N_gpc × N_sm | GPCs × SMs | 8 × 18 (shipping 16–18 after floorsweep: 7 GPCs of 16–18 and one of 8 + 8 lone) | meas. (TPC masks, GPC map); lit. H100 WP |
| P_sm | socket memory ports | 4 lines/clk each way (Vortex `L1_MEM_PORTS`) | Vortex |
| I_sm | per-SM ingress, shared by TMA and loads | 110–116 B/clk (215–230 GB/s at 1.98 GHz); TMA alone 196–204 GB/s, loads 218–223 near / 206 far | meas. (Part 2) |
| L_sm, L_up | SM → switch, switch → partition router | §1.4 | fitted |
| U | uplink lanes | 32 (≥ 3 × B_g / 64; fewer lanes lose bandwidth to lane head-of-line, §7.4) | mine; SimX |
| B_g | per-GPC limit on unique lines, return side | 648 B/clk (1.28 TB/s); flat from 8 SMs; the same for L2-resident (disjoint), DRAM, local, remote, mixed | meas. |
| LRC | per-GPC same-line merge | entries: unknown. Shared 256 KB: 2.75 TB/s at 18 SMs, 4 MB: 2.28 | meas.; lit. Accel-Sim 2.0 (≤ 4 merged) |
| G_p | GPCs per partition | 4 (sides {0,1,5,7}, {2,3,4,6}) | meas. |
| S_p | slices per partition | 48 = 6 FBPs × 8 | meas. (ncu: 96 slices, 8 per FBP, 12 FBPs) |
| C_s | slice capacity | 640 KiB (60 MiB / 96) | meas. (ncu) |
| W, B_x | fabric lanes and per-direction limit | B_x = 648 B/clk (1.28 TB/s); W ≥ 11 | meas. |
| L_x | fabric, each way | ≈ 88 (far − near = 177) | meas. |
| hash_p | partition select | PA12⊕PA13⊕PA15⊕PA17⊕PA19⊕c(2 MB page); 4 KB granule | meas. (= GemAtlas H20 Eq. 1) |
| G_slice | slice granule and hash | 256 B; bits 8, 10, 11 route, bits 7 and 9 do not | meas., **low confidence** (the probe is confounded by same-line merging; verification §8.1); lit. GemAtlas H20 |
| channels | HBM3E channels | 6,016-bit bus = 94 × 64 bit (one 128-bit slice of a stack disabled, inferred) | meas. (ncu), mine |
| slice ↔ channel | 1:1 | **our simplification**. It rests only on counts (96 slices, ~94–96 channels). NVIDIA's US11429534 describes 3 or 5 slices per 2 channels, never 1:1; Accel-Sim models 2:1 | mine |
| D | DRAM-side fixed delay | ≈ 174 (§1.4) | fitted |
| repl | remote-read copy | yes: afterwards near for the reader's whole side | meas. |
| sub-NUMA | near hits 280 / 305 split by GPCs {0–3} / {4–7}, across the halves | not modelled (±12 cycles) | meas. |
| cold tail | 7–10% of DRAM misses at ≈ 970 | from the DRAM model (row conflicts / refresh), to be checked | meas. |
| writes | 63 GB/s per SM, no GPC limit up to 935 GB/s | the return-side gate does not count posted writes | meas. |

### 1.2 Paths

**Near read.**
1. SM port → ingress gate → (L_sm) → GPC switch. The lane is hash(line) mod U, so one
   address always takes one lane.
2. Coalescer: a read of a line already outstanding from this GPC is merged and gets no
   uplink request.
3. Uplink → (L_up) → partition router. The partition is hash_p(address); local goes to
   the local crossbar.
4. Slice: hit, or a miss to its own channel (64 B fetch).

**Far read (with replication).**
- The router sends it first to the local crossbar, to the same slice index in the
  reader's partition ("local-first lookup": Accel-Sim 2.0, Fan et al.).
- Hit on a copy: near latency.
- Miss: forwarded over the fabric (W lanes, B_x, L_x) to the home slice. The response
  returns over the fabric, **fills the local slice as a copy**, and goes up the lane.

**Responses.**
- A slice has **one response queue** (as `VX_cache_bank` today).
- It goes through the crossbar output to the input it came on, then the fabric back if
  far, then a **merge per uplink lane** (local crossbar vs fabric: one arbiter per lane,
  1 cycle).
- Then the return-side gate, then the coalescer fan-out (multicast to every waiting
  port), then the switch, then the SM port, at one line per port per clock.

**Writes.**
- They go to the home slice; there is no local copy. The L2 is write-back when it is the
  LLC (`VX_CFG_L2_WRITEBACK`).
- Copies on the other side are invalidated by the home slice (§1.6).
- Evictions and fills use only the slice's own channel.

**CP and PTW clients.** They attach to partition A's crossbar on their own inputs,
outside the GPC gates. Their far half uses the fabric. A walk must not queue behind a
capped GPC (verification §3c).

### 1.3 Arbitration, flow control, ordering

| Element | In → out | Arbitration | Flow control | Notes |
|---|---|---|---|---|
| SM issue limiter | the socket's 4 request ports (DXA + dcache) | round-robin, credit I_sm per clock on requested lines | holds **requests** at the socket; responses are never held there | one ceiling shared by TMA and loads, as measured. Holding responses instead would block the GPC's shared response lanes; the part shows no slowdown inside an unsaturated GPC |
| GPC switch, requests | 72 ports → U lanes | per lane, round-robin over ports whose head selects it | valid/ready; input-queued (head-of-line) | per **port**. Option: per SM then per port (two-level), because a DXA SM uses 1 port and a load SM 4. In RTL a 72-input round-robin is a tree of 8-input slices: 2 cycles |
| coalescer | U lanes | — | full table → stall the lane | entries per GPC; merged responses fan out at the switch |
| gate | U lanes, return side | round-robin from where the last cycle ended, ≤ 1 per lane per clock; credit B_g/clk, burst ≤ U lines | holds responses; backlog backs into the uplink buffers, then the slices' response queues | request-side variant (knob) for comparison |
| partition router | lane → {local, fabric} | by hash_p | per output | the hash of ~16 bits is registered |
| crossbar (= the L2 input crossbar, restricted) | 4U local + W fabric + CP/PTW → S_p slices | round-robin per slice (`VX_cache.sv:520-543`) | slice skid | a full slice MSHR refuses every request to that slice |
| fabric | W lanes each way | round-robin; credit B_x/clk | delay buffers | separate request and response directions |
| merge per lane | {local, fabric} → lane | round-robin | — | needed for far responses |
| slice → channel | 1 : 1 | — | DRAM scheduler window | — |

**Fences and flushes (verification B2, required before step 1 leaves the prototype).**
- Today a flush token is served when it reaches the head of the L2 input it came on
  (`cache.cpp:2540-2549`, `:3434-3449`), and that input is the port's own FIFO.
- With address-hashed lanes, a port's earlier requests are spread over U lanes. The
  switch must therefore **hold a token until every earlier request of that port has
  left the switch** (a per-port outstanding counter), and **send it on every lane**,
  with the L2 joining U copies (or on lane 0 after draining all lanes).
- It must also export "network empty" to the fence/busy logic, as `l2_link_empty` does
  today (`VX_cluster.sv:305-326`).
- The prototype does not do this. Its streaming tests pass, but it is **not safe for
  kernels that fence between dependent memory phases**.

**Deadlock.** None, by this argument (corrected from rev. 2; verification §3c):
- requests and responses are separate networks, each a forest;
- a request crosses the fabric at most once;
- slices wait only on their response queue, MSHR and channel;
- a sink's ready (DXA writer, cache pipeline) never waits on the request network;
- gates refill with time alone.

Sinks are **not** unconditional, as rev. 2 wrongly said: the DXA's ready depends on its
shared-memory writer (`VX_dxa_gmem_req.sv:194`). It still never waits on the network.

### 1.4 Latency budget

**Targets (meas., SM cycles):**

| Path | Cycles |
|---|---|
| near L2 hit | 290 (sub-classes 280 / 305) |
| far L2 hit | 467 |
| DRAM miss, home near | 546 |
| DRAM miss, home far | 733 |
| warm 16 KB TMA (local copy) | 571 |
| warm 32 KB TMA | 721 |
| cold 16 KB TMA (new 2 MB page each trial) | 1,867. The TLB-miss explanation is inferred; no warm-TLB control exists |

**Vortex today (h200s):**
- L2 hit = 273 (calibrated on `h100` with one core, `ci/profile_check.py:141`).
- DRAM = 659.
- Per the verifier, the 273 is ~62 core/LSU/dcache + 2 × (94 link + ~2 arbiter/slice)
  + ~20 L2-internal. The rev.-2 "85 = L2 internals" was wrong.

**Re-split (mine):**

| Term | Target | Knob values |
|---|---|---|
| near hit | 290 = 273 + 17 | per direction L_sm + switch (2) + gate (1) + L_up = 94 + 8 − 3 → e.g. L_sm 40, L_up 59 |
| far hit − near hit | 177 | L_x + router/merge cycles ≈ 88 each way |
| near DRAM − near hit | 256, today 386 = 304 + ~82 | D = 304 − 130 = **174** |
| far DRAM − far hit | 266 | 256 + ~10 queueing: no new knob |
| warm 16 KB transfer | 571 | Vortex warm fixed part is 390 today (§7.2), +17 with the re-split; slope after the DXA rate calibration 16 KB / 110 B/clk = 149 → ≈ 556 ✓. **Only with replication**: without it half the lines are far and the transfer ends ≈ 177 later |
| cold tail 970 | | check that Ramulator's row-conflict / refresh tail produces ~7–10% at that latency; otherwise not modelled |

The constants move when the L2 has 32 banks and ~40 inputs instead of 16 and 90 (input
tree depth, response omega stages). **Re-run `ci/profile_check.py` per shape** and fit
L_up last.

### 1.5 The bystander effect: mechanism and validation

**Measured (meas., E-1).** Bystanders are 2 SMs in each of 7 GPCs; GPC 6 is on side B.

| Case | Same-half bystanders | Other-half bystanders |
|---|---|---|
| TMA alone | 192 | 192 |
| TMA + GPC 6 saturated (16 SMs) | 88 | 106 |
| TMA + GPC 6 with 6 SMs (unsaturated) | 180 | 182 |
| loads alone | 196 | 196 |
| loads + GPC 6 saturated | 93 | 132 |

The chip total with GPC 6 saturated is 2,655, below 2,687 (bystanders) + 1,278 (GPC 6).
The bystanders are in-flight-bound, so a halved rate means a doubled latency.

**Why independent caps cannot do this.**
- A request-side gate holds GPC 6's excess inside GPC 6. Nothing downstream changes, so
  other SMs keep their rate.
- The fabric is not saturated in this case (mine: side B's remote share ≈ 0.9 TB/s of
  1.28).
- So the slowdown is queueing in a shared place.

**Mechanism (proposed).**
- The GPC limit is enforced on the **return side**: GPC 6 may issue faster than its
  return port drains.
- The excess responses wait in the uplink buffers, then in the **single response queue of
  each slice they came from**, and for far lines in the **fabric's response queue**.
- A slice whose response queue holds lines for GPC 6 stalls (today `crsp_queue_stall`,
  `cache.cpp:1975-1976, 2111-2112`). Every requester of that slice then waits:
  head-of-line blocking at slices on both sides.
- Same-half bystanders also share GPC 6's fabric direction for their remote half, so they
  are hit harder.
- With a GPC of 6 SMs (unsaturated) nothing backs up, so bystanders lose only ~6%. That
  matches.

**Which queue holds a saturated GPC's backlog.** In Vortex terms:
- first the gate's input channels and the uplink response `DelayBuffer`s
  (2^⌈log2 L_up⌉ + 2 entries per lane: 66 at L_up = 59);
- then each L2 bank's **single core-response queue** (`L2_CRSQ_SIZE`; when it is full
  the bank stalls, `cache.cpp:1975-1976, 2111-2112`);
- for far lines, the fabric's response queue (step 3).

These depths are the calibration knobs. They are fitted **once** to the E-1 table and
then frozen.

The verifier's independent model (`noc-design-verification.md` §8.2) agrees with this
placement:
- a return-side limit with one FIFO per slice gives bystanders 84 / 86 (part 88 / 106);
- a request-side limit gives 178 / 181, which contradicts the part.

The model hits the other half harder than the part does (86 vs 106). The cause is not
known: on the part the far half is hit less.

**Validation**, against the measured table:
- (1) bystanders alone vs + saturated GPC vs + 6-SM GPC, TMA and loads;
- (2) the ratio same-half / other-half (needs step 3);
- (3) the chip total below the sum;
- (4) the saturation sweep "balanced vs all on one half": 2 GPCs 2.56, 3 or 4 GPCs on
  one half 2.565 (needs the fabric);
- (5) single GPCs: 1.28 for local, remote and mixed pages.

Step 1 can check (1) and (3) without partitions: `noc4_by_*` runs, §7.5. The request-side
gate is the control: it should show no effect.

### 1.6 Replication (measured; modelled in step 3b)

**What it takes:**
- **local-first lookup** for far reads in the reader's partition;
- the **fill on return** installs a copy in the local slice of the same index (a normal
  line marked "copy");
- **writes** go home and invalidate the copy on the other side. That costs an invalidate
  message over the fabric per write to a replicated line, or a per-line "replicated" bit
  at home to filter them.

Accel-Sim 2.0 describes "local write-through, remote write-back". The cheapest correct
choice for Vortex is write-to-home + invalidate.

Atomics and LR/SC must resolve at home and invalidate copies first.

Effort: SimX 1.5–2 weeks (slice tag state, an invalidate path, tests); RTL 3–4 weeks.

**What omitting it distorts:**
- A warm re-read of a far line stays far (467) where the part shows near (290). The warm
  16 KB transfer is ≈ 750 instead of 571.
- Every re-read of far data crosses the fabric, where the part crosses once. Fabric- and
  GPC-bound results for data with reuse across sides are wrong.
- Effective L2 capacity is overstated for lines used by both sides (Fan: 65.8% effective).

**Recommendation.** Build it in step 3, right after the partitions. Until then, the
model supports only read-once streaming across partitions (decode weights), not reuse
(compute GEMM operands, shared KV).

### 1.7 Per-GPC coalescer (LRC)

- A table per GPC of outstanding line addresses (`VX_CFG_NOC_LRC_ENTRIES`, 0 = off). A
  read that matches waits on the entry, and the response fans out to all waiting ports.
- Without it the model caps shared-buffer reads at 1.28 TB/s per GPC, against 2.75 on the
  part.
- Validate against the same-GPC sweep: shared 256 KB gives 1,302 / 1,585 / 1,947 / 2,514 /
  2,753 at 6 / 10 / 12 / 16 / 18 SMs; disjoint stays flat at 1.27–1.29.
- Effort: SimX 3–5 days.
- Default off. State its omission wherever SMs of one GPC share data.

---

## 2. Mapping onto Vortex

### 2.1 What exists (verified in code; cites corrected per the verifier)

| Structure | Where | Today |
|---|---|---|
| socket ports | `VX_socket.sv:931-1013`, port-0 priority arbiter `:973-988`, DXA on port 0 only `:803`; SimX `socket.cpp:253-311` | 4 ports × 1 line/clk. The DXA is one 64 B port (126.7 GB/s at most), so **210 GB/s per SM needs ≥ 2 DXA ports in RTL**. SimX has `VX_CFG_DXA_GMEM_PORTS` |
| SM → L2 link | `VX_cluster.sv:306-326` `VX_mem_bus_delay`; `cluster.cpp:128-147, 192-199` | private, elastic, 1 line/clk |
| L2 | `VX_cluster.sv:225-263` `VX_cache_wrap`; inputs = sockets × 4 + PTW + CP (`VX_gpu_pkg.sv:2345-2380`) | CP and PTW enter without the link |
| bank select | `VX_cache.sv:349-360`; `cache.cpp:132-159` | line bits XOR hash, 128 B granule, power-of-two banks |
| input crossbar | `VX_cache.sv:520-543` (tree in `VX_stream_arb.sv`); `cache.cpp:36-50, 2671-2830` | input-queued round-robin; 2 tree levels at 90 inputs |
| MSHR | per bank, `VX_cache.sv:596`; h200s 256 (`profiles/h100.json:47`) | a full MSHR refuses every request to its bank |
| bank → port | `VX_cache.sv:806-813`, omega networks `:264-284, 875-893`; `cache.cpp:3250-3305, 3702-3705` | at banks = ports the RTL still has 4 omega stages each way; **SimX's request side bypasses (0 cycles)**, a parity gap today |
| port → channel | SimX `memory.cpp:62-65` (`locate()`); rtlsim `VX_mem_bank_adapter` + `processor.cpp:443-459`; **opae/xrt do not use `locate()`** (`opae_sim.cpp:318-325`, `xrt_sim.cpp:539-571`) | independent of the bank |
| `locate()` | `dram_sim.cpp:393-417` | bijection; mixed mode needs power-of-two unit and channels. The SimX crossbar also needs ≤ 64 power-of-two outputs (`types.h:1734-1735`) |
| DRAM delay | `dram_sim.cpp:280` | reads only; a pure delay line |
| CP / PTW | `VX_gpu_pkg.sv:2361-2380`; `constants.h:120-143`; `cluster.cpp:109` | |
| cluster | `VX_gpu_pkg.sv:2421-2423`; `Vortex.sv:72-73` | private L2 per cluster |
| `VX_mem_xbar` | `hw/rtl/mem/VX_mem_xbar.sv`, no instance | **zeroes `tag.value`** (`:117, 121, 211`): not usable as is |
| KMU placement | `VX_kmu_bus_arb` (`VX_cluster.sv:149`, `VX_socket.sv:78`); SimX `kmu.cpp:109-157` | rev. 2's `VX_kmu.sv:179` was a DCR decode |
| L2 replacement | RTL tree PLRU (`VX_cache_repl.sv:110-163`), SimX age-counter LRU (`cache.cpp:312-360`) | differ today |
| `L2_FWD_HOLD` | RTL always holds; SimX knob, default replay | differ at h200s |

### 2.2 What each becomes

- **Slice = `VX_cache_bank`** with `L2_MEM_PORTS = L2_NUM_BANKS` = channels: bank i →
  port i → channel i.
  - The omega networks become wires in both RTL and SimX. That is a parity fix as well.
  - The harness crossbar becomes the identity, and `locate()` gives only the address
    inside the channel.
  - The channel-local address drops exactly the bit groups the selects consume. For
    16 slices per partition and 2 partitions: bits 8, 10, 11 plus one more slice bit,
    and bit 12 for the partition. Each is recoverable from its select value and the
    rest, because the selects are XOR folds.
  - A shared helper (`sim/common/addr_map.h` + an SV package function) is used by the
    bank select, the router and `locate()`, with a round-trip test over 2^20 lines.
  - Assert slice granule ≥ L2 line.
- **Non-power-of-two slices per partition** (24 at 3 stacks, 48 at full size) need a
  remap table with a defined inverse; a hash plus modulo is not invertible. The SimX
  crossbar's 64-output limit must also be lifted. `h200g2` (16 + 16) and `h200g4` with
  32 channels for 3 stacks (§4) avoid both.
- **Partitions** are bank groups inside the one L2. The router guarantees that a
  partition's inputs only target its banks, so the one input crossbar acts as two. The
  input tree and response omega are still sized by the totals: re-fit latency.
- **Replication** (step 3b) breaks "one line, one slice" by design: a copy lives in a
  slice of the other partition. The tag state needs a copy bit.
- **Vortex cluster** stays the whole GPU (`NUM_CLUSTERS = 1`).
- **MSHR per slice**: by Little's law at loaded latency (590–700 cycles seen at ~80%
  utilisation), 50 GB/s per slice needs ~250 lines, so keep 256.
- **SM issue limiter**: one more `NocUplink` instance per socket, in request-side mode
  (`GATE_REQ` = 1) on its 4 ports, I_sm per clock.
- **Credit bound, for every gate**: the credit grows by the rate per clock up to rate +
  (ports × line). A burst after idle is at most one line per port, and the long-run
  rate is exact.
- **FPGA drivers** (opae, xrt) map ports to physical banks themselves. The slice binding
  there is a separate change.

---

## 3. Knobs and steps

### 3.1 Knobs (defaults = today's code path)

| Knob | Default | Meaning | Step |
|---|---|---|---|
| `VX_CFG_NOC_GROUP_SIZE` | 0 = off | SMs per GPC | 1 (prototype) |
| `VX_CFG_NOC_UPLINK_PORTS` | next pow2 ≥ group × 4 | U | 1 |
| `VX_CFG_NOC_UPLINK_BYTES` | 0 = no gate | B_g per clock | 1 |
| `VX_CFG_NOC_UPLINK_GATE_REQ` | 0 = return side | 1 = count requests | 1 |
| `VX_CFG_NOC_SM_LATENCY` / `_UPLINK_LATENCY` | split of `L2_LINK_LATENCY` | L_sm, L_up | 1 |
| `VX_CFG_NOC_LRC_ENTRIES` | 0 | per-GPC coalescer | 1b |
| `VX_CFG_SM_INGRESS_BYTES` | 0 | I_sm per clock of requested lines, shared by the socket's request ports (DXA + dcache) | 1b |
| `VX_DCR_KMU_CORE_DISABLE0..7` | 0 | launch core mask | 4 (prototype) |
| `VX_CFG_L2_SLICE_BIND` | 0 (keeps today's two functions verbatim) | 1: channel = slice | 2 |
| `VX_CFG_L2_SLICE_GRANULE` / `_HASH` | 128 / today's | 256 / measured bits | 2 |
| `VX_CFG_L2_PARTITIONS` | 1 | 2 | 3 |
| `VX_CFG_L2_PARTITION_HASH` | — | bit list {12,13,15,17,19} (+ page bits) | 3 |
| `VX_CFG_NOC_GROUPS_PER_PARTITION` | all | G_p (a general mapping, so `h200g4x8` is a profile change) | 3 |
| `VX_CFG_NOC_XLINK_PORTS` / `_BYTES` / `_LATENCY` | — | W, B_x, L_x | 3 |
| `VX_CFG_L2_REPLICATE` | 0 | local-first lookup + copy | 3b |
| `VX_CFG_PLATFORM_MEMORY_LATENCY` | 304 | D (174 with partitions) | 3 |

Step-2 identity is achievable only with `SLICE_BIND = 0` keeping today's bank hash and
`locate()` verbatim. A "unified function with default parameters" cannot reproduce both.

### 3.2 Parameter table (the rev.-2 "pending measurement" column is now filled)

| Parameter | Vortex today (h200s) | 144-SM value | Source |
|---|---|---|---|
| per-SM ingress | DXA ~80–104 GB/s, one transfer at 32 B/clk; loads ~75 | 110–116 B/clk shared (TMA 196–204, loads 218–223 GB/s alone). Not one constant: with 4 KB TMA tiles the TMA + loads sum is 174–197. Whether it is a port limit or an in-flight budget is not separated | meas. |
| shared memory as the limit | — | no (TMA keeps 174–180 GB/s beside shared-memory readers) | meas. |
| GPC cap | — | 648 B/clk unique lines, flat from 8 SMs, DRAM = L2-resident (disjoint) | meas. |
| same-line merging | — | 2.75 TB/s per GPC with a 256 KB shared buffer | meas. |
| partitions × GPCs | 1 | 2 × 4 ({0,1,5,7}, {2,3,4,6}) | meas. |
| slices | 16 banks | 96 = 2 × 6 × 8; 640 KiB each | meas. (ncu) |
| partition hash / granule | — | PA12⊕13⊕15⊕17⊕19⊕page; 4 KB | meas. |
| slice granule | 128 B | 256 B (bits 8, 10, 11) | meas. (low confidence) |
| channel per slice | no | 1:1: our simplification (counts only; the patent has 3–5 slices per 2 channels) | mine |
| near / far hit | 273 | 290 (280 / 305) / 467 | meas. |
| near / far DRAM | 659 | 546 / 733; tail 970 (7–10%) | meas. |
| DRAM fetch | 64 B (L2 sector) | 64 B per miss | meas. |
| fabric | — | 1.28 TB/s per direction; one side local 2.31 / remote 1.28 / mixed 2.57 | meas. |
| saturation | — | min(Σ GPC, 2.56 per half, 4.6), **only when every active GPC is saturated**. The rule fails by −28 to −33% with bystanders and by −5 to −14% with 2–4 SMs per GPC spread (verification §8.1). Same-half GPC pairs are 0.7% below opposite-half pairs | meas. |
| bystanders | — | §1.5 | meas. |
| replication | no | yes | meas. |
| writes | — | 63 GB/s per SM, no GPC limit up to 935 GB/s | meas. |
| per-slice bandwidth | 1 req/clk/bank | not determined | — |
| L2 replacement | pseudo-LRU (calibrated) | unknown | — |

### 3.3 Steps

| Step | SimX | RTL | Tests | Effort |
|---|---|---|---|---|
| **0 per-SM rates (first; decision 4)** | SM issue limiter: one `NocUplink` per socket in request-side mode on its 4 ports, I_sm = 110–116 B/clk of requested lines, shared by the DXA and the dcache; DXA on 2 global-memory ports (`VX_CFG_DXA_GMEM_PORTS` = 2, exists in SimX) | the same rate-gate module per socket; a second DXA port behind the dcache on socket port 1 (`VX_socket.sv:800-803` `GMEM_OUT_PORTS` 1 → 2, plus a dcache-first arbiter on port 1, as SimX `socket.cpp:298-311`) | one SM: TMA alone ≈ 196–204, loads alone ≈ 218, TMA + loads in one CTA ≈ 215–230 GB/s at 1.98 GHz (all as fractions of 110–116 B/clk); rtlsim parity on `bwstream_dxa` and `bwstream -b32` | SimX 3–4 d; RTL 1–1.5 wk |
| 1 GPC switch + gate (return side, decision 1) | prototype done, including fence/flush ordering (§7.6). Left: the per-SM fairness option, the gfx-cache branch | `VX_mem_bus_delay` ×2; switch = `VX_stream_xbar` (8-input tree, 2 cycles) + `VX_mem_bus_arb`-style tag insert (not `VX_mem_xbar`); new `VX_noc_rate_gate` (per-clock grant count = prefix sum of valids against credit/64, not a 16-deep subtract chain); `VX_cluster.sv` generate replacing `:306-326`; `L2_SOCKET_REQS`, `L2_TAG_WIDTH` (+log2(G × 4)) | default perf_gate baselines unchanged; `model_parity` at 8 cores with G = 4, U = 2/4, B = 96 and a case with ample credit; **a fence-under-load case; the CP path during a capped stream**; `logic_gate` for the gate | SimX 3–4 d left; RTL 1.5 wk |
| 2 slice-channel binding | `cache.cpp` (granule, set slicing, identity bank → port with no omega), `memory.cpp`, `dram_sim.cpp` (channel-local address), `sim/common/addr_map.h`, rtlsim `processor.cpp` | `VX_cache.sv` bank/set slicing, omega bypass at identity; shared SV function | `tests/unittest/dram_sim` round trip; `cache.yaml` `SLICE_BIND = 1`; `profile_check` | SimX 1–1.5 wk; RTL 1.5–2 wk |
| 3 partitions + fabric | router, fabric, merge per lane, inputs per partition | router (registered hash), fabric = delay + gate, merge arbiter per lane | near/far latency (`bwstream_dxa -l` with near/far page lists); local/remote/mixed page sets; E-1 bystanders; saturation sweep | SimX 1–1.5 wk; RTL 2 wk |
| 3b replication (decision 2) | local-first lookup, copy fill, invalidate on write | tag copy bit, invalidate path | warm far re-read becomes near; write-after-replicated-read coherence test; atomics | SimX 1.5–2 wk; RTL 3–4 wk |
| 4 placement | prototype done (KMU mask); later `vx_launch_info_t` field | mask in `VX_kmu_bus_arb` at every level of the hierarchy | masked `bwstream` on rtlsim and SimX | RTL 2–3 d |

---

## 4. Shapes

### 4.1 What the part shows (meas.)

| Id | Phenomenon | Value |
|---|---|---|
| P1 | SMs that fill memory | 21 spread: 81%; 24: 87%; 32: 98% (of 4.63 TB/s). Hard-min 4.63 / 0.205 = 22.6 = 17% of 132 |
| P2 | GPC bend | flat from 8 SMs; hard-min crossing 6.35 SMs; −2.4% at 6 |
| P3 | packed vs spread | 16 SMs: 1,283 vs 3,084 = 2.4×; 8 SMs: 1.26× (TMA), 1.48× (loads) |
| P4 | fabric | 1.28 per direction; one side remote-only 1.28 vs local-only 2.31; ≥ 3 GPCs on one side capped at 2.565 |
| P5 | near/far latency | 290 / 467 |
| P6 | L2 per SM | 60 MiB / 132 = 465 KiB (427 KiB per full-die SM) |
| P7 | memory per SM | 4.63 TB/s achieved / 132 = 35 GB/s |
| P8 | bystander effect | §1.5 |
| P9 | GPC cap / memory | 1.28 / 4.63 = 0.28; per side: 4 GPC caps (5.12) on 2.31 local + 1.28 fabric |

### 4.2 `h200g2`: reference (user's decision)

**Shape.**
- 2 partitions. Each has 1 GPC of 16 SMs (an 18-SM variant is a note), a crossbar,
  16 slices, and one HBM3E stack of 16 channels (2 FBPs × 8 slices, slice = channel).
- The partitions are joined by the fabric.
- Totals: 32 SMs, 1.6 TB/s nominal (1.544 at the part's 96.5%), 32 × 640 KiB = 20 MiB.

**What binds** (per-SM 210 GB/s once calibrated; 50/50 mixed pages unless stated):

| Placement / pages | GPC cap (1.28) | side memory (0.772 each) | fabric (B_x) | result |
|---|---|---|---|---|
| 16 packed in GPC A, mixed | 1.28 | local half 0.64 ≤ 0.772 ✓; remote half 0.64 | 0.64 ≤ B_x needed | **1.28 if B_x ≥ 0.64** |
| 8 + 8 spread, mixed | 2 × 1.28 | 1.544 binds | 0.386 per direction | **1.544** (ratio 1.21) |
| 16 packed, local pages only | 1.28 | 0.772 binds | — | 0.772 |
| 16 packed, remote pages only | 1.28 | 0.772 | min(B_x, 0.772) | 0.772 if B_x ≥ 0.772 |
| all 32, mixed | 2.56 | 1.544 | 0.772 | 1.544 |

**B_x choice** (answers verification B1):
- On the part, B_x equals one GPC's cap (1.28 = 1.28). A single GPC never reaches the
  fabric limit: remote, local and mixed all give 1.28.
- Scaled by memory instead (0.55 × side local = 0.43 TB/s), the fabric would cap a packed
  GPC at 0.86 and hide the GPC limit. That contradicts the part's single-GPC behaviour.
- **Set B_x = B_g = 648 B/clk (measured, unscaled).**
- Consequence: in `h200g2` the fabric **never binds**, because one side's memory (0.772)
  is below it. The fabric phenomena (remote-only 55% of local-only, ≥ 3 GPCs on a side
  capped at 2 × fabric) **are not reproduced**. On the part they need ≥ 3 GPCs per side
  as well.

**Plainly** (this agrees with verification §8.3):
- **Packed in one GPC, mixed pages: 1.28 TB/s, GPC-bound.** Spread 8 + 8: 1.54,
  memory-bound.
- **Packed with local-only or remote-only pages: 0.77, bound by one side's memory.** So
  in `h200g2` the GPC cap shows only with mixed pages.
- **The fabric never binds**, and local-only and remote-only pages give the same totals
  (the part: 1.8×).
- **No fabric value keeps both the GPC limit and the part's local/remote contrast**: one
  side's memory (0.77) is below the GPC cap here and 1.8× above it on the part.
- **Same-half bystanders cannot exist** (one GPC per side).

**Which studies must use another shape:**
- same-half bystanders, and several GPCs sharing a partition: `h200g4` (2 GPCs per side)
  or `h200g4x8`;
- the local/remote page contrast and the "≤ 2.56 TB/s per half" fabric limit: only the
  full 144-SM shape, which has 3 stacks per side.

**Deviations from the part**, to be stated with every result:

| Quantity | `h200g2` | Part |
|---|---|---|
| memory per SM, achieved | 1.544 / 32 = 48 GB/s | 35 GB/s (1.375×) |
| SMs that fill memory | ~7.4 hard-min = 23% (≈ 11 SMs for 98% on the part's roll-off) | 17% |
| L2 per SM | 640 KiB | 465 KiB (1.38×) |
| GPC cap / memory | 0.83 | 0.28 |
| packed / spread at the fill point | ≤ 1.2× | 2.4× at 16 SMs |
| fabric | invisible | binds at ≥ 3 GPCs per side |
| one GPC per partition | GPC and partition effects cannot be separated; every bystander is in the other half, so the same-half case (88 vs 106 on the part) cannot be reproduced | 4 GPCs per partition |
| page sets | packed with local-only or remote-only pages is memory-bound at 0.6 of the cap, so page-set experiments say nothing about the GPC or the fabric | local 2.31 / remote 1.28 per side |

**Preserved:**
- everything inside the SM (once calibrated);
- the GPC bend at ~1/3 of a GPC;
- near/far latency and replication (once step 3b exists);
- slice-channel binding and the address path;
- the few-memory-SMs regime (1–3 SMs leave most memory idle);
- co-residency studies;
- the bystander mechanism, **other half only**: with one GPC per partition every bystander
  is in the other half.

**Text for the future profile's `sources`** (proposed):

> "Two GPCs of the H200 (16 SMs each, one per L2 partition), each with one of the six
> HBM3e stacks. The part has 1.5 stacks per two GPCs; two are used so that the measured
> GPC limit (1.28 TB/s) binds. So memory per SM is 48 GB/s against the part's 35
> (1.375×), about 7–8 SMs (23%) fill the memory against 22 (17%), and the L2 holds
> 640 KiB per SM against 465. The fabric is kept at the measured 1.28 TB/s per direction
> and does not bind with one GPC per side. With one GPC per partition, packing into one
> GPC is packing into one partition. Quote results as fractions of the memory roof."

**Knob values** (future profile, base `h200s`; not created):

| Knob | Value | Source |
|---|---|---|
| `NUM_CORES` | 32 (36 for 18-SM GPCs) | scaled |
| `NOC_GROUP_SIZE` | 16 | meas. |
| `NOC_GROUPS_PER_PARTITION`, `L2_PARTITIONS` | 1, 2 | scaled, meas. |
| `NOC_UPLINK_PORTS` / `_BYTES` / `_GATE_REQ` | 32 / 648 / 0 | meas. (648 B/clk); 32 lanes per §7.4; return side per §1.5 |
| `NOC_LRC_ENTRIES` | 0 until fitted | meas. target §1.7 |
| `SM_INGRESS_BYTES` | 113 | meas. 110–116 |
| `NOC_SM_LATENCY` / `_UPLINK_LATENCY` | 40 / 59 (re-fit per shape with `profile_check`) | meas. near hit 290 |
| `NOC_XLINK_PORTS` / `_BYTES` / `_LATENCY` | 16 / 648 / 88 | meas. 1.28 TB/s per direction; far − near 177 |
| `L2_PARTITION_HASH` | bits {12,13,15,17,19} (+ page bits; with < 2 MB pages use the low bits only) | meas. |
| `L2_NUM_BANKS` (slices) / `L2_MEM_PORTS` | 32 / 32 | scaled: 16 channels per side |
| `L2_SLICE_GRANULE` / `_HASH` | 256 / bits 8, 10, 11 (+ one higher bit for 16 per side) | meas. (medium) |
| `L2_SIZE` / `L2_NUM_WAYS` | 20,971,520 / 20 (256 sets × 128 B per slice = 640 KiB) | meas. slice size |
| `L2_MSHR_SIZE` | 256 | mine (Little at loaded latency) |
| `L2_REPLICATE` | 1 | meas. |
| `PLATFORM_MEMORY_NUM_BANKS` / `_DATA_RATE` / `_TYPE` | 32 / 6250 / HBM3E | 2 stacks × 16 channels |
| `PLATFORM_MEMORY_LATENCY` | 174 | meas. DRAM − hit |
| other platform and per-SM knobs | as h200s | h200s calibration |

### 4.3 `h200g4`: exact ratio (64 SMs)

**Shape.**
- 4 GPCs × 16 SMs, 2 per partition, 3 HBM3E stacks (48 channels) = 2.4 TB/s nominal
  (2.32 achieved).
- 48 slices × 640 KiB = 30 MiB. That is 24 slices per partition: a non-power-of-two
  remap in step 2.
- The step-1 prototype runs use **32 channels at 75 GB/s** and a flat 30 MiB L2 in
  32 banks × 30 ways, because `locate()` and the SimX crossbar need powers of two.

**What it keeps:**

| Quantity | `h200g4` | Part |
|---|---|---|
| memory per SM | 36 GB/s | 35 ✓ |
| SMs that fill memory | 11 = 17% | 17% ✓ |
| L2 per SM | 480 KiB | 465 ✓ |
| per side | 2 GPC caps on 1.16 local + 1.28 fabric | part per 2 GPCs: 1.16 local ✓ |
| packed 16 (one GPC) vs spread 16 (4 per GPC) | 1.28 vs 2.32 = **1.8×** | part at 24% of SMs (32): 2.0× |

**What it loses:** the fabric phenomena still need ≥ 3 GPCs per side. With B_x = B_g a
side's remote-only traffic is memory-bound (1.16) like its local-only traffic.

**Cost:** §7.6.

### 4.4 All shapes

Values assume per-SM ingress at 210 GB/s and memory at 96.5% of nominal.

| Shape | SMs | GPCs × SMs (per partition) | slices / stacks | memory | per SM | fill SMs | GPC bend | cap / memory | packed vs spread | GPC vs partition separable | fabric visible | L2 / SM | cost (wall per kcycle, §7.6) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **`h200g2` (reference)** | 32 | 2 × 16 (1 + 1) | 32 / 2 | 1.6 | 48 | 7.4 = 23% | 6.1 ✓ | 0.83 | ≤ 1.2× | no | no | 640 KiB | ~1.3× h200s |
| `h200g2-18` | 36 | 2 × 18 | 32 / 2 | 1.6 | 43 | 21% | 6.1 ✓ | 0.83 | ≤ 1.2× | no | no | 569 KiB | ~1.4× |
| `h200g4x8` | 32 | 4 × 8 (2 + 2), cap 324 B/clk | 32 / 2 | 1.6 | 48 | 23% | 3.1 = 38% ✓ | 0.41 | one GPC 0.64 / one side 1.28 / spread 1.54 | yes | no (side 2 × 0.64 < 1.28) | 640 KiB | ~1.3× |
| `h200g2-1s` (1 stack) | 32 | 2 × 16 | 16 / 1 | 0.8 | 24 | 12% | (never) | 1.66 | 1.0× | no | no | 320 KiB | ~1.2× |
| **`h200g4` (exact ratio)** | 64 | 4 × 16 (2 + 2) | 48 / 3 | 2.4 | 36 ✓ | 17% ✓ | 6.1 ✓ | 0.55 | 1.8× | yes | no (needs ≥ 3 GPCs per side) | 480 KiB ✓ | §7.6 |
| full | 144 | 8 × 18 (4 + 4) | 96 / 6 | 4.8 | 35 ✓ | 17% ✓ | ✓ | 0.28 ✓ | 2.4× ✓ | yes | **yes** | 427–465 KiB ✓ | ~6–7× |
| `h200s` today | 22 | none | 16 / 1 | 0.8 | 36 | 17% | — | — | — | — | — | 393 KiB | 1 |

### 4.5 Decision: both profiles (user, 2026-11-21)

**`h200g2`** is for per-SM mechanism studies and fast iteration. **`h200g4`** is for
anything that depends on ratios, on several GPCs in one partition, or on same-half
bystanders. **`h200g4x8`** stays as a third shape: it separates the GPC effect from the
partition effect at `h200g2`'s cost.

All three are knob settings of one model. No code differs, provided step 3 implements
`NOC_GROUPS_PER_PARTITION` as a general GPC → partition map and step 2 supports the
remap below.

#### Knob values side by side (future profiles; none created)

| Knob | `h200g2` | `h200g4` | `h200g4x8` | Source |
|---|---|---|---|---|
| `NUM_CORES` | 32 | 64 | 32 | scaled |
| `NOC_GROUP_SIZE` | 16 | 16 | 8 | meas. (GPCs of 16–18); 8 is scaled |
| `L2_PARTITIONS` / `NOC_GROUPS_PER_PARTITION` | 2 / 1 | 2 / 2 | 2 / 2 | meas. (2 partitions × 4 GPCs), scaled |
| `NOC_UPLINK_BYTES` | 648 | 648 | 324 (3 SMs' worth: a GPC saturates at ~38% of its SMs) | meas. 1.28 TB/s; scaled |
| `NOC_UPLINK_PORTS` (U) | 32 | 32 | 32 | mine: ≥ 3× B_g / 64 against lane head-of-line (§7.4) |
| `NOC_UPLINK_GATE_REQ` | 0 (return side) | 0 | 0 | §1.5, §7.5 |
| `NOC_SM_LATENCY` / `_UPLINK_LATENCY` | 40 / 59 | 40 / 59 | 40 / 59 | meas. near hit 290; re-fit per shape with `profile_check` |
| `NOC_LRC_ENTRIES` | 0 (later fitted) | 0 | 0 | §1.7 |
| `SM_INGRESS_BYTES` | 113 | 113 | 113 | meas. 110–116 B/clk |
| `NOC_XLINK_BYTES` / `_PORTS` / `_LATENCY` | 648 / 32 / 88 | 648 / 32 / 88 | 324 / 32 / 88 (= one GPC cap, as on the part) | meas. 1.28 TB/s per direction; far − near 177 |
| `L2_PARTITION_HASH` | bits {12,13,15,17,19} | same | same | meas. |
| slices = channels | 32 (16 per side) | 48 (24 per side) | 32 (16 per side) | scaled: 16 per stack |
| `L2_SLICE_GRANULE` / hash | 256 / bits 8, 10, 11 + one more | 256 / remap to 24 per side | as `h200g2` | meas. (low confidence) |
| `L2_SIZE` / `L2_NUM_WAYS` (640 KiB slices, 256 sets × 128 B) | 20 MiB / 20 | 30 MiB / 20 | 20 MiB / 20 | meas. slice size |
| `L2_MSHR_SIZE` | 256 | 256 | 256 | mine |
| `L2_REPLICATE` | 1 | 1 | 1 | meas. |
| `PLATFORM_MEMORY_TYPE` / `_NUM_BANKS` / `_DATA_RATE` | HBM3E / 32 / 6250 | HBM3E / 48 / 6250 | HBM3E / 32 / 6250 | 16 channels per stack (JESD238B) |
| stacks / memory | 2 / 1.6 TB/s | 3 / 2.4 TB/s | 2 / 1.6 TB/s | — |
| `PLATFORM_MEMORY_LATENCY` | 174 | 174 | 174 | meas. DRAM − hit |
| memory per SM (achieved) | 48 GB/s (1.375×) | 36 GB/s (≈ part) | 48 GB/s | — |
| SMs that fill memory (hard-min) | 23% | 17% | 23% | part 17% |
| L2 per SM | 640 KiB | 480 KiB | 640 KiB | part 465 KiB |

#### The 48 channels of `h200g4`

The SimX memory crossbar needs ≤ 64 power-of-two outputs (`types.h:1734-1735`), and
`locate()`'s mixed mode needs power-of-two channels (`dram_sim.cpp:400-402`). Two
routes:

- **(a) Final (step 2).** With `SLICE_BIND = 1` the port-to-channel crossbar is the
  identity, so it is **not instantiated**: each slice's memory port is wired straight to
  its channel, and the crossbar limit does not apply.
  - The slice select within a partition uses a remap table with a defined inverse,
    24 entries per side (GemAtlas's "128-to-96 remapping" style).
  - `locate()` then gives only the channel-local address and keeps its power-of-two
    bank/row hash inside the channel.
  - The cache bank count (48) is not a power of two either. The bank index comes from
    the remap, not from address bits, and the set index uses the remaining bits.
  - That is part of step 2's address-map work (verification §3e): about +3–5 days.
- **(b) Interim** (until step 2): 32 channels at 9375 MT/s (75 GB/s each, 2.4 TB/s) and a
  flat 30 MiB L2 in 32 banks × 30 ways. This is what the §7 runs use. It keeps the memory
  bandwidth and capacity but has 2/3 of the part's channel and bank parallelism per byte.
  Quote it as such.

#### Which profile each planned study must use

| Study | Profile | Why |
|---|---|---|
| per-SM ingress, DXA/TMA vs loads, co-residency inside one SM, the shared-memory bypass | `h200g2` | SM-level; fastest |
| GPC limit, packed vs spread at the GPC level, placement inside a GPC | `h200g2` (mixed pages only) | the GPC cap binds; one side's memory binds with single-side page sets |
| GPC vs partition attribution | `h200g4x8`, or `h200g4` | `h200g2` cannot separate them |
| bystanders in the same half | `h200g4` | `h200g2` has one GPC per side |
| any number quoted against the part's memory-per-SM or "SMs that fill memory" (the motivation: few SMs leave memory idle) | `h200g4` | exact ratio |
| co-run of a memory-bound and a compute-bound job across the chip | `h200g4` | ratio-dependent |
| local vs remote page sets, the 2.56 TB/s per half fabric limit | **neither**: full 144 SMs only | side memory must exceed the fabric (3 stacks per side) |
| near/far latency, replication | either | needs step 3/3b |

The fabric binds in **neither** scaled profile. In `h200g4` a side's local memory is
1.16 TB/s, below the fabric's 1.28. Fabric studies at reduced size would need B_x scaled
to 0.55 × side memory (0.64 TB/s in `h200g4`). That breaks the measured fact that a
single GPC is unaffected by local/remote placement (verification §8.3), so it is not a
default; use it only with that caveat.

---

## 5. Risks

1. **Per-SM ingress is not calibrated.** In RTL the DXA is one 64 B port: 126.7 GB/s at
   1.98 GHz, at most.
   - So 210 GB/s per SM is unreachable for TMA in RTL by any rate change. It needs ≥ 2
     DXA global-memory ports (SimX has `VX_CFG_DXA_GMEM_PORTS`; RTL fixes 1 at
     `VX_socket.sv:803`) or a 128 B port.
   - Until then a GPC of DXA SMs bends at ≥ 10 SMs, not 6–8. Today's SimX DXA gives
     43–52 B/clk, so the bend is at ~13–15 SMs.
   - Plain loads are MSHR-bound at ~40 B/clk.
2. **Replication missing** (until 3b): §1.6.
3. **Bystander strength depends on buffer depths** that were not measured on the part.
   Fit them to E-1 and keep them fixed afterwards; do not re-tune them per study.
4. **Fences** (§1.3): the prototype is unsafe for fenced kernels.
5. **Per-port fairness.** A load SM gets up to 4× the share of a DXA SM in a capped GPC.
6. **Today's per-SM rate falls with SM count even without any network** (`noc_off_*`).
   It is also placement-dependent without a network (`noc3_off_*`, §7). This must be
   understood before network results are read.
7. **The slice granule and hash are low confidence.** The 1:1 slice ↔ channel binding
   rests on counts. The h200s 1 KB channel interleave was chosen for row locality; with
   256 B slices the channel-internal row mapping must restore it.
8. **The cold-latency tail and D**: the TLB share of the 1,867 cold transfer is unknown.
9. **Shape artefacts**: `h200g2` cannot separate GPC from partition, and no scaled shape
   shows the fabric limit.
10. **Not modelled**: TPC (no limit, meas.), CPC (no read limit, meas.), the sub-NUMA
    280/305 split, the SM-to-SM network, MIG, power-capped clock drops (the part's GB/s
    fall at high load; the model's do not).

---

## 6. Prototype state

**Patch.** `notes/noc_model.patch`, against the HEAD export 540b3dad2, regenerated at the
end of this revision.

**Gate.** `sim/simx/noc_uplink.h`:
- return-side (default) or request-side (`VX_CFG_NOC_UPLINK_GATE_REQ = 1`);
- round-robin from where the previous cycle's grants ended, at most one packet per lane
  per clock;
- credit burst up to one line per lane;
- sleeps on `size()`, not `empty()`.

**Bugs fixed during this work; every affected result was discarded:**
- (a) lost wake-up on `empty()` (`src_noc_run1`);
- (b) the round-robin pointer moved inside the scan, which double-granted lanes or
  skipped them (`src_noc_run1/2`; verification F1);
- (c) credit capped at one line, which lost rate under bursts (F2);
- (d) core lists with commas were cut by `sbatch --export`; `+` is accepted now.

**Copies.**
- `src_noc_run3` has (a)–(c) fixed with a **request-side** gate.
- `src_noc_run4` adds the gate-side knob (return side by default) and the `-T` per-core
  rates.
- `src_noc_run5` = `src_noc` = the patch. It adds fence/flush ordering through the lanes
  (`sim/simx/noc_order.h`: `NocPortOrder` per group, `NocLaneTap` per lane). The uplink
  request delay is shortened by 2 so the latency is unchanged.

**Other changes.**
- `assert` → `__assert` for the tag-overflow and mask checks. `NDEBUG` builds keep them.
- `bwstream -T` and `bwstream_dxa -T`: per-core bytes over each core's span, from per-CTA
  `{core, start, end}` cycles.
- `-l S` latency mode; `-M` core lists.

**Not done:**
- the ingress (issue) limiter;
- the coalescer;
- the per-SM fairness option;
- the gfx-cache branch;
- rtlsim (the KMU mask is SimX-only: rtlsim would place CTAs on masked cores).

---

## 7. Measurements (SimX prototype, step 1 only)

**Setup.**
- Runs: `results/noc*`; the frozen copy is named per table.
- The collector is `notes/noc_collect.py`.
- B/clk = memory reads × 64 / kernel cycles, whole kernel. "Per-core" values come from
  `-T`: each core's CTA bytes over its own span.
- Workloads:
  - DXA: `bwstream_dxa -e4096 -n256 -d4 -c0` (16 KB tiles, 4 in flight per CTA, 3 CTAs
    per SM);
  - loads: `bwstream -n2048 -u4 -b32`.
- Placement:
  - "packed" = cores 0..b−1, filling GPC 0 first;
  - "spread" = b/G per GPC.

**What is and is not representative.**
- Built: groups, U lanes, the byte gate (return-side unless stated) and two latency
  levels.
- Behind them is today's flat L2: banks not tied to channels; channels from `locate()`
  with the 1 KB unit. There are no partitions, no fabric, no replication and no
  coalescer, so every L2 hit is "near".
- `h200g4` runs use the interim memory: 32 channels at 75 GB/s and a flat 30 MiB L2
  (§4.5).
- **Per-SM rates are today's Vortex rates**: DXA 41–52 B/clk, loads 39–40 B/clk. The
  part's ingress is 110–116 B/clk, so every knee sits at ~2.5× the part's SM count.
  Fractions of the memory roof are quoted, but **the SM counts are not comparable with
  the part's** until the per-SM calibration lands.

### 7.1 Default is cycle-identical (step 1 off; `src_noc_run1`)

| Run | HEAD | Prototype, off |
|---|---|---|
| `gemm_block -m16 -n1024 -k4096 -s1`, `-DMTILES=1 -DPROFILE_ENABLE`, 1 core | 653,498 | **653,498** |
| `bwstream -n256 -u4 -b32`, 22 cores | 137,037 | **137,037** |
| `bwstream_dxa -e4096 -n16 -d4 -c0`, 22 cores (HEAD's test binary) | 56,464 | **56,464** |

The NoC-off code path is unchanged in every later copy: everything new is under
`#if VX_CFG_NOC_GROUP_SIZE != 0`, plus `__assert` and the KMU mask, which has no effect
at all-zero.

The test kernels gained `-l` and `-T` branches. Their binaries differ (the instruction
count changed), so identity is shown with HEAD's binaries.

### 7.2 Single-transfer latency (1 SM)

| | warm 16 KB | cold 16 KB | warm 32 KB | cold 32 KB | fixed + slope |
|---|---|---|---|---|---|
| Vortex `h200s`, off | 902 | 1,315 | 1,414 | 2,038 | 390 + 512 per 16 KB (one worker at 32 B/clk) |
| Vortex `h200g2` shape, groups on (`src_noc_run2`) | 906 | 1,320 | 1,418 | 2,048 | +4 cycles: extra channel stages |
| H200 (meas.) | 571 | 1,867 (new page each trial) | 721 | 2,013 | ≈ 421 + 150 (110 B/clk) |

### 7.3 The GPC bend and packed vs spread (return-side gate, `src_noc_run4`)

**`h200g2` shape** (2 × 16, cap 646 B/clk, 32 ch, 1.6 TB/s = 808 B/clk), B/clk total
(per SM; % of roof):

| b | DXA packed | DXA spread | spread / packed | loads packed | loads spread | spread / packed |
|---|---|---|---|---|---|---|
| 1 | 43 (5%) | — | — | 40 | — | — |
| 4 | 198 (49.5; 24%) | 204 (51; 25%) | 1.03 | 158 | 157 | 1.00 |
| 8 | 323 (40.4; 40%) | 374 (46.8; 46%) | 1.16 | 312 | 312 | 1.00 |
| 16 | 511 (31.9; 63%) | 603 (37.7; 75%) | **1.18** | 583 (72%) | 589 (73%) | 1.01 |

**`h200g4x8` shape** (4 × 8, cap 323 B/clk, 8 lanes):

| b | DXA packed | DXA spread | spread / packed | loads packed | loads spread | spread / packed |
|---|---|---|---|---|---|---|
| 4 | 180 | 200 | 1.11 | 158 | 158 | 1.00 |
| 8 (= one GPC) | 267 (33.8; **83% of the cap**) | 389 | **1.46** | 301 | 302 | 1.00 |
| 16 (= two GPCs = one future partition) | 520 | 608 | 1.17 | 548 | 594 | 1.08 |

**`h200g4` shape** (4 × 16, cap 646, interim 2.4 TB/s = 1,212 B/clk):

| b | DXA packed (return-side, `run4`) | DXA spread | spread / packed | DXA packed (request-side, `run3`) | spread (`run3`) | loads packed / spread (`run4`) |
|---|---|---|---|---|---|---|
| 1 | 41 | — | — | 43 | — | — |
| 4 | 199 | 165 | 0.83 | 201 | 169 | — |
| 8 | 329 (27%) | 395 (33%) | 1.20 | 331 | 402 | 311 / 314 |
| 12 | — | — | — | 434 | 577 | — |
| 16 | 536 (44%) | 709 (58%) | **1.32** | 557 | 697 | 573 / 597 |
| 24 | — | — | — | 641 (53%) | 918 (76%) | — |
| 32 (= 2 GPCs packed) | 854 (70%) | 932 (77%) | 1.09 | 848 (70%) | 939 (78%) | 835 (69%) / 897 (74%) |

Against the part:

| | Part | Model now |
|---|---|---|
| bend | flat from 8 SMs per GPC | with today's ~45 B/clk SMs, the 646 B/clk cap is reached only at ~14–16 SMs; the `h200g4` packed curve flattens at 16–24 (536 → 641). The packed curve is soft, not a sharp bend |
| packed / spread at 16 SMs | 2.4× (8 GPCs) | 1.32× (`h200g4`), 1.18× (`h200g2`). The part-like expectation at these shapes (§4) is 1.8× and 1.2×; the model is lower because its SMs offer only ~45 B/clk |
| SMs that fill memory | ~17% (87% at 24, 98% at 32) | `h200g4`: spread 32 SMs reach 78% (`run3`); per-SM rates fall as SMs are added, also without any network (risk 6) |

Loads show almost no placement effect. The load-streaming SMs (40 B/clk each) never
offer more than the cap: 16 × 37 = 590 < 646. The DXA is the stronger source.

### 7.4 Why packed runs stay below the configured cap (verification B4)

Gate bugs are fixed (round-robin, credit, wake-up); the runs above use the fixed gate.
The remaining shortfall comes from **lane head-of-line blocking**:

| `h200g4x8`, cap 323 B/clk | lanes U | 4 SMs packed | 8 SMs packed |
|---|---|---|---|
| no network (private ports) | — | 172 (44/SM) | — |
| gate, U = 8 | 8 | 180 | 267 (83% of cap) |
| gate, U = 32 | 32 | 200 | **301 (93% of cap)** |
| no gate, U = 32 | 32 | 202 | — |

- With 8 lanes for 5 lines per clock, a GPC loses 10% already at 4 SMs, well below the
  cap.
- With 32 lanes the loss is ≤ 1% below the cap, and a capped GPC reaches 93% of it.
- Hence U = 32 in the profiles (§4.5).
- Side finding: today's private ports put each SM's DXA traffic on one L2 input (port 0).
  That costs ~15% per SM (44 vs 51 B/clk); the lanes spread it. This matters for the
  per-SM DXA calibration.
- The remaining 7% under the cap, against the part's flat ±0.2%, is open.

### 7.5 Bystanders (`h200g4x8` shape, DXA, per-core B/clk; `src_noc_run4/5`)

Bystanders are 2 SMs in each of GPCs 1–3; the aggressor is GPC 0 with 8 SMs.

| Case | bystanders | aggressor per SM | part (TMA) |
|---|---|---|---|
| bystanders alone | 50.8 | — | 192 GB/s |
| + aggressor GPC with 3 SMs (unsaturated) | 49.1 (−3%) | 43.8 | 180–182 (−6%) |
| + aggressor GPC saturated, cap 323 (offered ~1.2× the cap) | 48.1 (−5%), request-side 48.5 | 28.9 | — |
| **+ aggressor saturated, cap 150 (offered ~2.7× the cap, as the part's 16-SM GPC: 3.1 / 1.28 = 2.4×), return-side** | **35.5 (−30%)** | 17.5 (= 140 B/clk ≈ cap) | **88 same half (−54%) / 106 other half (−45%)** |
| same, request-side gate | 49.2 (−3%) | 17.4 | — |

- **The return-side gate reproduces the bystander effect; the request-side gate does
  not.** This is what the part requires and what the verifier's model predicted (84/86
  return-side vs 178/181 request-side).
- The strength is about half of the part's. There are no partitions yet: no fabric
  queue, no same-half/other-half split.
- The cap had to be lowered to make the aggressor offer 2.4–2.7× its cap with today's
  ~50 B/clk SMs. With calibrated SMs the measured cap (648 / 324) gives that excess by
  itself.
- Loads cannot saturate the cap today (37.6 vs 39.7 alone with the 323 cap).

### 7.6 Ordering with the switch on (B2), `src_noc_run5`

The rule implemented (`sim/simx/noc_order.h`):
- A port's flush token (fence) waits until the port has no request between the switch
  and the L2.
- The port's later requests wait until the token has reached the L2.
- Lane taps report each request's arrival at the L2.
- Same-address order (atomics, LR/SC, a load behind a store, `war_order`'s same-word
  case) holds, because one address always takes one lane.

Results, all **PASSED**, with 2 groups of 2 cores, U = 2, B = 96 B/clk, an L2 and a
20-cycle link:
- `fence`;
- `fence_mp -n64 -m2` (`EXT_A`, `DBG_MEM_BANK0_WDELAY`);
- `flush_coherence -n2048 -r16 -c`;
- `amo` (`EXT_A`);
- `war_order` (h200s, 4 cores);
- `fence` with the request-side gate.

The control `fence` with the switch off also passes. The U = 2 configuration is the one
in which the old round-robin defect served one lane per clock. RTL: the same rule needs
a per-port in-flight counter at the switch and a delivery pulse per lane.

### 7.7 Simulation cost (wall time per kilocycle of kernel; build ~4 min included in wall)

| Run | built / active SMs | wall | kernel cycles | ms per kcycle | peak RSS |
|---|---|---|---|---|---|
| `h200s` loads, all 22 SMs (`noc3_cost_s22`) | 22 / 22 | 2,262 s | 1,012,016 | 2.24 | 512 MB |
| `h200g2` shape, loads, 8 SMs | 32 / 8 | 777 s | 429,955 | 1.81 | 518 MB |
| `h200g2` shape, DXA, 16 SMs | 32 / 16 | 911 s | 394,118 | 2.31 | 793 MB |
| `h200g4` shape, DXA, 16 SMs | 64 / 16 | 1,003 s | 361,590 | 2.77 | 875 MB |
| `h200g4` shape, DXA, 32 SMs | 64 / 32 | 1,540–1,561 s | 428,728–474,722 | 3.24–3.64 | 546 MB |
| `h200g4` shape, loads, 16 SMs | 64 / 16 | 1,734–1,965 s | ~455,000 | 3.83–4.29 | 657 MB |
| `h200g4` shape, loads, 32 SMs (`noc5_g64_bw_pk32` / `sp32`) | 64 / 32 | 4,185–4,449 s | 599,165–643,420 | 6.9–7.0 | — |
| `h200g4` shape, all 64 SMs loads (`noc3_cost_g64`) | 64 / 64 | hit its 1 h 45 min limit (568 MB RSS); re-run on ICE as `noc7_cost_g64` (8 h limit, 2 CPUs), with `noc7_g64_dxa_{pk,sp}{12,24}`. Fetch: `ice/ice_fetch.sh noc7_cost_g64` | | | 568 MB |

- The simulator is single-threaded: TotalCPU equals elapsed time. So 2 CPUs per job
  are enough (for the build).
- Memory is under 1 GB.
- `h200g4` costs about 1.5–1.9× `h200s` per kernel cycle at equal active SMs.
- Loads with 32 active SMs cost 6.9–7.0 ms per kcycle, about 3× `h200s` with all 22 active.
- A full-chip `h200g4` run with all 64 SMs active is estimated at ~3–4× a 22-SM `h200s`
  run of the same kernel, more than 1 h 45 min for this stream. The ICE run will
  give the number.

---

## 8. Decisions (user, 2026-11-21) and implementation order

**Principle (user): "실제 하드웨어와 같게"** (same as the real hardware).

**Decided:**
1. **The GPC limit is a return-side gate** on unique lines (§1.5). Its backlog waits in
   the slices' shared response queues; that is the bystander mechanism. The request-side
   variant stays as a knob, only as a control.
2. **Replication of remote reads is built** (step 3b, §1.6), right after the partitions.
3. **Per-SM rates are calibrated before any placement number is read against the part**
   (decision 4 in the coordinator's list).
   - The SM issue limiter is set at 110–116 B/clk, shared by TMA and loads.
   - The DXA gets its second 64 B global-memory port, so that the limiter, not the
     port, sets the rate.
   - This does not exceed the part. 2 × 64 = 128 B/clk of port capacity sits behind a
     110–116 B/clk limiter, so the SM ingests at most the limiter's rate, which is the
     measured per-SM ingress. One 64 B port (126.7 GB/s at 1.98 GHz) could not reach
     the part's 215–230 GB/s at all.

**Open, as recommended (not decided):**
- **Fabric studies only at full size (144 SMs).** In every scaled shape a side's memory
  is below the fabric's 1.28 TB/s, so the fabric never binds (§4.5).
- **The coalescer, the response-buffer depths and the slice granule** are decided after
  the bystander measurement has been reproduced with partitions (step 3).
  - Buffer depths are fitted once to E-1 and then frozen.
  - The slice granule stays low confidence until then.

**Implementation order.** It starts on a new branch after the integration branch has
merged; nothing is implemented in the repository before then.

| Order | Step | Why in this place |
|---|---|---|
| 0 | per-SM rates: issue limiter + second DXA port (§3.3 step 0) | every knee and every packed/spread ratio depends on the per-SM rate (§7: today's ~45 B/clk SMs put the knee at ~14 SMs instead of ~6–8) |
| 1 | GPC switch with the return-side gate, fence ordering, U = 32 lanes | prototype exists; RTL after step 0 so the gate is calibrated against real per-SM demand |
| 2 | slice-channel binding and the unified address map (power-of-two first; the 24-per-side remap for `h200g4` with it) | needed before partitions, because the partition bit is part of the same address function |
| 3 + 3b | partitions with the fabric, then replication | measured behaviour; the warm-latency target (571) and the same-half bystander case need both |
| 4 | placement control (KMU mask in RTL, `VX_kmu_bus_arb`) | the SimX mask suffices for studies until rtlsim parity is needed |
| after the bystander re-measurement | coalescer (LRC), buffer depths, slice granule | decision 5 |

---

## 9. Responses to the verification (`noc-design-verification.md`, final): fixed or rejected, per item

| # | Item | Status | Where / how |
|---|---|---|---|
| B1 | `h200g2` fabric default hides the GPC limit | **fixed** | B_x = B_g = 648 B/clk, measured (§4.2, §4.5). The consequence is stated: the fabric binds in no scaled shape |
| B2 | fences not ordered through hashed lanes | **fixed in the prototype** | `noc_order.h`: per-port in-flight count, token waits, lane taps. `fence`, `fence_mp`, `flush_coherence`, `amo`, `war_order` pass with the switch on (§7.6). Atomics, LR/SC and same-word order hold by one-address-one-lane |
| B3 | bystander mechanism must be shown | **fixed** | return-side gate (default); SimX reproduces −30% bystander loss with the aggressor at 2.7× its cap, and the request-side control −3% (§7.5). Remaining: about half the part's strength; the same/other-half split needs step 3 |
| B4 | packed GPC plateaus below its cap | **fixed in part** | gate bugs fixed; lane head-of-line identified: with U = 32, 93% of the cap (§7.4); profiles use U = 32. The last 7% is open |
| — | gate round-robin pointer moved inside the scan | **fixed** | the start is fixed per cycle, at most one packet per lane per cycle (`noc_uplink.h`) |
| — | credit discarded each clock | **fixed** | the credit bound is rate + U × line, stated in §2.2 |
| — | the per-SM ingress gate held responses, blocking shared lanes | **fixed (design)** | it is now a request-issue limiter at the socket (§0 item 5, §1.3, §2.2). Not implemented yet (step 1b) |
| — | §4.2 contradiction on same-half bystanders in `h200g2` | **fixed** | "other half only" |
| — | "every response sink accepts unconditionally" | **fixed** | wrong; the deadlock argument is restated (§1.3) |
| — | the "85 cycles" decomposition | **fixed** | ~20 cycles are inside the L2; re-fit per shape (§1.4) |
| — | `VX_mem_xbar` reusable | **fixed** | it zeroes `tag.value`; use `VX_stream_xbar` + a tag insert (§2.1, §3.3) |
| — | SimX/RTL already differ (omega stages at identity, `L2_FWD_HOLD`, LRU) | **fixed (stated)** | §2.1; step 2 makes the omega networks wires in both |
| — | opae/xrt do not use `locate()` | **fixed (stated)** | §2.1, §2.2 |
| — | SimX crossbar ≤ 64 power-of-two outputs; 96 / 48 channels | **fixed (design)** | the crossbar is not instantiated with `SLICE_BIND`; remap with an inverse; interim 32 channels (§4.5) |
| — | memory per SM 1.43× | **fixed** | 1.375× |
| — | 625 KB | **fixed** | 640 KiB; 427 / 465 KiB per SM |
| — | one 64 B port = 126.7 GB/s; 210 GB/s unreachable for a one-port DXA in RTL | **fixed (stated)** | §5 risk 1: ≥ 2 DXA ports or a 128 B port are needed; until then the knees are at ≥ 10 SMs |
| — | "measured" but inferred (knee 6.1–6.2, 22.6 SMs, 528, 1,078, 1.62 TB/s) | **fixed** | marked hard-min arithmetic; 528 / 1,078 / 1.62 dropped; measured roll-off quoted (§4.1) |
| — | 256 B slice granule confidence | **fixed** | "low" |
| — | US11429534 is 3–5 slices per 2 channels | **fixed** | "slice = channel" labelled our simplification |
| — | Accel-Sim `dramid@8` is a channel granule | **fixed** | removed as a slice-granule source |
| — | `h200g2`: what binds; no fabric value keeps both | **fixed** | §4.2 "Plainly", and the study-to-profile table in §4.5 |
| — | asserts compiled out in release builds | **fixed** | `__assert` (tag overflow, gate rate, mask width, order) |
| — | responses use one arbiter shared by all ports (`TxRxCrossBar`) | **accepted, not changed** | existing class; noted as a parity item for step-1 RTL (one arbiter per output) |
| — | the KMU mask is ignored by RTL | **accepted** | SimX-only prototype; RTL is step 4 (`VX_kmu_bus_arb`) |
| — | results not produced by the patch's code | **fixed** | every table names its frozen copy; the patch is regenerated from `src_noc` = `src_noc_run5` |
| — | CP and PTW bypass the switch and gate | **accepted (by design)** | stated in §1.2: they keep their own inputs, outside the GPC gates |
| — | writes charged a line each way | **fixed** | the return-side gate charges responses only; posted writes are not limited, as measured (63 GB/s per SM, no GPC limit) |
| — | MSHR 128 per slice | **fixed** | 256 |
| — | `PLATFORM_MEMORY_PEAK_BW` has no timing effect | **accepted** | documentation only |
| — | "rule total = min(...)" holds only for saturated GPCs | **fixed** | §3.2 |
| — | per-SM ingress is not one constant (4 KB tiles 174–197) | **fixed (stated)** | §3.2 |
