# Figure specifications for `noc-and-design-space.md`

## Conventions for every diagram

- **Nodes** are boxes. **Edges** are written `from -> to : label`.
- **Status tags:**
  - **[EXISTING]**: in Vortex today, or documented on the real part.
  - **[NEW]**: added by the option.
  - **[REUSED]**: an existing module given a new role.
- **Colour:** draw NEW elements in the single accent colour and everything else grey.
- **Arrow styles:**
  - request edges are thin solid;
  - data/response edges are thick solid;
  - completion/barrier edges are dashed.
- **Scope:** each diagram shows one SM pair or group, not all 22 SMs. Use "×N" badges
  for replication.

---

## (a) Vortex SM-to-memory path today (h200s)

**Point of the figure:** there is no topology. Each SM has 4 private fixed-delay
pipes into one flat L2. The DXA uses only one of them. The only SM-to-SM-capable path
is the socket-local DXA LMEM switch.

### Nodes

- `Core` (SM): warps, LSU, DxaUnit, LMEM 228 KB (16 banks × 8 B = 128 B/clk, shared by DMA and LSU). [EXISTING]
- `Dcache` 256 KB, 4 banks, MSHR 512, LSU pending 512. [EXISTING]
- `Icache`. [EXISTING]
- `DXA core` (per socket): 8 workers × 256 lines in flight, 1 gmem port, 1 LMEM write stream 128 B/beat. [EXISTING]
- `Unit MMU` (socket). [EXISTING]
- `Socket arb P0`: priority icache > dcache bank 0 > tcache > rtcache > DXA. [EXISTING]
- `Ports P1..P3`: dcache banks 1–3 straight through. [EXISTING]
- `L2 link ×4`: `VX_mem_bus_delay`, 94 cycles each way, 1 line/cycle, private per port. [EXISTING]
- `L2 input arbiter`: ~90 inputs → 16 banks, round-robin per bank, 2-level tree (12 × 8 → 1). [EXISTING]
- `L2 bank ×16`: 8.65 MB total, pipeline 8, MSHR 256 per bank, pseudo-LRU. [EXISTING]
- `Omega (banks -> 16 mem ports)`. [EXISTING]
- `L3 passthrough`. [EXISTING]
- `Memory crossbar (locate(): 1 KB interleave + hash)`: in the simulator harness, not the RTL. [EXISTING]
- `HBM3E channel ×16` (Ramulator, window 256) + `fixed 304-cycle delay`. [EXISTING]
- `Socket DXA LMEM switch` (route by core id in tag; 1 output when SOCKET_SIZE = 1). [EXISTING]

### Edges

- Core -> Dcache : loads, 1 request per LSU block per cycle
- Dcache -> Socket arb P0 : bank-0 misses, 64 B line
- Dcache -> Ports P1..P3 : bank 1–3 misses
- Icache -> Socket arb P0 : fetch misses
- Core(DxaUnit) -> DXA core : descriptor request (slot, coords, LMEM address, barrier)
- DXA core -> Unit MMU -> Socket arb P0 : line reads, lowest priority
- Socket arb P0 -> L2 link (port 0) : 64 B/clk
- Ports P1..P3 -> L2 links 1..3 : 64 B/clk each
- L2 link ×4 -> L2 input arbiter : "94 cycles"
- L2 input arbiter -> L2 bank : 1 request per bank per cycle
- L2 bank -> Omega -> L3 passthrough -> Memory crossbar -> HBM3E channel : miss, 64 B sector
- HBM3E channel -> (304-cycle delay) -> L2 bank : fill
- L2 bank -> L2 link -> socket port -> Dcache / DXA core : response, retraces path by tag
- DXA core -> Socket DXA LMEM switch -> Core LMEM DMA port : 128 B write beats
- Core LMEM DMA port -> barrier : completion (dashed)

### Annotations

- **Latency budget along the bottom:** L2 hit 273 = 94 + 94 + ~85. DRAM 659 = 188 + 304 + ~167.
- **Callout on DXA → port 0:** "DXA cap = 1 port = 64 B/clk = 127 GB/s (measured 101–111)".
- **Callout on Dcache:** "512 lines / 659 cycles = 49.7 B/clk = 98 GB/s (measured 77)".

---

## (b) Real GPU (H100/H200) hierarchy, as far as known

**Point of the figure:** the hierarchy is SM → TPC → CPC → GPC → crossbar → L2 slice
(2 partitions) → memory controller, with a separate SM-to-SM network per GPC.

Label every element with its tag: FACT, INFERENCE or RUMOR.

### Nodes

- `SM` (unified L1/smem 228 KB, ~128 B/clk; TMA). FACT
- `TPC` = 2 SMs (read speedup 2, no limit: FACT, MICRO'24 and our H200). [corr. 2026-11-21: "mux" is Jin's wording, not NVIDIA's]
- `CPC` = 2–3 TPCs. Name documented (patent, driver), size INFERENCE; no read-bandwidth effect
- `GPC` = 7–9 TPCs (H100 SXM: 8 GPCs, 132 SMs); uplink: [corr. 2026-11-21] ~1.28 TB/s from DRAM ≈ 6 SMs at 210 GB/s (our H200); Jin L2-hit reads: 9 SMs (1/TPC) ≈ 7.8×, 18 SMs ≈ 9× one SM
- `GPC SM-to-SM network` (DSMEM; clusters ≤ 8 portable / 16). FACT
  - "~32 B/clk per SM; receive port shared with L2 fills". RUMOR, label it so.
- `Crossbar` (`xbar`; `gxc` compressor at the GPC side). FACT (Nsight unit names)
- `LRC` (L2 request coalescer, multicast). FACT
- `L2 partition A`, `L2 partition B` (~40 slices each on H100), joined by `ltcfabric`. INFERENCE for the counts, FACT for the names
- `FBPA / memory controller` ×10 (H100 SXM) / ×12 (H200, 6 stacks). FACT
- `HBM stack` ×5 (H100) / ×6 (H200). FACT

### Edges

- SM -> TPC mux : requests
- TPC -> CPC -> GPC uplink : "≈ 1.28 TB/s ≈ 6 SMs' worth per GPC (H200, measured)" [corr. 2026-11-21]
- GPC uplink -> Crossbar : requests and responses
- Crossbar -> L2 slice (near partition) : "~273 cycles hit"
- Crossbar -> L2 slice (far partition, via ltcfabric) : "+160–175 cycles"
- L2 slice -> FBPA -> HBM : miss
- SM <-> SM (same GPC) via GPC SM-to-SM network : DSMEM / TMA `.shared::cluster`, "~180 cycles"
- LRC -> Crossbar -> multiple SMs : multicast duplication at separation points (patent)

### Inset

Per-SM measured values from our H200 data:
- 107 GB/s plain loads; 101 GB/s TMA (16 KB × 4); fair share 36 GB/s.
- "cap location unknown: in-flight vs ingress (E1)".

---

## (c) One diagram per design option: data path of one 16 KB transfer

In each one, draw two SMs: **B** (borrower / memory job) and **L** (lender / compute
job). Add the L2/DRAM column on the right. Number the steps on the arrows.

### (c1) Option 1a — remote DXA writes into the borrower's LMEM

**Point of the figure:** requests go out through L's port; data returns through L and
then over a NEW SM-to-SM path into B's LMEM. B's ingress is the open question.

Nodes:
- B.Core (warp, DxaUnit), B.LMEM (128 B/clk shared), B.port0 [EXISTING]
- L.DXA core, L.port0, L.unit MMU [EXISTING]
- `Group request crossbar` [NEW]
- `Group data crossbar (DXA LMEM writes)` [NEW; REUSED `VX_mem_xbar` / generalized socket switch]
- `Ingress arbiter at B` with a knob "shared with L2 responses / separate" [NEW]
- L2, DRAM [EXISTING]

Edges:
1. B.Core -> Group request crossbar -> L.DXA core : descriptor (~32 B) [NEW]
2. L.DXA core -> L.unit MMU -> L.port0 -> L2 : 256 line reads, lowest priority [EXISTING]
3. L2 -> DRAM -> L2 : miss/fill [EXISTING]
4. L2 -> L.port0 -> L.DXA core : 256 × 64 B [EXISTING]
5. L.DXA core -> Group data crossbar -> Ingress arbiter at B -> B.LMEM : 128 B beats [NEW]
6. B.LMEM -> B barrier : completion (dashed) [EXISTING]
7. B.LMEM -> B consumer (tensor core / LSU) : reads, "shares the 128 B/clk banks" [EXISTING]

Callout: "per-borrower bound = min((1+k)·101, I); I = 127 staged, 253 write-only, 101 if ingress is shared".

### (c2) Option 1b / 4b(ii) — lender prefetches into L2, borrower pulls

Nodes:
- L.DXA core with `prefetch mode (L2PROMO)` [NEW mode on EXISTING unit]
- L2 `prefetch op` [NEW]
- B.DXA / B.LSU, B.port0 [EXISTING]

Edges:
1. L.DXA -> L.port0 -> L2 : 256 prefetch requests [NEW type]
2. L2 -> DRAM -> L2 : fill [EXISTING]
3. L2 -> L.port0 : ack only, no data [NEW]
4. later, B.DXA/LSU -> B.port0 -> L2 : 256 reads, hit [EXISTING]
5. L2 -> B.port0 -> B.LMEM / registers : 64 B lines at 273-cycle latency [EXISTING]

Callout: "helps only if the per-SM cap is in-flight-bound: loads ~2.3× in Vortex, DXA ≤ 1.25×".

### (c3) Option 2 — group pool (pooled DXA with its own uplink ports)

Nodes:
- B.Core, L.Core (each with a small private DXA share) [EXISTING, resized]
- `Group DXA pool` (workers + P_g uplink ports) [NEW; REUSED from the pre-7d6950de5 cluster DXA]
- `Group data crossbar` [NEW]
- L2 [EXISTING]

Edges:
1. B.Core -> Group DXA pool : request [NEW]
2. Group DXA pool -> L2 : on uplink ports 1..P_g [NEW]
3. L2 -> Group DXA pool : data
4. Group DXA pool -> Group data crossbar -> B.LMEM : 128 B beats [NEW]
5. L.Core -> Group DXA pool : the compute job's own small use, with a quota [NEW]

Callout: "cap = P_g × 64 B/clk per group (H200 GPC ≈ 6 SMs' worth, measured) [corr. 2026-11-21]; no lender involved".

### (c4) Option 3b — fat-pipe SM (DXA on all 4 socket ports)

Nodes:
- B.DXA core (16 workers) [EXISTING, more workers]
- B ports 0..3, each with a `dcache-first arbiter` on ports 1..3 [NEW in RTL; EXISTS in SimX as `VX_CFG_DXA_GMEM_PORTS`]
- 4 L2 links, L2 [EXISTING]

Edges:
1. B.DXA -> ports 0..3 -> 4 links -> L2 : 4 × 64 B/clk
2. L2 -> 4 links -> B.DXA -> B.LMEM : ≤ 128 B/clk

Callout: "no other SM involved; P0(b) tests it with zero code".

### (c5) Option 4b(i) — software cluster-TMA borrowing on Hopper (real GPU, today)

Nodes:
- CTA 0 on SM B (consumer, smem buffers, mbarrier) [EXISTING]
- helper CTAs 1..k on SMs L1..Lk in the same GPC (1 thread, ~0 smem) [EXISTING hardware; software role]
- each helper SM's TMA [EXISTING]
- GPC SM-to-SM network [EXISTING]
- L2 [EXISTING]

Edges:
1. helper i -> its TMA : `cp.async.bulk.tensor ... .shared::cluster` dst = CTA 0 buffer, mbar = CTA 0 barrier
2. TMA_i -> L2 -> TMA_i : 16 KB
3. TMA_i -> GPC SM-to-SM network -> SM B smem : data [EXISTING; "receive port shared with L2 fills?" RUMOR]
4. -> CTA 0 mbarrier : complete_tx (dashed)

Callout: "E2 measures SM B's ingest vs k; constraints: same grid, same GPC, helper must be resident".

### (c6) Option 5 — L2-side push engine

Nodes:
- `L2 push engine` (descriptor, uses L2 MSHRs) [NEW]
- L2 banks [EXISTING]
- B socket ports 0..3 (responses) [EXISTING]
- `B push router -> LMEM DMA` [NEW]
- B.LMEM [EXISTING]

Edges:
1. B.Core -> (DXA instruction with "remote engine" bit) -> L2 push engine : descriptor [NEW]
2. L2 push engine -> L2 banks -> DRAM : 256 internal line reads [NEW client]
3. L2 banks -> response network -> B ports 0..3 : "push" packets [NEW packet class on EXISTING wires]
4. B ports -> B push router -> B.LMEM : ≤ 128 B/clk [NEW]
5. B.LMEM -> barrier : completion (dashed)

Callout: "removes every SM-side request limit; ingest is the only cap".

### (c7) Option 6b — co-residency: the memory job's CTA on the compute SM

Nodes:
- SM L: compute CTA (most of the registers and smem) + memory-role CTA (1 producer
  warp + consumer warps, 32–64 KB smem) [EXISTING mechanism]
- L's DXA/TMA, L.port, L's tensor core (shared) [EXISTING]
- `Memory lane` reservation: issue priority + reserved LMEM [NEW, only in the hardware variant 6b+]
- L2 [EXISTING]

Edges:
1. memory-role producer warp -> L.DXA -> L.port -> L2 -> L.LMEM : 16 KB [EXISTING]
2. L.LMEM -> L tensor core -> partial sums : "~5–11% of the SM's tensor rate at 100 GB/s"
3. partial sums -> global/atomic reduce : small
4. compute CTA <-> issue slots : contention, "measured 0.29× for a plain-load stream; TMA unmeasured (E3)" (red/dashed)

Callout: "no SM-to-SM path; uses every SM's R and a sliver of S and C".

### (c8) Option 6a — time-slicing

Draw a timeline, not a datapath:
- the compute job on all SMs;
- decode bursts of 7–9 ms on all SMs interleaved;
- the compute job paused in the bursts.

Label it "no overlap gain".

---

## (d) Comparison

### (d1) Bar chart: memory-job bandwidth on h200s at b = 2 memory SMs (upper bounds, GB/s)

Horizontal line at P = 716 (memory available to the memory job).

Bars:

| Bar | Value |
|---|---|
| baseline | 202 |
| 1a staged (I = 127) | 254 |
| 1a write-only (I = 253, k ≥ 2) | 506 |
| 1a ingress shared | 202 |
| 1b loads (Vortex) | ~460 (2.3×; capped by the per-SM 128 B/clk dcache) |
| 1b DXA | ≤ 254 |
| 3b staged / write-only | 254 / 506 |
| 5 staged / write-only | 254 / 506 |
| 6b loads (r_co 15–29) | 500–716, as a range bar |
| 6b TMA | "unmeasured" hatched bar up to 716 |

- Mark bars that are bounds, not measurements, with hatching.
- Mark the two values that rest on measured inputs (baseline, 6b loads) solid.

### (d2) Table figure (from section 2.8)

- Rows: options.
- Columns: limit attacked, new hardware (none / per-SM / per-group / global), software
  visibility, gain bound, interference, Vortex effort, novelty risk.
- Colour the "kills it" column with the experiment id (E1, E2, E3, P0) that can
  eliminate each row.

### (d3) Decision tree for the exploration order

- E1 result (in-flight-bound?):
  - yes → 1b/4b(ii) live, 2 (MSHR pool) live;
  - no → both dead, 3b/5 live.
- E2 result (consumer ingest scales with helpers?):
  - yes → borrowing works on NVIDIA in software; the hardware story is "residency-free + isolation";
  - no → ingress is the cap; 1a/2/5 are bounded at ~1.0–1.25×; pivot to 6b+ (memory lane).
- E3 result (TMA co-resident ≥ 0.7× solo at ≤ 5% compute loss?):
  - yes → software co-residency is the strong baseline;
  - no → the hardware isolation case is strong.
- P0(a) (Vortex decode consumes ≥ ~250 GB/s per SM?):
  - no → fix the consumer before any fetch-side prototype.
