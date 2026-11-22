# The GPC limit on an H200-class GPU: is there a hardware proposal in it?

Written 2026-11-21. Read-only study: no code edits, no simulations, no GPU jobs.
Inputs: `related-work-placement.md`, `gpu-noc-sources.md`, `tpc-placement-and-borrowing.md`,
`open-threads.md`, `noc-and-design-space.md` (its section 1b has known errors, listed in
`gpu-noc-sources.md`), `noc-model-design.md`, and the raw files of the L2-structure job
in progress (`results_gpu/h200_l2s_*`, job 15976443). `h200-l2-structure.md` does not
exist yet. Every point that depends on it is analysed for both outcomes.

Tags:
- **[meas.]**: measured on our H200s (file named).
- **[raw]**: read by me from raw files of the L2-structure job that is still running.
  Preliminary; that agent's note supersedes it.
- **[lit.]**: literature (source named; figure-read values are ±10%).
- **[inf.]**: my inference or arithmetic. Assumptions are stated.

Symbols:
- r: one SM's ingest, 205-219 GB/s with TMA (239 with plain 16 B loads).
- C: one GPC's cap, 1.28 TB/s from DRAM.
- M: chip memory roof, 4.63 TB/s achieved (4.8 nominal).
- G: 8 GPCs. S: 16.5 SMs per GPC on average.
- 1.98 GHz SM clock, so 1 TB/s ≈ 505 B/clk.

---

## 0. Answer first

**Verdict: not a proposal on its own for H200-class parts.** It is a supporting piece of
an SM-level proposal, plus one measurement that could reopen it.

1. **With good placement the GPC cap costs nothing on today's H200.**
   - The uplinks add up to Σ C = 8 × 1.28 = 10.2 TB/s, which is **2.2 × M**.
   - A memory-bound job spread at ≤ 4 SMs per GPC never reaches C, because 4 × 210 =
     0.84 < 1.28.
   - Spread placement gives 3.08-3.5 TB/s at 16 SMs, 4.03 at 24 and 4.56 at 32, against
     an ideal min(k·r, M) of 3.36 / 4.63 / 4.63 [meas.].
   - GPCPlace (TPC masks) and Fan et al. (NUMA and topology placement) already publish
     the software.
2. **The cap binds only when a job is held in fewer than M / C = 3.6 GPCs.**
   - Bad default placement: MPS packs clients. 8 SMs get 1.27 TB/s against 1.89 spread
     (1.5×), and 32 SMs get 2.56 against 4.43 (1.7×) [meas.]. A software fix exists.
   - Confinement to whole GPCs: 16-SM clusters, GPC-granular isolation of tenants, or
     Green Context cluster mode. A memory job given 2 GPCs is capped at 2.56 TB/s, so it
     loses 45% of the memory roof. To get 4.5 TB/s it needs 4 GPCs (about 66 SMs)
     instead of 24 spread SMs. That is 42 SMs, 32% of the chip's compute, given up for
     uplinks [inf.].
   - Future parts where memory per GPC exceeds the uplink: M / G > C, i.e. memory per
     SM > C / S ≈ 78 GB/s. The H200 has 35 and the B200 54.
     - With H200-class uplinks, a 16 TB/s HBM4 part (8 stacks × 2 TB/s) would stream at
       ≤ 10.2 TB/s (64%) **whatever the placement**. This is the only regime where no
       placement suffices.
     - But Fan et al.'s B200 knee is at 16 SMs, against 6 on the H200 [lit., L2-hit
       test]. That suggests NVIDIA already widened the uplink on Blackwell.
3. **What physically sets C is mostly inferable and points at the GPC side of the
   response path.** It is neither the inter-partition fabric nor DRAM.
   - **Rate, not latency.** L2-resident reads of local copies (near latency ≈ 293 cycles
     against ≈ 545-740 from DRAM [raw]) lift the cap only from 1.28 to 1.27-1.55 TB/s
     [meas.]. An in-flight limit would have given ≈ 2×.
   - **Not the fabric.** One GPC sends only 0.64 TB/s across the fabric, far below the
     ≈ 1.3 TB/s per direction the fabric carries [lit. Fan Fig. 8, reading b in §1.3].
   - **Not the request path.** Writes get more GPC speed-up than reads (≈ 12.4× against
     ≈ 9× of one SM on H100 [lit. Jin]), so the read limit sits on the data-carrying
     return path.
   - **Why ~1/3 of the GPC's ingest.** C is about 1/8 of the L2's aggregate throughput
     (H100-class L2 fabric ≈ 7-8 TB/s at its clocks [lit. Jin, Luo]). The crossbar is
     balanced for an all-GPCs-busy chip, not for one GPC alone. SM ingest (Σ ≈ 27.7
     TB/s) is sized for L1/shared-memory traffic and bursts.
4. **A new observation that could reopen the question (Observation G)** [inf. from
   `h200_tpc_network.txt` per-unit rates; the run was not designed for this]:
   - **G1, cross-GPC interference.** With GPC6 saturated (18 SMs), the 3 unsaturated SMs
     placed in GPC7 got ≤ 96 GB/s each, against ≈ 200 for spread SMs at the same chip
     total (1.6 TB/s). That is about half their rate, from a neighbour in *another* GPC.
   - **G2.** Saturated GPCs that run **together** deliver less than they do alone: about
     0.8-1.15 TB/s each when 3-4 are saturated, against 1.28 alone. At 64 SMs, four
     packed GPCs give ≈ 3.3 TB/s (per-unit estimate); the same 64 SMs spread give 4.63.
   - **Signature:** tree saturation. A saturated GPC's backlog fills shared slice and
     crossbar queues and blocks other GPCs' traffic.
   - **If a fixed-window measurement (E-1) confirms it:**
     - GPC isolation is not real once one GPC saturates. That undercuts GPCPlace's
       "dedicated GPCs" premise.
     - The hardware question becomes "contain a saturated GPC's backlog" (M8, admission
       at the GPC by return credits: cheap), not "lift C".
5. **Mechanisms (§3) ranked by value on our numbers.**
   - **Cross-GPC lending over short side links (M2)** and **finer GPCs (M6)** recover
     1.8-2.0× in the confined regimes. Lending costs linear wire area; M6 doubles the
     crossbar inputs.
   - **Partition-aware allocation (M3)** is software. It is worth 0 for C if the cap is
     GPC-side, but it is necessary on future parts, where the fabric binds first:
     interleaved streaming needs M / 4 per direction, so a ≈ 1.3 TB/s fabric caps M at
     ≈ 5.2 TB/s, and the H200 runs at 89% of that.
   - **Coalescing (M4) and memory-side push (M5)** give ≈ 0 for disjoint streams. Push
     helps the SM-level limits, not C.
   - **Uplink QoS (M7)** gives no bandwidth, only latency isolation when GPCs are shared.
   - **Wider uplinks (M1)** are the oracle and appear to be what NVIDIA shipped on the
     B200.
   - **Backlog containment (M8)**: admit requests at the GPC only within a return-credit
     budget. It costs tens of flops, but it is worth something only if G1 is real; then
     bystanders get ≈ 2×.
6. **Smallest deciding experiments.**
   - Real GPU (≈ 1 h inside a holder job), **E-1**: (a) 2 bystander SMs in each of 7
     GPCs, alone and next to one over-subscribed GPC; (b) 12 SMs in each of g = 1, 2, 3,
     4, 6, 8 GPCs. Both with TMA and with 16 B loads, measured over a fixed time window
     rather than by makespan. Also **E-2**: all 132 SMs, L2-resident on disjoint ranges.
     - Total = min(g × 1.28, 4.63) means the verdict stands.
     - A per-GPC rate that falls with g below the memory roof means a shared crossbar
       stage is the target.
   - Simulator: on the existing SimX GPC-switch prototype (`src_noc`, `VX_CFG_NOC_*`),
     4 groups × 8 cores with the cap at 3 cores' worth, packed against spread, then a
     shared credit pool between two groups as the lending model (about 2 days). It shows
     only what was put in; its use is to cost the lent path (latency, effect on the
     lender, fairness).

---

## 1. What physically sets a GPC's limit

### 1.1 Facts that constrain the answer

| # | Fact | Tag / source |
|---|---|---|
| F1 | One GPC returns ≤ 1277-1285 GB/s from DRAM. The knee is at 4 TPCs: 3 TPCs give 1.20-1.21, 4 give 1.28. Same for TMA (W) and 16 B loads (L16), and for GPC1 and GPC6 (16 and 18 SMs) | meas. `h200_tpc_network.txt`, `h200_tpc_gpcscan.txt` |
| F2 | L2-resident in one GPC: 1.27 (6 SMs), 1.29 (8), 1.34-1.48 (10-14), 1.36-1.55 (16-18). It rises slowly and not monotonically. Units re-read one 24 MB window from staggered start offsets (`tpc_lab.cu:746-748`), so their requests rarely coincide in time | meas. `h200_tpc_gpcscan.txt` |
| F3 | No TPC limit (2 × 205). No CPC limit for reads (any 3 TPCs of GPC6 give 1180-1199) | meas. |
| F4 | One SM: 205-207 GB/s (TMA, 32 KB × 6), 239 (16 B loads), the same on every GPC; ≈ 110 B/clk | meas. |
| F5 | Same-GPC L2-hit curve: ≈ 1.5 TB/s at 6, 2.0 at 12, **2.8 at 18 SMs**. "a buffer sized to exceed L1 but fit within L2", read repeatedly by every SM | lit. Fan et al. Fig. 2(a), figure-read |
| F6 | A per-GPC "LRC" merges up to 4 reads from SMs of one GPC | lit. Accel-Sim 2.0 §II-C (measured). The multicast patent instead calls the LRC "per L2 slice" (US12020035 FIG. 6) |
| F7 | H100 L2-hit read speed-up: GPC_g (18 SMs) ≈ 9× one SM; writes ≈ 12.4×; GPC bandwidth grows with the number of destination memory partitions ("in space", V100) | lit. Jin MICRO'24, Fig. 10 and Obs. 11 |
| F8 | Patent embodiment: crossbar from 6 FBPs × 8 slices × 2 ports; "Each CPC includes two arbiters, each having 4 ports to yield 24 ports of input to each GPC" | lit. US12020035 FIG. 7 (embodiment, not the product) |
| F9 | H200 die: 12 FBPs, 96 L2 slices (8 per FBP), 60 MiB L2, FB bus 6,016 bits, memory clock 3.2 GHz | meas. ncu device attributes, `h200_l2s_ncu.txt` |
| F10 | Two L2 partitions of 4 GPCs and 66 SMs each: {GPC0, 1, 5, 7} and {GPC2, 3, 4, 6}. L2 hit near ≈ 293 cycles (p50) and far ≈ 466 (p75 of the mixed set); DRAM ≈ 545 near and ≈ 740 far | raw `h200_l2s_lat.txt` (my reading: SMs in GPCs 0, 1, 5, 7 see lines warmed by SM 0 or 14 at near latency; SMs in GPCs 2, 3, 4, 6 see a near/far mixture, p50 ≈ 430) |
| F11 | Remote HBM ≈ 1.3 TB/s against local ≈ 2.4; "bottlenecked by the LTC fabric" (H200). The SM set is not stated | lit. Fan Fig. 8, Takeaway 8 |
| F12 | Packed vs spread at equal SM count: 16 SMs 1.28 vs 3.08 TB/s; 32 SMs 2.28 vs 4.56; 64 SMs (pair-packed in 4 GPCs) 2.77 vs 4.63 | meas. |

Side notes:
- F9: 6,016 / 6,144 = 0.979, and 141 / 144 GB = 0.979. So one 128-bit slice of one HBM3e
  stack appears to be disabled [inf.].
- F9: 6,016 b × 6.4 Gb/s / 8 = 4.81 TB/s, consistent with the 4.8 TB/s nominal.

### 1.2 Request path or response path, rate or in-flight

**Rate-limited, not latency-limited** [inf. from F1, F2, F10].
- If the cap were a per-GPC limit on outstanding requests N_out, then bandwidth =
  N_out × line / latency.
- L2-resident local-copy reads have about half the latency of DRAM reads: 293 against
  545-740 unloaded [raw]. They should then reach ≈ 2-2.5 TB/s.
- They reach 1.27-1.55. So at most a small in-flight component exists: the slow rise
  past the knee in F2. The dominant term is a byte rate of ≈ 650 B/clk.

**The return direction carries the limit for reads** [inf. from F7].
- A read moves data on the response path; a write moves data on the request path and
  gets a small acknowledgement back.
- H100 writes get 12.4× one SM; reads get ≈ 9×.
- So the request path is not narrower than the response path. It is either wider, or the
  two share nothing.
- A read stream (our case) is limited on the L2 → GPC direction.

**Width candidates.** 650 B per SM clock decomposes, among other ways, as:
- (a) 5 × 128 B (five 128 B response ports at the SM clock);
- (b) 20 × 32 B sectors per clock;
- (c) 24 × 32 B ports at a crossbar clock of ≈ 1.68 GHz. That is F8's "24 ports of input
  to each GPC": 768 B/clk × 1.68 GHz ≈ 1.29 TB/s.

These are consistent with F1, not evidence for any one of them. Nothing we measured
separates (a)-(c). The crossbar/L2 clock of GH100 is not documented in the sources read.

**Not split by CPC.** If the 24 ports of F8 belonged 8 per CPC, then 3 TPCs packed in
one CPC would get ≤ 1/3 of 1.28 = 0.43 TB/s. Every 3-TPC subset gave 1.18-1.20 [meas.].
So any CPC-level partition of the return ports is not visible for reads. This agrees
with Jin's "CPC has no impact on reads".

**"In space" (F7).** On V100, GPC bandwidth grew 218% when the destinations went from 1
to 4 memory partitions. So at least part of a GPC's capacity is per-destination lanes,
not one pipe. Our DRAM stream targets all 12 FBPs, the most favourable case. Whether the
H200 cap is one pipe or 12 lanes of ≈ 107 GB/s matters for lending (§3, M2) and is
measurement E-4.

### 1.3 The inter-partition fabric

Fan's "remote ≈ 1.3 vs local ≈ 2.4 TB/s" (F11) can be read three ways. Our chip roof
tests each one [inf.].
- **(a) All 132 SMs read remote-only pages and get 1.3 TB/s in total.** Then the fabric
  carries ≈ 0.65 TB/s per direction. Normal 4 KB-interleaved streaming sends half of
  every byte across, i.e. M / 4 per direction. That would cap the chip at
  4 × 0.65 = 2.6 TB/s. **We measure 4.63, so (a) is ruled out.**
- **(b) The SMs of one partition (66) read local-only pages at 2.4 and remote-only pages
  at 1.3.**
  - 2.4 is exactly half the nominal memory (3 of 6 stacks).
  - 1.3 is then the fabric's capacity in one direction.
  - Interleaved full-chip streaming needs 4.63 / 4 = 1.16 TB/s per direction: **89% of
    the fabric**. Consistent with our 4.63, and it means the fabric is sized just above
    HBM.
- **(c) A small SM set (one GPC).** Then 1.3 would be the GPC cap again and says nothing
  about the fabric. It cannot be excluded from the paper's text.

**For one GPC, the fabric is not the cap under (b).**
- A GPC at 1.28 TB/s sends 0.64 TB/s across, half the fabric.
- The local half (0.64 TB/s) is served by 3 stacks (2.4 TB/s).
- The cap must therefore sit on the GPC side: GPC port, crossbar input, or a per-source
  limit at the slices.
- Under (c), a per-GPC remote-path limit of ≈ 0.64 TB/s could instead produce the whole
  1.28 cap, because a 50/50 stream gives ≤ 2 × 0.64. Our F2 argues against it:
  - after warm-up, every line of the 24 MB window has a local copy (near latency for
    self-warmed lines everywhere [raw]);
  - so L2-resident reads would bypass the remote path and rise well above 1.28;
  - they rise only to 1.27-1.55.

**The pending local-only vs remote-only measurement, both outcomes:**

| Outcome per GPC (DRAM, 16 SMs, 4 KB-aligned page sets) | Meaning | Consequence |
|---|---|---|
| R1: local-only ≈ remote-only ≈ 1.28 | GPC-side cap; the fabric does not bind for one GPC | Mechanisms on the uplink (M1, M2, M6) are the only levers for C. Partition-aware allocation (M3) is worth 0 for C but matters for chip-level fabric headroom |
| R2: local-only > 1.28 (e.g. 1.6-2.5), remote-only ≈ 0.64 | A per-GPC remote-path limit makes the cap; the GPC port is wider than we think | M3 (pure software, Fan's remap trick) lifts single-GPC bandwidth to the local-only value; the hardware case weakens further. Also contradicts F2 unless the L2-resident run kept far copies, so recheck F2 |
| R3: remote-only per GPC ≈ 1.28 but one partition's 4 GPCs remote-only ≈ 1.3 in total | Fabric ≈ 1.3 per direction (reading b) | Fabric at 89% for full-chip streaming today: the first thing to bind on a faster-memory part (§2.5) |

### 1.4 Why a designer would size it at ~1/3 of the GPC's ingest

The framing "the uplink is 37% of what its SMs can ingest" (1.28 / (16.5 × 0.21)) is the
wrong denominator [inf.].
- **Balance with the L2.** Σ C = 10.2 TB/s (DRAM-sourced), or ≈ 11-12 L2-resident.
  - Whole-chip L2 throughput on H100-class parts is ≈ 7 TB/s (Jin Fig. 9(a),
    figure-read) and ≈ 7.8 TB/s (Luo: 4472 B/clk × 1.755 GHz, H800).
  - At the H200's 1.98 GHz and 96 slices, ≈ 9-12 TB/s is plausible (unmeasured; E-2
    measures it).
  - So each GPC's port ≈ its 1/8 share of the L2. A symmetric crossbar with no
    over-provisioning for imbalance is the natural choice when the design target is the
    whole chip busy (training, large GEMM, prefill).
- **Balance with DRAM.** Σ C / M = 2.2. Under the target workload every GPC streams
  M / 8 = 0.58 TB/s from DRAM, 45% of C. The rest of C serves L2 hits (2.4-3.5× DRAM in
  aggregate, Jin p6).
- **SM ingest is sized for something else.** 110 B/clk per SM ≈ the L1/shared-memory
  fill width (L1 125.8 B/clk/SM, smem 127.9, Luo Table V). Σ over 132 SMs = 27.7 TB/s =
  6 × M. No crossbar is provisioned for that; the L2 could not serve it either.
- **Wire area** [inf., order of magnitude; assumptions: 650 B/clk data plus ~10% ECC and
  control per direction, two directions, effective 100 nm routing pitch on two
  semi-global layers]:
  - ≈ 11-12 K wires per GPC, a routing channel of ≈ 0.6 mm per GPC;
  - ≈ 4.6 mm for 8 GPCs, crossing to the L2 on a ≈ 28 mm die edge;
  - doubling C adds that again.
- **Crossbar area.** For a wire-dominated matrix crossbar, area ∝ N_in × N_out × w².
  - Doubling port width w costs ≈ 4× crossbar area; doubling the number of GPC ports
    costs ≈ 2×.
  - US12020035 speaks of "quadratic escalation of the crossbar costs".
- **Energy** is per byte moved, not per wire. A wider idle port costs area and leakage,
  not dynamic energy.

**What the B200 suggests.** Fan et al.: "When the number of SMs exceeds 6 on H200 and 16
on B200, their L2 bandwidth becomes lower than that of SMs in different GPCs."
- At the same per-SM rate, a B200 GPC port is ≈ 16 SMs' worth, about 2.7× the H200's
  ratio and close to non-blocking for its GPC.
- Caveat: an L2-hit test, possibly inflated by F6 coalescing (but the H200 knee matches
  ours).
- So the vendor already moved in the direction a "wider uplink" proposal would argue for.

### 1.5 The Fan conflict (F2 against F5)

| Hypothesis | Predicts | Status |
|---|---|---|
| H-a: every SM reads the same buffer in near lockstep, so the GPC-side LRC merges up to 4 requests and replicates the response inside the GPC; the uplink carries one copy | Fan's curve > ours; ours (staggered offsets) shows little merging | Most consistent [inf.]. It also implies that replication happens below the uplink, i.e. the uplink cap is on wire bytes |
| H-b: merging happens at the slice (patent "per L2 slice LRC"), responses still go out once per SM | No uplink gain from sharing; Fan's 2.8 then needs another cause | Possible |
| H-c: an L2-hit uplink of ≈ 2.8 really exists, and the DRAM-sourced 1.28 is a per-source-GPC miss-credit limit at the slices/FBPs | Disjoint L2-resident would reach ≈ 2.8 | Contradicted by F2 (1.27-1.55 with local copies) unless our L2 test was flawed. **If true, this is the cheapest and strongest proposal (M2b, credit sharing)** |
| H-d: per-SM L2-hit rate differs (Fan ≈ 250 GB/s per SM) | A different knee position, not a different plateau | Does not explain a plateau of 2.8 against 1.5 |

### 1.6 Measurements that would discriminate (real H200, TPC masks, no root)

Shared protocol for all of them:
- Every TMA run is paired with its 16 B-load counterpart.
- Record NVML SM and memory clocks per run (Kuruppu: the clock moves with data movement).
- Report **instantaneous rates over a fixed window**: sample bytes landed per SM with
  `%globaltimer` every ~10 µs, take the middle 60% of the overlap. Makespan aggregates
  hide straggler GPCs (see §2.2).
- Use the GPC map of the die in use (`h200_l2s_gpc.txt`).

| Id | Question | Design | Discriminates |
|---|---|---|---|
| E-1 | Do saturated GPCs interfere? | (a) **Bystander arm:** 2 SMs in each of 7 GPCs alone, then the same 14 SMs plus 16 SMs saturating the 8th GPC; also with the saturating GPC throttled to 6 SMs (at its cap, not beyond). (b) 12 SMs (6 TPCs) in each of g = 1, 2, 3, 4, 6, 8 GPCs, choosing g/2 GPCs from each partition (g = 1: one on each side separately). W and L16; DRAM-resident disjoint ranges | (a) Bystander rate ≈ alone: G1 not real. Bystanders drop when the GPC is over-subscribed (16 SMs) but not at exactly 6: backlog / tree saturation, so M8 is the target. (b) Per-GPC rate constant at ≈ 1.28 and total = min(g·1.28, 4.63): private port, verdict stands. Per-GPC rate falling with g while total < M: a shared stage |
| E-2 | Is C the chip's L2 bottleneck? | All 132 SMs, each a disjoint 128 KB-aligned slice of a 30 MB L2-resident buffer, warm by the same SM; then 8 SMs per GPC only | Aggregate ≈ 8 × 1.4-1.55 ≈ 11-12 TB/s: uplinks bind L2 too (balanced crossbar). Clearly lower: L2 slices bind and wider uplinks help only imbalance |
| E-3 | Fan reproduction | One 18-SM GPC, 2-18 SMs, L2-resident: (i) same 24 MB buffer, synchronized start; (ii) same buffer, staggered (ours); (iii) disjoint 1.3 MB per SM. Plus ncu `lts__t_requests`, `lts__t_sectors` per run | (i) ≫ (ii) ≈ (iii): coalescing (H-a/H-b; the uplink counter tells which). (iii) ≈ 2.8: H-c, our F2 wrong |
| E-4 | One pipe or per-destination lanes ("in space") | One GPC, 16 SMs, L2-resident lines restricted to d ∈ {1, 2, 3, 6, 12} FBPs. Use the H200 hash from the L2-structure job; GemAtlas's H20 equation as a start, validated by per-line latency clustering | B(d) saturates at d ≈ 2 (1 FBP ≈ 8 slices × 64 B/clk ≈ 1 TB/s): one GPC-side pipe. B(d) ∝ d to d = 12 (≈ 107 GB/s per lane): per-destination lanes |
| E-5 | Request rate or bytes | One GPC, 16 SMs, the same byte volume as (i) 128 B lines, (ii) 32 B sectors scattered one per line | Bytes the same: width-limited. Bytes ≈ 1/4: transaction-rate-limited (then M5 push and request batching matter) |
| E-6 | Duplex | One GPC: read-only, write-only (TMA store `cp.async.bulk.global.shared`), 50/50 mixed | Mixed ≈ 2 × read: independent directions. Mixed ≈ read: a shared transaction resource |
| E-7 | Fabric (pending agent) | Local-only vs remote-only 4 KB page sets (bit 12 remap, Fan's trick) for (i) one GPC, (ii) one partition's 4 GPCs, (iii) all SMs; read `ltcfabric__*` counters if exposed | R1/R2/R3 of §1.3 |
| E-8 | Clock domain of the cap | C in B per SM clock at two SM clocks | Needs clock locking (root); ask the admins. Constant B/clk: SM-clock domain. Constant GB/s: crossbar or L2 clock |

---

## 2. When the limit costs something once placement is good

### 2.1 Model

- B(job) = min(Σ over GPCs g of min(k_g · r_eff, C_g), M, fabric term).
- r_eff falls from 205 to ≈ 190 once the chip total passes ≈ 3 TB/s (loaded latency
  [meas.]).
- The cap binds for job j only if some GPC holds more than C / r ≈ 6 of its streaming SMs.
- Overall: **GPC-bound iff the job's SMs lie in g < M / C = 3.6 GPCs** (H200 numbers).

### 2.2 Scenarios (TB/s; measured where marked)

| Scenario | Placement | Job gets | Ideal min(k·r, M) | Cost of the GPC cap | Fixed by software? |
|---|---|---|---|---|---|
| (i) ideal spread, k = 8 / 16 / 24 / 32 | ≤ 4 per GPC | 1.61 (W) and 1.89 (L16) / 3.08 and 3.5 / 4.03 / 4.56 [meas.] | 1.68 / 3.36 / 4.63 / 4.63 | **0**: losses are r_eff and the DRAM roof | — |
| (ii) MPS-style packing | fill GPCs | k = 8: 1.27; 16: 1.28; 32: 2.56 (two caps) [meas. L16 under MPS] | 1.89 / 3.5 / 4.43 | 1.5× / 2.7× / 1.7× | yes: TPC mask or Green Context, as GPCPlace does |
| (iii-a) beside a compute job, SM-granular | memory 24-32 spread; compute on the rest | 4.03-4.50; compute keeps 0.82-0.76 of the all-SM rate and 0.986-1.002 on its own SMs [meas.] | 4.63 | **0**; the cost is the 24-32 SMs (18-24%) | — |
| (iii-b) beside a compute job, GPC-granular (compute takes whole GPCs, e.g. 16-SM clusters or isolation) | memory in g GPCs | g = 2: ≤ 2.56 (2.28 measured for GPC6 + 7, 32 SMs); g = 3: ≤ 3.84, but ≈ 3.0-3.2 estimated (§2.3) | 4.63 | g = 2: 45%. To reach 4.5 the job needs 4 GPCs (~66 SMs) instead of 24 SMs: 42 SMs = 32% of compute | only by giving up GPC granularity |
| (iii-c) beside a CUTLASS FP8 GEMM, SM-granular | 30 memory SMs, GEMM on 100 | 3.66-3.73 (GEMM's own traffic); GEMM −10% [meas.] | — | 0 from the GPC cap | — |
| (iv) T tenants, each GPC-isolated, memory phases with duty p | 8 / T GPCs each | all streaming: M / T (roof-bound; uplink irrelevant). One streaming alone: (8/T) × 1.28 | M | T = 4 (2 GPCs each): 45% in solo phases. Expected loss in streaming phases ≈ (1−p)^(T−1) × 45%: p = 0.3 → 15%, p = 0.1 → 33% [inf.] | yes, if tenants accept sharing GPCs: disjoint TPCs beside compute-bound work cost 0.997-1.001 [meas.]. GPCPlace's 2.42× latency penalty arises only when the co-tenant is memory-heavy |
| (v-a) HBM4 part, 16 TB/s, 8 GPCs, H200-class C | any | ≤ 10.2 (64%) | 16 | **36%, no placement helps** | no |
| (v-b) same, fabric at H200 ratio (1.3 per direction) | interleaved | ≤ 4 × 1.3 = 5.2 | 16 | the fabric binds first (fabric must scale ≈ 3×) | partly: partition-local allocation (M3) removes fabric traffic for disjoint streams |
| (v-c) larger GPCs (S = 24), C ∝ L2 share | packed k = 16 | one GPC: C | — | packing loss grows with S; the spread case is unchanged as long as Σ C ≥ M | yes |

Critical ratios per part [inf.; C assumed H200-class unless stated]:

| Part | M (TB/s) | SMs | M per SM | Σ C / M with H200-class C | Binds at chip scale? |
|---|---|---|---|---|---|
| H100 SXM | 3.35 | 132 | 25 GB/s | 3.1 | no |
| H200 | 4.63 achieved | 132 | 35 | 2.2 | no |
| B200 | 8 nominal | 148 | 54 | ≈ 1.3 if C were H200-class (unknown GPC count) | no; and Fan's knee at 16 suggests C was widened |
| HBM4, 8 stacks | 16 (JEDEC HBM4: 2048 b × 8 Gb/s ≈ 2 TB/s per stack) | ~200 (assumed) | ~80 | 0.64 | **yes**: M per SM ≥ C / S ≈ 78 GB/s |

### 2.3 Observation G: saturated GPCs together deliver less than alone

Source: `h200_tpc_network.txt`. W kernel, per-unit averages; [inf.]: the run was not
designed for this.

| Run | GPCs (SMs) | Slowest unit (GB/s) → that GPC's rate | Fastest unit | Estimated total while all run | Σ min(cap, demand) | Same SM count spread |
|---|---|---|---|---|---|---|
| 21 pair-packed | GPC6 (18), GPC7 (3) | 71.3 → 1.28 | 96.1: GPC7's 3 SMs are **not** saturated, yet get < 1/2 of the 205 they would get alone | ≈ 1.6 | 1.9 | 3.64-3.75 |
| 32 pair-packed | GPC6 (18), GPC7 (14) | 71.4 → 1.28 | 83.7 → GPC7 1.17 | ≈ 2.45 | 2.56 | 4.56 |
| 48 pair-packed | GPC1 (12), GPC6 (18), GPC7 (18) | 58.6 → ≈ 1.05-1.15 | 71.5 → GPC1 ≈ 0.86 | ≈ 3.0-3.2 | 3.84 | 4.60 |
| 64 pair-packed | GPC1 (16), GPC2 (12), GPC6 (18), GPC7 (18) | 43.2 → ≈ 0.78 | 71.5 | ≈ 3.3 | 4.63 (roof) | 4.63 |
| 32 single-packed | GPC1 (8), GPC2 (6), GPC6 (9), GPC7 (9) | 111 → ≈ 1.0 | 140 | ≈ 3.7 | 4.63 (roof) | 4.56 |

- **G1, bystanders slow down.** In the 21-SM run the chip moves only 1.6 TB/s. Spread
  SMs at that load run at ≈ 200 GB/s (8 spread: 1.61-1.89 TB/s). Yet GPC7's 3 SMs, below
  any cap, ran at ≤ 96. The only thing they share with GPC6 is the crossbar, the slices
  and the DRAM.
- **G2. Up to 2 GPCs: each saturated GPC reaches its cap. From 3 saturated GPCs: none
  does**, while the chip total stays below the memory roof.
- The makespan metric reflects the slowest unit. Units in fast GPCs finish early and the
  slow ones then speed up. So the totals during full overlap are, if anything, lower than
  estimated.
- The partitions are balanced in the 64 run (34 against 30 SMs), so a partition limit
  does not explain it.
- Candidates:
  - (0) tree saturation, which explains G1 and G2 together. A GPC whose return port is
    the bottleneck has ≈ 18 × 192 KB = 3.5 MB of TMA requests outstanding, and they wait
    at shared queues:
    - in a saturated GPC, latency ≈ in-flight / C ≈ 2.7 µs, consistent with the 71 GB/s
      per SM;
    - if those requests wait at the slices or the crossbar outputs, other GPCs' requests
      queue behind them (Pfister & Norton's "hot spot", 1985);
  - (1) head-of-line blocking in an input-queued crossbar. Uniform traffic from
    saturated inputs saturates such a switch near 58.6% (Karol et al. 1987); here 3.3 /
    (4 × 1.28) = 64%;
  - (2) unfair arbitration at the slices, which favours GPCs with fewer pending requests;
  - (3) DRAM efficiency with ≈ 400 concurrent 32 KB streams. Unlikely: spread 64 SMs run
    the same number of streams at 4.63.
- If E-1 confirms this:
  - the cost of packing grows with chip load;
  - many-tenant scenario (iv) is worse than the model says (≈ 0.8 instead of 1.16 per
    2-GPC tenant when 4 stream);
  - there is a hardware question that placement only avoids.

### 2.4 Is there a regime where no placement suffices?

- **Today (H200): no**, provided SM-granular placement across GPCs is allowed and the
  job's kernels can be re-placed at kernel boundaries (μs; LithOS does it per kernel).
  Σ C = 2.2 M.
- **When the job is GPC-confined by the program or the operator:** placement cannot
  help by definition; only C per GPC or lending can. Examples:
  - a 16-SM cluster kernel with fewer clusters than GPCs;
  - GPC-granular isolation for latency SLOs.

  MIG is **not** such a case: it partitions memory slices too. A 1g slice gets ≈ 1/7 of
  memory (≈ 0.69 TB/s), below C.
- **Future parts with M per SM ≥ C / S:** no placement suffices at full load.
  - Vendors appear to scale C (B200).
  - So the research question becomes "provision or share", and the evidence for "share"
    is the imbalance probability of §2.2 (iv).

### 2.5 Fabric, for completeness

- Interleaved streaming sends half of every stream across, M / 4 per direction.
- Under reading (b) the H200's fabric (≈ 1.3) is 89% used at 4.63 TB/s.
- A job confined to one partition is capped at ≈ 2.3 TB/s (local 3 stacks) + 1.3
  (fabric) only if its data is interleaved. With partition-local allocation it is capped
  at 2.4 (local stacks) and uses no fabric.
- A 2-GPC memory job should take **one GPC per partition**. Two GPCs of one partition
  send 1.28 TB/s one way across the fabric, at its limit. Placement rules must include
  the partition (F10 gives it).

---

## 3. Mechanisms

Shared assumptions:
- Gains come from the scenario numbers of §2.2. S1 = MPS-packed 16 SMs (1.28 against
  3.08-3.5 spread); S2 = GPC-granular co-run with the memory job in 2 GPCs (≤ 2.56
  against 4.63); S3 = 4 GPC-isolated tenants, one streaming (≤ 2.56 against 4.63);
  S4 = ideal spread (gain 0 for every mechanism); S5 = HBM4 16 TB/s part.
- "Vortex needs": what SimX/RTL must have to evaluate the mechanism. Vortex today has 4
  private 94-cycle links per socket into one flat L2. A SimX prototype of a GPC switch
  with lane count, byte gate and two latencies, plus a KMU launch core mask, exists in
  `vortex-exp/src_noc` (`notes/noc_model.patch`; knobs `VX_CFG_NOC_GROUP_SIZE`,
  `_UPLINK_PORTS`, `_UPLINK_BYTES`, `_SM_LATENCY`, `_UPLINK_LATENCY`). There are no
  partitions, no inter-partition link and no remote copies yet (steps 2-3 of
  `noc-model-design.md`).
- Per-SM ingest in Vortex is ~104 GB/s (DXA) against 210 on the part. Until that
  calibration lands, express every cap in SMs' worth.

### M1. Wider (statically over-provisioned) uplinks

- **Data path:** unchanged; C → αC.
- **Hardware:**
  - α× GPC-side response wires and crossbar input width;
  - crossbar area ≈ α² (width) or α (more ports);
  - ≈ 0.6 mm of routing channel per GPC per 1× [inf.].
- **Who pays:** every part, in area and leakage, all the time.
- **Gain (α = 2):**
  - S1: 1.28 → 2.56 (2.0×);
  - S2/S3: 2.56 → 4.63 (1.8×);
  - S4: 0;
  - S5: 10.2 → 16 (roof).
- **Vortex needs:** nothing new (`VX_CFG_NOC_UPLINK_BYTES`).
- **Risks:** "just provision more"; the B200 appears to have done it (Fan's knee at 16).
- **Prior work:** the vendor (B200); Jin MICRO'24 (characterization); Kim et al. PACT'20
  (the NoC-memory interface as the bottleneck).
- **Role:** oracle upper bound for every other mechanism.

### M2. Cross-GPC lending of idle uplink capacity

- **Data path (M2a, wire lending):**
  1. A request from SM s in GPC A goes up A's own uplink. Requests are small; no
     lending is needed on that path.
  2. The crossbar returns the response on GPC B's uplink when A's is out of credit and
     B's has spare credit (B = a physical neighbour).
  3. The response then crosses a new short **A↔B side link** into A's GPC switch.
- **M2b, credit lending.** Only if E-3/E-7 show H-c: the limit is per-source-GPC miss
  credits at the slices, not wires. Then credits are pooled in the L2 and no wire is
  added.
- **Hardware (M2a):**
  - per GPC pair, a side link of width w_s (e.g. 0.5-1 × C) plus a 2-input merge at the
    receiving GPC switch;
  - a credit exchange between the two GPCs' byte gates (a few bits per cycle);
  - response tags that carry the "via B" route.
  - The wires are short (adjacent GPCs) instead of the long GPC-to-L2 run, so area grows
    linearly in w_s, with no crossbar ports.
- **Who pays:**
  - the lender GPC's idle uplink;
  - extra hop latency on lent responses (≈ 20-40 cycles [inf.]);
  - ≈ 0.45 pJ/bit more on lent bytes (3 mm at ≈ 0.15 pJ/bit/mm, rule of thumb [inf.]):
    ≈ 4.6 W at 1.28 TB/s lent, ≈ +10-15% over the HBM access energy per byte.
- **Gain:**
  - S1, one neighbour lends 1.28: 1.28 → 2.56 (2.0×); with two neighbours,
    16 × 190 ≈ 3.0 (2.4×).
  - S2: the compute GPCs lend (DiT uses ≈ 0.6 GB/s per SM [meas.]); two lenders → 4.63
    (1.8×).
  - S3: 1.8× in solo phases.
  - S4: 0. S5: 0, because nobody is idle.
- **Vortex needs:**
  - a shared credit pool between two groups' byte gates in `noc_uplink.h`;
  - a side-link `DelayBuffer` for the response path, with routing by tag through the
    lender's switch;
  - the KMU core mask (exists);
  - a compute role that leaves its group's uplink idle (`gemm_block` compute role; check
    its own traffic in Vortex).
  - Effort: SimX ≈ 1 week; RTL: `VX_noc_rate_gate` with a borrow port, plus
    `VX_mem_bus_delay` for the side link, ≈ 2-3 weeks.
- **Risks:**
  - if E-4 shows per-destination lanes, lending must be per destination;
  - ordering of responses that take two routes (Vortex tags per request, so it is fine
    there; NVIDIA unknown);
  - borrowing from a lender that wakes up (needs fast revoke: a credit-based design
    revokes in one round trip);
  - relevance depends on GPC-confined jobs existing.
- **Prior work:** none found for GPC uplink lending (`related-work-placement.md` §10
  direction 2). Adjacent ideas: NoC bandwidth borrowing and adaptive routing in CMP
  NoCs; Ibrahim et al. PACT'19 (remote-core bandwidth, which exploits locality, not
  spare links); LithOS TPC stealing (compute, not links).

### M3. Partition-aware allocation and placement (software plus a small hardware hint)

- **Data path:** pages of a stream are placed in the partition of the SMs that read
  them. No traffic crosses the fabric.
- **Hardware:** none, or a per-allocation partition selector, as Blackwell's driver
  exposes for locality domains (Fan; SciTix B300).
- **Who pays:**
  - capacity balance (one partition's 70 GB can fill);
  - a kernel's SM-to-data mapping must be static;
  - for 4 KB interleave inside 2 MB pages, Fan's remap wastes or compacts address space.
- **Gain:**
  - for C, R1 gives 0 and R2 gives local-only/1.28;
  - chip-level fabric headroom: today 89% → 0% fabric use for disjoint streams;
  - S5: removes the 5.2 TB/s fabric bound (v-b);
  - a 2-GPC job in one partition: fabric-bound at ≈ 1.3 one-way → local.
- **Vortex needs:**
  - steps 2-3 of `noc-model-design.md` (slice = channel, unified address map, 2
    partitions, xlink with its own byte gate);
  - an allocator hint in the runtime (a partition-colored megapage heap; the megapage
    heap `VORTEX_VM_MEGAPAGE_HEAP=1` is required for valid runs);
  - the remote-copy option for L2 latency fidelity.
- **Risks:** already published (Fan: up to 1.22×); it is a baseline, not a contribution.
- **Prior work:** Fan et al.; SciTix (B300 locality domains); Joo et al. (MI300X);
  NUMA-GPU work (Milic et al. MICRO'17, MCM-GPU ISCA'17).

### M4. Request coalescing / stream-aware batching at the GPC

- **Data path:** requests for the same line from SMs of one GPC within a window merge
  (an LRC extended to longer windows or across kernels). One response crosses the
  uplink and is replicated inside the GPC.
- **Hardware:** a merge table in the GPC switch (CAM of pending lines), plus response
  fan-out.
- **Who pays:** latency on every miss (CAM lookup), area of the merge table.
- **Gain:**
  - **0 for disjoint streams** (decode GEMV: every SM reads its own weights);
  - for shared operands (activations, KV reused by several CTAs, several tenants
    serving the same weights from the same physical pages), up to 4× on the shared bytes
    (F6). Decode activations are kilobytes against megabytes of weights, so ≈ 0.
- **Vortex needs:** a merge table in the group switch (`cluster.cpp`); workloads with
  sharing.
- **Risks:** Hopper already has it (F6, TMA multicast); it explains Fan's 2.8 rather than
  lifting our 1.28.
- **Prior work:** Accel-Sim 2.0 (LRC); US12020035 (multicast); classic MSHR merging.

### M5. Memory-side push path (L2 or FBP streams into a GPC without per-line requests)

- **Data path:** a descriptor goes to an L2-side engine. The engine reads lines and
  pushes them as write packets on the response network into the SM's shared memory
  (`noc-and-design-space.md` Option 5).
- **Hardware:** a descriptor engine per partition or FBP group; a push packet class; a
  push router per SM into shared memory.
- **Who pays:** L2 MSHRs and engine area; a new packet class (deadlock and ordering
  analysis).
- **Gain for C: 0** if the cap is on the response path's bytes. §1.2 says it is: E-5
  decides. Pushed bytes cross the same uplink.
  - It lifts C only with a dedicated injection port, which is M1 in disguise.
  - It does help SM-level limits: request machinery, and staging when it writes straight
    to the consumer.
  - If E-5 finds a transaction-rate limit, push or request batching (one request per 4
    KB) becomes the cheapest lever on C.
- **Vortex needs:** an engine as an L2 client; push tags; a socket push router to LMEM.
  SimX 1.5-2 weeks, RTL 3-4 weeks (`noc-and-design-space.md`).
- **Risks:** solving the wrong limit.
- **Prior work:** D2MA (PACT'14), DAC (ISCA'17), CudaDMA (SC'11), NVIDIA's multicast path.

### M6. Reshaped GPCs (smaller GPCs, more ports)

- **Data path:** unchanged. 16 GPCs of 8-9 SMs, each with an H200-sized port: Σ C =
  20.5 TB/s.
- **Hardware:** 2× crossbar inputs (area ≈ 2×), 2× uplink wiring.
  - Clusters are limited to 8-9 SMs. Portable clusters are already ≤ 8; the opt-in
    16-SM cluster is lost.
  - The DSMEM network splits in two.
- **Who pays:** every part, in area; software that uses 16-SM clusters.
- **Gain:**
  - S1: the job occupies two small GPCs → 2.56 (2.0×);
  - S2/S3: GPC-granular jobs get finer granularity, e.g. 4 small GPCs = 34 SMs at
    4 × 1.28 = 5.1 → roof (1.8×);
  - S5: 20.5 > 16 (OK).
- **Vortex needs:** none: a profile change (`h200g4x8` against `h200g2`,
  `noc-model-design.md` §4.3).
- **Risks:** equivalent to M1 in cost; the vendor went the other way (larger GPCs and
  clusters on Blackwell).
- **Prior work:** GPU NoC topology papers; MCM-GPU.

### M7. Uplink QoS: per-SM or per-tenant weighted arbitration at the GPC port

- **Data path:** unchanged. The arbiter weights requests by class or tenant, and the
  crossbar output arbiters stop favouring lightly loaded GPCs (Observation G).
- **Hardware:** a class bit per request, weighted round-robin.
- **Who pays:** a few gates.
- **Gain:**
  - no bandwidth;
  - latency isolation for a latency-critical job that shares a GPC with a memory-heavy
    one (GPCPlace: 2.42× step time, RTX 4090);
  - fairness for co-tenants;
  - if Observation G is HOL blocking, virtual output queues at the GPC port could
    recover ≈ 1.2-1.4× for concentrated loads (the 64-SM estimate is 3.3 against 4.63).
- **Vortex needs:** two-level arbitration (per SM, then per port) in the group switch.
  Today it is per port, so a load-streaming SM gets up to 4× the share of a DXA SM
  (`noc-model-design.md` §5 risk 3). Also a VOQ option.
- **Risks:** a crowded NoC-QoS field.
- **Prior work:** many NoC QoS papers; GPCPlace (software side).

### M8. Backlog containment: admission at the GPC by return credits

Only if E-1 confirms G1.
- **Data path:**
  - the GPC switch admits a read request onto the crossbar only while the bytes it has
    outstanding stay below a credit budget B_cr;
  - B_cr ≈ C × the unloaded round trip, ≈ 1.28 TB/s × ≈ 0.37 µs ≈ 470 KB per GPC;
  - excess requests wait **inside the GPC** (in the SMs' own queues), not at the shared
    slices.
- **Hardware:** a byte counter and a comparator per GPC port (charge at issue, refund on
  the response), plus a stall signal to the GPC switch inputs. Tens of flops; no new
  wires.
- **Who pays:**
  - nothing in throughput: the GPC is already at C;
  - the saturated GPC's SMs see their queueing moved from the slices to the GPC;
  - a risk of under-filling if B_cr is too small for far-partition and DRAM latency.
    Size B_cr from the far DRAM round trip, or adapt it.
- **Gain:**
  - bystanders recover from ≤ 96 to ≈ 200 GB/s per SM (G1, ≈ 2×);
  - concentrated loads recover toward min(g·C, M): 64 SMs in 4 GPCs ≈ 3.3 → 4.63
    (1.4×), if G2 has the same cause;
  - a multi-tenant packed or GPC-isolated deployment regains the inter-GPC isolation
    that GPCPlace assumes;
  - spread placement: 0.
- **Vortex needs:**
  - a per-group outstanding-bytes counter in the group switch (`cluster.cpp`,
    `noc_uplink.h`);
  - L2 bank input queues and MSHRs shared across groups, so the tree saturation can
    occur. Today's L2 has per-bank round-robin over inputs and 256 MSHRs per bank;
    whether a backlog blocks other inputs must be checked in SimX first;
  - per-SM calibrated ingest.
- **Risks:**
  - G1 may be a makespan artefact; E-1(a) decides;
  - NVIDIA may already throttle, so that G1 has another cause.
- **Prior work:**
  - hot-spot / tree saturation (Pfister & Norton 1985);
  - NoC source throttling and congestion control (e.g. Nychis et al. SIGCOMM'12);
  - GPU NoC request throttling (several MICRO/HPCA papers; none found on the
    GPC-uplink-backlog case);
  - GPCPlace (software, measures queueing only inside shared GPCs).

### 3.8 Comparison

| | S1 packed 16 | S2 GPC-granular co-run | S3 isolated tenants, solo phase | S4 spread | S5 HBM4 | Area / cost | Novelty risk | Vortex work |
|---|---|---|---|---|---|---|---|---|
| M1 wider ×2 | 2.0× | 1.8× | 1.8× | 0 | → roof | high (≈ α² crossbar) | high (B200) | knob |
| M2 lending | 2.0-2.4× | 1.8× | 1.8× | 0 | 0 | linear side links | low-medium | 1 wk SimX |
| M3 partition alloc | R1: 0, R2: ≤ 2× | removes fabric binding for one-partition jobs | same | 0 | removes the 5.2 bound | software | high (Fan) | steps 2-3 (≈ 3-4 wk SimX) |
| M4 coalescing | 0 (disjoint) | 0 | 0 | 0 | 0 | medium | high (LRC) | 1 wk |
| M5 push | 0 for C | 0 | 0 | 0 | 0 | high | medium | 2 wk SimX |
| M6 smaller GPCs | 2.0× | 1.8× | 1.8× | 0 | → roof | high | medium | profile |
| M7 QoS / VOQ | 0 (≤ 1.4× if Obs. G is HOL) | 0 | 0 | 0 | 0 | low | high (QoS) / medium (Obs. G) | 3-5 d |
| M8 backlog containment | 0 for the packed job; bystanders ≈ 2× (if G1) | ≤ 1.4× (if G2) | restores isolation between tenants | 0 | 0 | very low | medium (classic idea, new place) | 2-3 d SimX + L2 queue check |

---

## 4. Verdict

**Not a standalone proposal for H200-class parts.**
- Σ C / M = 2.2, so SM-granular spreading, which is published (GPCPlace, Fan) and which
  we measured (3.08-4.56 TB/s at 16-32 SMs), leaves the cap at zero cost.
- The regimes where it costs (packing by MPS, GPC-granular confinement, GPC-isolated
  tenants) are either software-fixable or depend on a deployment choice whose prevalence
  we cannot show.
- The future-parts argument is real (HBM4 at ≈ 80 GB/s per SM ≥ C / S ≈ 78), but the
  vendor appears to scale C already (B200 knee at 16 SMs).

**Use it as a supporting piece** of an SM-level proposal (memory lane / co-residency
isolation, or bypassing shared memory), as:
- (a) the model of §2 (the critical ratio M / (G·C) and when GPC confinement costs) and
  the placement rule (≤ 5-6 memory SMs per GPC, one GPC per partition for 2-GPC jobs);
- (b) M2 lending as the robustness option for GPC-granular deployments;
- (c) M3 as the fabric fix future parts need.

**What would reopen it as a standalone direction:**
1. E-1 shows Observation G. G1: bystander SMs in other GPCs lose about half their rate
   when one GPC is over-subscribed. G2: saturated GPCs together get ≤ 0.8-1.1 TB/s each
   while the chip is below its roof.
   - Then GPC isolation fails under saturation, a fact against the published premise of
     dedicated-GPC placement.
   - A near-free containment mechanism (M8) has a measurable target: a short
     characterization-plus-mechanism paper.
   - This is the most promising angle in this study, and it rests on one unplanned data
     point.
2. E-3/E-7 show H-c: a disjoint L2-hit uplink ≈ 2.8 against 1.28 from DRAM. Then the
   DRAM cap is a per-source credit limit, and credit sharing (M2b) is a near-free 2×.

**Smallest simulator experiment** (SimX, existing `src_noc` prototype):
- Shape: 4 groups × 8 cores, cap = 3 cores' worth of Vortex's measured per-core DXA rate
  (state the 104 against 210 GB/s calibration gap).
- `bwstream_dxa -M` packed (one group) against spread, k = 2-16. Check that the
  packed/spread ratio follows min(k·r, g·C).
- Add a shared credit pool between groups 0 and 1 and a side-link delay (≈ 50-100 lines),
  with `gemm_block`'s compute role on group 1.
- Report the packed job's rate (expected 3 → 6 cores' worth), the lent-path latency, and
  the compute role's slowdown.
- Prerequisite: root-cause the per-SM rate drop with core count that exists without any
  network (`noc-model-design.md` §5 risk 6).

**Smallest real-GPU experiment:** E-1 plus E-2, about 1 hour in a holder job. E-1(a), the
bystander arm, is 4 runs of 10 s each and is the single most decisive one.
- Decision rule: bystanders unaffected, E-1(b) total = min(g·1.28, 4.63), and E-2 ≈ 11-12
  TB/s together mean "covered by placement". Anything else reopens it.
- Add E-3 (Fan reproduction, ≈ 30 min) to settle the conflict before any number goes
  into SimX.

**Strongest reviewer objections:**
1. "Spread the job; Σ uplinks exceed memory 2.2×; GPCPlace and Fan already do it."
   Fatal for a standalone H200 paper.
2. "Blackwell already widened the GPC port (Fan: knee at 16 SMs)." The problem is being
   provisioned away.
3. "Who runs a memory-bound job confined to < 4 GPCs?" We have MPS packing (a fixable
   default) and no measured production case of GPC-granular confinement.
4. "Lending is a wider uplink with an extra hop." The answer is short wires and no
   crossbar ports. That needs a floorplan argument we cannot measure, only estimate.
5. "Your cap is a measurement artefact." Answered in part: TMA and plain loads agree, two
   GPCs and two dies agree. But Fan's L2-hit 2.8 is unresolved, and our aggregates are
   makespan-based.
6. "The simulator encodes the limit you study." The SimX byte gate is set to the
   measured C; Vortex has no partitions, no remote copies, and half the per-SM ingest.
   Only relative and second-order results (latency, fairness, lender cost) are
   evidence.

---

## 5. Unverified, open

- The partition map (F10) is my reading of another agent's raw file. The L2-structure
  note supersedes it.
- Observation G comes from per-unit averages in a run built for something else.
- The H200 L2 aggregate bandwidth and crossbar clock are not measured; nor is the B200
  GPC size in our notes.
- Wire, area and energy figures in §1.4 and M2 are order-of-magnitude, with the
  assumptions stated.
- The HBM4 part (8 stacks, ~200 SMs) is hypothetical. Vendor figures for upcoming parts
  were not checked.

---

## 6. Diagram specifications

**D1. Hierarchy with numbers (block diagram, left to right).**
- Path: SM (110 B/clk, 205-239 GB/s) → TPC (2×, no limit) → GPC switch (16-18 SMs;
  return port ≈ 650 B/clk = 1.28 TB/s DRAM, 1.27-1.55 L2) → partitioned crossbar → two
  partitions {GPC0, 1, 5, 7} and {GPC2, 3, 4, 6} (66 SMs each) joined by the LTC fabric
  (≈ 1.3 TB/s per direction, reading b; +≈ 173 cycles) → 6 FBPs × 8 slices per
  partition → 3 HBM3e stacks per partition (2.4 TB/s).
- Mark the response direction thick and the request direction thin.
- Annotate the candidate decompositions of 650 B/clk in a callout.

**D2. "When it binds" (line chart).**
- x = memory SMs k (0-64); y = TB/s (0-5).
- Measured series: spread W and spread L16; pair-packed W; MPS L16.
- Model series: g·C for g = 1-4 as a staircase.
- Horizontal lines at M = 4.63 and Σ C = 10.2 (off scale, as an arrow).
- Shade the region of Observation G (48, 64 pair-packed).

**D3. Critical ratio across parts (bar chart).**
- Σ C / M for H100, H200, B200 (H200-class C, hatched as hypothetical), and HBM4 16 TB/s.
- Line at 1.0 labelled "no placement suffices below".

**D4. Lending data path (M2a).**
- Two adjacent GPCs A and B, each with a byte gate and an uplink to the crossbar.
- A short A↔B side link.
- A request from A goes up A's uplink; its response returns via B's uplink and the side
  link.
- Credit exchange arrow between the gates. Label the lent-path extra latency and the
  energy per bit.

**D5. E-1 experiment layout.**
- An 8 × 18 grid of SMs coloured by GPC and partition.
- For g = 1, 2, 3, 4, 6, 8: highlight 12 SMs per chosen GPC, balanced across partitions.
- Beside it the two predicted outcomes as small plots: per-GPC rate flat at 1.28 against
  falling with g.
- An inset for arm (a): one GPC filled red (16 SMs, over-subscribed) and 2 bystander SMs
  outlined in each other GPC. Two bars: bystander GB/s per SM alone (≈ 200) and beside
  the red GPC (G1: ≤ 96).
