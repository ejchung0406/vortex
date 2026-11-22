# Verification of `noc-model-design.md` (independent verifier)

Written 2026-11-21. Sections 1–8 check the design as of 16:36 (rev. 2, §7 filled),
the patch `noc_model.patch` as of 16:12, repo HEAD 540b3dad2, and
`h200-l2-structure.md` (16:42) with its raw logs `results_gpu/h200_l2s_*` (the two
INVALID files were not used). The design's rev. 3 (17:22) answers this file and
absorbs the L2-structure note; §9 checks what it changed. The blocking list below
is kept as written against rev. 2, with the rev. 3 status on each item.

## Verdict

**Sound with required changes.** The structure (GPC switch, two partitions, slices
bound to channels, one inter-partition link) is realisable in Vortex, its zero-load
latency budget adds up and now matches our own H200 within 2%, and with the knobs
off the prototype is the old code path, cycle for cycle. It is not ready to build as
written: the four items below would make a planned study measure the wrong thing or
break a fence. Several numbers tagged "meas." or "lit." are inferred, from another
GPU, or mis-cited (§1).

## Blocking

B1. **`h200g2`'s inter-partition link default hides the GPC limit.** (rev. 3: accepted, B_x = B_g = 648; closed in the text)
- W = 4, B_x = 212 B/clk is scaled from memory ("0.54 × a partition's local rate").
- A packed GPC sends half its lines far, so the link caps it at 2 × 212 = 424 B/clk.
  My model gives 740–840 GB/s for 5–16 packed SMs; the 646 B/clk gate never binds.
- This contradicts the stated reason for two stacks ("so that the GPC limit binds").
- It contradicts the part, now measured: remote-only traffic is capped at
  1,281–1,284 GB/s per direction, the same value as the per-GPC cap, and one GPC
  reading only remote pages still gets 1,283–1,285 (`h200_l2s_local.txt:118, 136,
  154, 172, 262`). So on the part B_x = B_g and a single GPC is never link-bound.
- Required: B_x = B_g (646 B/clk, W = U) for `h200g2`. §8.3 shows what then binds,
  and that no B_x keeps both of the part's ratios in this shape.

B2. **Fences are not ordered through address-hashed lanes.** (rev. 3: accepted, not implemented; still blocking for the prototype)
- The L2 serves a flush token when it reaches the head of the input it arrived on
  (`cache.cpp:2540-2549`, `:3434-3449`), which today is the socket port's own FIFO.
- With the switch, a port's earlier requests are spread over U lanes by address
  (`cluster.cpp` `uplink_sel`), each with its own delay buffer. The token takes one
  lane and can be served while earlier stores of the same port are still in flight
  on other lanes.
- The design's §1.2 "one address always takes one lane, so per-address order is
  kept" covers same-line order only. Fence and flush order is per port.
- Required: replicate the token to every lane and count, or drain the switch before
  forwarding a token; and export "network empty" to the fence/busy logic as
  `l2_link_empty` does today (`VX_cluster.sv:305-326`).
- The `fence` and `flush_coherence` tests with the switch on: §5.

B3. **The design has no stated mechanism or target for the bystander effect, and
the place of the limiter decides it.** (rev. 3: mechanism and target added in §1.5;
open until a SimX run shows it)
- Measured on the part (`h200_l2s_e1.txt:4-58`, one run each): one saturated GPC
  (16 SMs) cuts unsaturated SMs in every other GPC from 192 GB/s to 88 (same half)
  or 106 (other half) with TMA, and to 93 / 132 with loads. The saturated GPC keeps
  its 1,278. With 6 SMs in that GPC (not saturated) the others keep 177–182.
- So the excess requests of a saturated GPC are not held at its own port: they
  occupy something every other SM also uses, in both halves.
- My model (§4): a gate that holds requests at the GPC leaves bystanders at
  −3%. A gate on the return side, with one
  first-in-first-out queue per slice anywhere on the return path, gives 184 → 84
  (same half) / 86 (other half), the saturated GPC at 80 per SM and the total below
  the bystanders alone, as on the part.
- The prototype charges responses only, and an L2 bank has one response queue and
  stalls when it is full (`cache.cpp:1975-1976`, `:2111-2112`). That is the right
  kind of coupling, but nobody chose it and nothing calibrates it.
- Required: make the bystander numbers a calibration target, say in §1.3 which
  queue holds a saturated GPC's backlog, and test it at small size.

B4. **The prototype does not reproduce the saturation shape: a GPC plateaus below
its configured cap.** (rev. 3: three gate bugs reported fixed; the re-measurement in
its §7 is not there yet, and I have not reviewed the new gate)
- On the part the bend is sharp: −2.4% at 6 SMs (TMA; loads −5%), flat within ±0.2%
  from 8 to 18 SMs, for DRAM and for L2-resident disjoint data.
- In the author's runs (design §7.3, §7.4): one GPC of 8 delivers 477 (DXA) and 513
  (loads) GB/s against a 640 GB/s gate; one GPC of 16 delivers 872 against 1,280
  while the same SMs spread give 1,087. The design's §7.6 item 2 says the same.
- These are whole-kernel averages, launch and drain included.
- The runs come from `src_noc_run1`, whose gate tests `empty()` before sleeping;
  the patch tests `size() == 0` with the comment that packets still on their way in
  must count. With `empty()` a packet in flight to a sleeping gate waits for the
  next wake-up. The published plateau numbers may therefore be from a gate that
  loses wake-ups; they must be rerun from the patch before any cause is assigned.
- Other candidates: credit above one line is discarded each clock; head-of-line
  blocking of response lanes on a single DXA port (the author's inference); bank
  stalls (B3).
- My runs from the patched code, small size (§5): 97% of the cap at 2 SMs of a group
  of 4, 90% at 4 SMs, and 83% of the sum when both groups are saturated. Closer, but
  not flat, and two saturated groups cost each other 8% where the part loses 1%.
- Required: rerun from the patched code, measure in a steady-state window, and make
  plateau = configured rate (within the part's ±0.2%, or a stated tolerance) a pass
  criterion.

## 1. Numbers traced to their sources

### 1.1 Measured values, recomputed from the raw logs

`NET` = `results_gpu/h200_tpc_network.txt`, `SCAN` = `h200_tpc_gpcscan.txt`,
`CHIP` = `h200_tpc_chip.txt`, `E1` = `h200_borrow_e1.txt`, `LAT` =
`h200_borrow_latency.txt`. The measurement code divides by 1e9 (`gpu/tpc_lab.cu:842-849`,
`gpu/borrow_tma.cu:330-335`): no GB/GiB slip in the logs.

| Quantity | Design | Recomputed | Source | Verdict |
|---|---|---|---|---|
| per-SM rate, TMA into shared memory, DRAM-sourced, one thread | 205 | 203.9–206.8 (12 SMs, all GPCs); 208.0 on another die | NET:3-91; E1:17 | confirmed |
| top of "205–219" | 219 | 219.1 is L2-resident with 14 remote helper SMs; best DRAM-sourced 213–215 | `h200_borrow_e2_isolated.txt`:632, 118-148 | conditions mixed |
| "~110 B/clk" | 110 | 205 → 103.5; 210 → 106.1; 219 → 110.6 | arithmetic | 110 is the top only |
| per-SM, plain 16 B loads | not in design | **239.1–239.6** (2 × 1,024 threads); 218–219 with 32 warps in the new runs | NET:7-95; `h200_l2s_part2.txt` | "per-SM ingest 205–219" is the TMA path. TMA and loads together cap at 215–230 (§8.1): one shared per-SM limit of 109–116 B/clk, which 4 Vortex ports of 64 B/clk do not model |
| GPC cap, DRAM | 1,277–1,285 | 1,274.1–1,285.5 (TMA, staged, loads; 8–18 SMs) | NET:259-349, 819-849; SCAN:14-34, 50-66 | confirmed |
| GPC cap per clock | 646 ≈ 10 lines | 1,280 / 1.98 = 646.5 B/clk = 10.1 lines | arithmetic | confirmed; read direction only, "each way" is inferred |
| GPC cap, L2-resident | 1.29–1.62 TB/s | medians 1,282–1,548; best single rep 1,670 | SCAN:16-36, 52-68 | "1.62" is in no log; reps vary ±7–14% |
| knee | 6.2 / 6.1 SMs | 1,283 / 205.5 = 6.24 | arithmetic | **inferred**: asymptote crossing; nothing measured between 6 and 8 SMs |
| packed vs spread, 16 SMs | 2.4× | 1,283 vs 3,084–3,090 = 2.40–2.41× | NET:339, 359, 399 | confirmed |
| packed vs spread, 8 SMs | 1.26× | 1.262–1.268 (TMA); loads 1.48× | NET:259-329 | confirmed for TMA only |
| spread 24 SMs = 87% | 87% | 4,011–4,050 / 4,632 = 86.6–87.4% | CHIP:79, 83, 203, 207 | measured beside an FMA job on the other 108 SMs |
| chip roof | 4.63 TB/s | 4,626–4,632 | NET:719, 779, 799 | confirmed (window average) |
| 96.5% of nominal | 96.5% | 4,631.8 / 4,800 | arithmetic | confirmed; 4.8 is the spec value |
| "22.6 SMs fill memory" | 22.6 | 4,632 / 205 | arithmetic | **inferred**; measured: 21 spread 81%, 24 ≈ 87%, 32 spread 98.4% |
| SMs per GPC | 16–18, 8 GPCs, 132 | GPC1–5: 16; GPC6, 7: 18; GPC0: 8 + 8 | `h200_tpc_gpcmap.txt` | 16-SM clusters measured; 18-SM GPCs and GPC0's lone TPCs inferred |
| warm 16 KB transfer | 528–571 | 571 in 3 runs | LAT:6, 12, 18 | 571 confirmed; **528 is in no log** (16,384 B / 61.3 GB/s × 1.98 = 529: a repeat period from bandwidth) |
| 32 KB warm; fixed 421; slope 150 | 721 | 721 | LAT:24 | confirmed; two-point fit, 32 KB on one SM only |
| DRAM 16 KB repeat period | 1,078 | **not found**; nearest derivation 1,089 (E1:9) | — | not found |
| cold transfer | 1,867 "with a TLB miss" | median 1,867 (1,721–3,327) | LAT:5, 11, 17 | number confirmed; **the TLB-miss cause is inferred**, no warm-TLB control exists |
| "~370 MB apart" | 370 MB | stride 256.4 MB; successive cold trials 512.8 MB | `borrow_tma.cu:144, 161` | off by 1.4×; conclusion (new 2 MB page) holds |
| barrier ping-pong | 369–519 | medians 369 and 519 | LAT:9, 15, 21, 27 | confirmed; "≤ 90–130 per hop" is inferred |
| SM clock | 1.98 GHz | nvidia-smi max 1,980 MHz; clock64 / globaltimer median 1.980 GHz over 284 runs | `h200_tpc_env.txt:7` | confirmed as the running clock; clocks were not locked |
| L2 size | 60 MB; slice 625 KB | 62,914,560 B = 60 MiB; / 96 = 640 KiB | E1:4 | **MB/MiB slip**: 625 KB should be 640 KiB (the `h200g2` slice is then exact); L2 per SM is 427 / 465 KiB, not 417 / 455 |

The measured curve the model must reproduce (DRAM-sourced TMA, GPC6, GB/s):

| SMs in one GPC | 1 | 2 | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|---|---|
| total | 205.5 | 410.4 | 819.7 | 1,203.4 | 1,280.1 | 1,283.8 | 1,284.6 | 1,283.9 | 1,283.2 | 1,285.1 |
| vs min(205.5 n, 1,283) | 0 | −0.1% | −0.3% | −2.4% | −0.2% | +0.1% | +0.1% | +0.1% | 0 | +0.2% |

- A hard-min fit crosses at 6.35 SMs; a soft fit has exponent p = 20 (about 3.4%
  rounding at the crossing). SMs in a capped GPC share within about 1%.
- Spread over GPCs the roof is approached softly: −5.8% at 16 SMs, −13% at 21–24,
  98.4% only at 32 SMs (NET single_spread).
- Run-to-run: DRAM reps ±0.1–0.5%; across dies and builds 0.1–3.5%. Three dies were
  used.

### 1.2 Literature values

Read at the primary source unless marked: Fan et al. (arXiv 2609.24270), DGNA,
GemAtlas, Accel-Sim 2.0, Jin (MICRO'24), the A100 and H100 whitepapers, Accel-Sim
`addrdec.cc` and the `SM90_H200` config, US11429534 (description, not the claims).
JEDEC only through press copies.

| Claim | Design | Source says | Verdict |
|---|---|---|---|
| near / far L2 hit | 295 / 470 | DGNA, H100 SXM. Fan, H200: ~309 / ~466. **Ours, H200 at 1.98 GHz (`h200_l2s_lat.txt`, recomputed from the samples): near p50 293–297 (local-home median 290–294), far p50 466–467** | within 2% of the design; an H200 measurement now exists |
| near / far DRAM | 554 / 728 | DGNA H100. Fan H200: ~490 / ~655. **Ours: medians 545–548 near-home, 730–736 far-home**, plus 7–10% of cold lines near 960–975 | within 1.5% of the design. The pooled median 702–705 and the "650 / 1,125" fit in `h200_l2s_lat_analysis.txt` are a mixture and a mis-fit, not classes. Fan's values are not reproduced |
| far − near | 174–175 | ours: 173–177 (L2), 185–188 (DRAM) | L_x = 86 stands for hits; the DRAM gap is about 12 cycles larger than the hit gap, which the design's "nothing new" row does not allow for |
| D | 177 | ours: 546 − 290 − 82 = 174 | confirmed to 2%, given the "82" |
| unit | cycles at 1.98 GHz | ours: every latency run at 1,974–1,980 MHz in-kernel | settled for our data |
| 96 slices = 2 × 6 × 8 | lit., pend. | ncu on our H200: `l2s_count` 96, `num_l2s_per_fbp` 8, `fbp_count` 12 (`h200_l2s_ncu.txt`) | measured (tool attribute) |
| G_p = 4, 2 partitions | lit., "pend. H200" | ours: sides {0, 1, 5, 7} and {2, 3, 4, 6}, one probed SM per GPC | measured |
| slice granule 256 B | GemAtlas; "Accel-Sim `dramid@8`" | GemAtlas Eq. 1 (H20). In Accel-Sim `dramid@8` places the **channel**; its slices alternate every 128 B. Ours (`h200_l2s_bw_slice.txt`): bits 7 and 9 keep the one-slice rate, 8, 10, 11 change it, but the 434 GB/s rows equal the probe kernel's own cap and the bits 8–9 set contradicts the reading | **low confidence** (the note says medium); Accel-Sim mis-cited |
| partition interleave | "4 KB hash or 8 KB alternation" | ours: PA12 ⊕ PA13 ⊕ PA15 ⊕ PA17 ⊕ PA19 ⊕ a per-2 MB-page constant, zero error on 65,536 lines in 4 pages; identical to GemAtlas H20 | one XOR hash, not two options; a single-bit select would be wrong. Bits above 20 unknown (Fan: 16 bits) |
| remote-read copy | not modelled | confirmed by GemAtlas, DGNA, Accel-Sim 2.0, Fan (effective L2 65.8%), **and by us**: side-A SMs hit lines homed on B at 297 after another side-A SM read them, not at 467. Accel-Sim 2.0 and Fan: local tag check first, then forward | the design's router sends far requests straight to the link |
| channel per slice | US11429534 "(embodiment)" | NVIDIA's patent; embodiments have 3 or 5 slices per 2 channels, some shared. Never 1:1 | **mis-cited**. 1:1 on the H200 rests on arithmetic only (96 slices, 96 channels); Accel-Sim models 2:1 |
| HBM3E 16 × 64 bit, 800 GB/s per stack | — | 16 × 64 b × 6.25 Gb/s = 800 GB/s | arithmetic confirmed; six stacks and 6.25 Gb/s inferred |
| remote 1.3 vs local 2.4 TB/s | "via coordinator, source pending" | Fan Fig. 8. Ours: one side 2,314 local, 1,284 remote; chip 4,629 / 2,562 | measured; ratio 0.55 |
| Jin p8 (far share lower) | §1.3 fairness note | A100, a latency effect by Little's law | misapplied |
| 144 SMs, 60 MB | H100 WP p18 | confirmed | — |
| A100 64 B/clk per slice | WP arithmetic | 5,120 B/clk / 80 slices | A100, not Hopper |
| LRC merges up to 4 reads per GPC | not modelled | Accel-Sim 2.0 (H100): only within one GPC | confirmed |

### 1.3 Labelled "measured" but inferred

- The knee at 6.1–6.2 SMs, and "22.6 SMs fill the memory".
- 528 (warm), 1,078 (DRAM repeat period), 1.62 TB/s (L2-resident cap).
- The TLB-miss explanation of 1,867.
- "Each way" for the GPC cap: only reads were measured.
- That the cap is a per-clock byte limit on the SM clock at all. It is the same for
  DRAM and L2-resident data (disjoint buffers), which rules out a limit on lines in
  flight; whether it scales with the SM clock was not measured (single-GPC runs were
  all at 1.98 GHz).
- "Fabric" for the 1.28 TB/s per-direction cap: the data show a cap on remote-homed
  traffic per side, equal to the per-GPC cap, not where it sits.
- In `h200-l2-structure.md`: "pages 2j / 2j+1 opposite in 100% of pairs" is
  tautological (bit 12 is in the hash that built the lists; the raw cold classes
  give 92.7%); the 3.4% disagreement of the 7 GiB check is on non-tail blocks, not
  the tail; the page lists were made on an 8 GiB allocation and applied to a 24 GiB
  one in another process; every bandwidth configuration is a single run.

### 1.4 Unit and arithmetic slips

- **Memory per SM "1.43×".** 50 GB/s is nominal, 35 GB/s is achievable. Like for
  like it is (2 / 32) / (6 / 132) = **1.375×**.
- 625 KB vs 640 KiB, and 417 / 455 KB vs 427 / 465 KiB (§1.1).
- `h200s` is built on "50 MB / 6" (`profiles/h200s.json` sources); the design uses
  60 MB. One of the two is stale.
- **One port carries one 64 B line per clock = 126.7 GB/s.** The TMA rate is
  1.62 lines/clk (205 GB/s), loads 1.89. The DXA uses port 0 only in RTL
  (`VX_socket.sv:803` `.GMEM_OUT_PORTS (1)`); SimX has `VX_CFG_DXA_GMEM_PORTS`
  (`VX_config.toml:479-482`). So "per-SM ingest 210 GB/s once the DXA calibration
  lands" cannot be reached in RTL by a rate change. Until then a GPC of DXA SMs
  bends at ≥ 10 SMs, not 6.
- 210 GB/s at a 554–728-cycle DRAM round trip is 900–1,200 lines in flight per SM
  (my model: a 256-line window gives 58 GB/s). Check `DXA_MAX_INFLIGHT` (256) and
  the dcache MSHR (512) against that.
- MSHR "128 per slice by Little's law" uses the zero-load 260 cycles. The author's
  own runs show memory latency 590–700 cycles at about 80% utilisation; that needs
  about 250 lines. `h200s` has 256.

### 1.5 The conflict with Fan et al.: resolved by measurement

`h200_l2s_gpcsweep.txt`, GPC 6, one run each, 1.98 GHz, GB/s:

| SMs | 1 | 2 | 4 | 6 | 8 | 10 | 12 | 14 | 16 | 18 |
|---|---|---|---|---|---|---|---|---|---|---|
| DRAM, disjoint, loads | 219 | 429 | 847 | 1,221 | 1,279 | 1,283 | 1,285 | 1,285 | 1,284 | 1,285 |
| DRAM, disjoint, TMA | 196 | 391 | 782 | 1,170 | 1,281 | 1,284 | 1,284 | 1,285 | 1,285 | 1,284 |
| L2, disjoint 256 KB per SM | 245 | 483 | 976 | 1,245 | 1,273 | 1,289 | 1,273 | 1,289 | 1,273 | 1,286 |
| L2, one shared 256 KB | 244 | 488 | 980 | 1,302 | 1,355 | 1,585 | 1,947 | 2,247 | 2,514 | 2,753 |
| L2, one shared 4 MB | 245 | 487 | 973 | 1,278 | 1,297 | 1,417 | 1,610 | 1,831 | 2,055 | 2,276 |

- Fan's 2.8 TB/s is reproduced only when all SMs read the same lines. With disjoint
  data the GPC is flat at 1.27–1.29 from DRAM and from the L2.
- Our earlier "L2-resident 1.29–1.55" was a shared 24 MB window with staggered
  starts: partial merging.
- What the design does under this resolution:
  - Keeps one gate at 646 B/clk, counting **unique lines**. Correct as designed for
    disjoint data, hits included.
  - Adds, or lists as an omission with its size, a per-GPC merge of same-line reads
    in front of the gate. Without it a GPC reading shared lines is under-delivered
    by up to 2.1× (2,753 / 1,285), and the whole chip by 2.6× (25.0 against 9.6 TB/s,
    `h200_l2s_e2.txt`).
  - L2 residency in these runs is inferred from footprint and rate, not from
    counters.
- Not settled: whether the limit is on requests or on returns. Shared reads exceed
  it, which the note reads as "not the return wires". That holds only if the merge
  is outside the GPC; a merge inside the GPC returns each line once as well.

## 2. Claims about today's Vortex, at the instantiation

Static reading of RTL and SimX at 540b3dad2; `h200s` resolved with
`ci/gen_config.py`. `h200s`: 22 cores, 4 ports, L2 16 banks × 128 sets × 33 ways ×
128 B (sector 64 B), `L2_LATENCY` 8, MSHR 256, write-back, link 94, 90 L2 inputs.

| # | Claim | RTL | SimX | Verdict |
|---|---|---|---|---|
| 1 | 4 ports; port 0 a priority arbiter (icache > dcache b0 > tcache > rtcache > DXA); DXA on port 0 only | `VX_socket.sv:931-1013`, arbiter `:973-988`, `:803` | `socket.cpp:253-311` | confirmed. Strict priority: the DXA can be starved. SimX-only `DXA_GMEM_PORTS` |
| 2 | private link, `L2_LINK_LATENCY` each way, 1 line/clk | `VX_cluster.sv:306-326` (`VX_mem_bus_delay`) | `cluster.cpp:128-147, 192-199` | confirmed. Elastic: FIFO of 130, a stall does not freeze in-flight items; RTL and SimX match |
| 3 | one `VX_cache_wrap`, inputs = sockets × 4 + PTW + CP | `VX_cluster.sv:225-263`; `VX_gpu_pkg.sv:2345-2380` | `cluster.cpp:85-116` | confirmed; package cite stale by about 27 lines. CP and PTW enter **without** the link |
| 3b | bank select = line bits XOR hash, 128 B granule, power-of-two banks | `VX_cache.sv:349-360` | `cache.cpp:132-159` | confirmed; IPOLY at 16 / 32 banks |
| 4 | L2 input = round-robin crossbar per bank, "tree of 8-input slices `:118-133`" | `VX_cache.sv:520-543`; tree in `VX_stream_arb.sv` | `cache.cpp:36-50, 2671-2830` | confirmed; `:118-133` is the sizing function. Input-queued, head-of-line; 2 cycles at 90 inputs, 1 at ≤ 12 |
| 5 | MSHR per bank, 256 (`h100.json:88`) | `VX_cache.sv:596` | `cluster.cpp:99` | confirmed; value at `h100.json:47`. A full MSHR refuses every request to the bank, hits included |
| 6 | bank i → port i mod ports; omega networks; identity still adds stages | `VX_cache.sv:806-813, 264-284, 875-893` | `cache.cpp:3250-3305, 3702-3705` | RTL confirmed (4 stages each way at 16 → 16). **SimX request side wrong**: at banks = ports the arbiter is in bypass and drops its delay (`types.h:1882-1888`), so SimX has 0 cycles where the RTL has 4. Toml cite is `:386` |
| 7 | L3 passthrough | `Vortex.sv:151-188` | — | confirmed |
| 8 | port → channel by `locate()`, independent of the bank | rtlsim: `VX_mem_bank_adapter` (block interleave) then `processor.cpp:443-459` | `memory.cpp:62-65` | partly stale. rtlsim has two remaps; **opae and xrt do not use `locate()`** (`opae_sim.cpp:318-325`, `xrt_sim.cpp:539-571`) |
| 9 | `locate()` plain / mixed, power-of-two unit and channels | — | `dram_sim.cpp:393-417` | confirmed, and a bijection in both modes. The SimX crossbar also needs ≤ 64 power-of-two outputs in every mode (`types.h:1734-1735`), so 96 channels fail before `locate()` |
| 10 | 304 cycles on every read response | not in RTL; rtlsim uses the same DramSim | `dram_sim.cpp:280` | confirmed. Reads only, after the scheduler, a pure delay line with no throughput limit. `PLATFORM_MEMORY_PEAK_BW` is used by no timing model |
| 11 | CP and PTW as L2 clients, `write_around` | `VX_gpu_pkg.sv:2361-2380`; `VX_cluster.sv:245` | `constants.h:120-143`; `cluster.cpp:109` | confirmed (cites stale). CP: 64 B words, up to 64 reads in flight, a flush token before a launch |
| 12 | private L2 per cluster; write-back only with one cluster | `VX_gpu_pkg.sv:2421-2423`; `Vortex.sv:72-73` | `constants.h:53` | confirmed. Evictions use the bank's own memory request path, the same queue as its fills |
| 13 | `L2_FWD_HOLD` options | RTL always holds (`VX_cache_bank.sv:411, 431-433`) | `cache.cpp:1593-1597`; default 0 | SimX-only knob; **at `h200s` SimX replays where the RTL holds** |
| 14 | "every response sink accepts unconditionally" (§1.3) | DXA: `rsp_ready = sw_ready && bus_rsp_present` (`VX_dxa_gmem_req.sv:194`); caches gated by `pipe_stall` | DXA `capture_line` returns without popping (`dxa_core.cpp:599-602`) | **wrong**. Port 0's response switch couples icache, dcache bank 0 and DXA |
| 15 | `VX_mem_xbar` exists, unused, reusable | `hw/rtl/mem/VX_mem_xbar.sv`, no instance | — | unused confirmed; **not usable as written**: it zeroes `tag.value` (`:117, 121, 211`) and truncates the widened tag. `VX_mem_bus_arb` does the tag insert correctly (`:90-94, 121-150`) |
| 16 | KMU: one CTA per cycle, round-robin (`VX_kmu.sv:179`) | placement is in `VX_kmu_bus_arb` (`VX_cluster.sv:149`, `VX_socket.sv:78`) | `kmu.cpp:109-157` | SimX confirmed; RTL cite wrong (`:179` is a DCR decode) |
| 17 | "~85 = input tree + bank pipeline + response path" (§1.4) | — | see below | **wrong decomposition** |
| 18 | "+ ~82 Ramulator array timing" | — | — | also contains harness crossbar, boundary delay, response tap, fill and replay |
| 19 | atomics and LR/SC resolve in one place | `VX_cache_amo` in the LLC bank | same | confirmed: needs only address → one bank |
| 20 | pseudo-LRU "calibrated" | tree PLRU (`VX_cache_repl.sv:110-163`) | age-counter LRU (`cache.cpp:312-360`) | SimX and RTL differ today |

The "85". Socket port to socket port in SimX at `h200s`: socket arbiter 1 +
register slice 1 + link 94 + input tree 2 + admission ~1 + bank 8 + response
`omega_stages(16, 90) + 1` = 8 + link 94 + slice 1 + arbiter 1 ≈ 211. So only about
19–20 cycles are inside the L2; about 62 of the 273 are the core, LSU and dcache
miss path. Two of the L2 terms depend on the bank and input counts, so the constant
moves when the L2 has 32 banks and about 40 inputs instead of 16 and 90. The 273
itself was calibrated on `h100` with one core (`ci/profile_check.py:141`), not on
`h200s`.

## 3. The design itself

### (a) Scaling arithmetic of `h200g2`

- Confirmed: 1.6 TB/s nominal, 1.544 at 96.5%; 7.4 SMs fill it (23%); cap / memory
  0.83; 1,280 / 210 = 6.1 SMs = 38% of 16; 646 B/clk; B_x 0.54 × 772 = 417 GB/s =
  211 B/clk; `h200g4x8` 323 B/clk = 3.05 SMs.
- Slip: 1.43× should be 1.375× (§1.4).
- L2 geometry: 20,971,520 B = 32 banks × 256 sets × 20 ways × 128 B. Accepted by RTL
  (`VX_cache.sv:75-86`: power-of-two sets and banks, any ways) and SimX
  (`cache.cpp:93-129`). PLRU takes 20 ways (32-leaf tree folded). `L2_LATENCY`
  becomes 9; hash IPOLY-32.
- Channels: 32 is a power of two, so `locate()` and the SimX crossbar accept it.
  16 × 64 bit × 6.25 Gb/s = 800 GB/s per stack; 50 GB/s = 25.25 B/clk per channel.
- Bank groups stay inside a channel, so binding a slice to a channel does not touch
  them. The channel-local address must drop exactly the bits the slice and
  partition selects consume; see (e).
- "About 8 SMs fill the memory" is the hard-min figure. On the part's measured
  roll-off, 1.0× the fill count gives about 87% and 98% needs about 1.45×, i.e.
  about 11 SMs here.
- The cap sits at 0.83 of the memory roof, against 0.28 on the part. With slices
  bound to channels each channel runs at 83% when a GPC is capped, and no channel
  can be borrowed. My model loses 4–6% of the cap from slice imbalance alone
  (1,206–1,232 against 1,280), and 11% with a poorer hash. The plateau will not be
  flat within ±0.2% as on the part.

### (b) Latency budget

- My model, with the design's values (L_sm 40, L_up 63, switch 1, gate 1, L_x 86 + 1,
  "85", D + 82 = 259), one request at a time: **near hit 295, far hit 469, near DRAM
  554, far DRAM 728**. The sums are right, requests and responses.
- The prototype does not meet its own formula: design §7.2 measures warm 906 against
  902 with the default split (47 + switch + gate + 45), i.e. two extra cycles each
  way from channel stages. With 40 / 63 the near hit would be 299, and the toml
  comment "the defaults keep the L2 link's round trip" is off by 4 cycles.
- Conditions the design does not state:
  - The "85" moves with the L2's input and bank counts (§2).
  - A far response needs a merge of two sources (local crossbar, link) onto each
    uplink lane. The §1.3 table has the request router only. The merge needs an
    arbiter and either a cycle or a combinational path.
  - If the link's gate is enabled it adds a cycle each way: far − near is
    2 × (L_x + 2).
  - With partitions the response omega (`omega_stages(banks, inputs)`) and the
    input tree are sized by the total, not per partition.
- Under load the queues dominate: my model's mean round trip goes from 643 to
  930–1,180 cycles for 7–16 packed SMs. No target exists for that.
- The warm-transfer target (528–571) is met only if warm lines are near. With no
  remote copy half are far: 571 + 174 ≈ 745 in the design's own terms.

### (c) Flow control and deadlock

Argument for no deadlock (needs the three conditions at the end):
1. Requests move SM → switch → router → {local crossbar, link → other crossbar} →
   slice. Responses move slice → {lane, link → lane} → switch → SM. No request is
   created by a response and no response re-enters a slice, so the two networks are
   separate and each is a forest. The two link directions are different channels.
   The two crossbars do not depend on each other: a request crosses the link once.
2. A slice waits on its response queue, its MSHR, and its memory port. Fills,
   evictions and write-backs use the slice's own channel and never the network.
   A held fill (`FWD_HOLD`) waits on the response queue only.
3. So every wait chain ends at an SM's response port or at the DRAM model, and the
   byte gates refill with time alone.

Conditions:
- **The sinks are not unconditional** (§2 item 14). The DXA's ready depends on its
  shared-memory writer, the caches' on their bank pipeline. Neither waits on the
  request network, so the chain still ends; the design's stated reason is wrong and
  must be replaced by this one.
- The CP and PTW clients must stay outside the gate or have a reserved share: a
  walk blocked behind a capped GPC's backlog stalls the SM that needs it. The design
  attaches the CP to partition 0, so half of its traffic crosses the link and a
  210 B/clk link makes the upload path part of the bottleneck.
- Fences: B2.

Not deadlock but coupling (counterexamples to "isolated"):
- B3: capped GPC → bank response queue full → bank stalled for all. The part shows
  an effect of this kind; its size in SimX is unmeasured.
- Port 0: a DXA that is not ready blocks icache and dcache bank 0 responses behind
  it in the same lane, and with the switch that lane is shared by the whole GPC.
- Requests: a far request at a lane head waiting for the link blocks near requests
  behind it. My model: with B_x = 212 a packed GPC gets 740–840 GB/s, not a steady
  840, and the curve is not monotonic.
- A full MSHR in one slice blocks every lane whose head targets it.

Prototype specifics are in §5.

### (d) Fairness and the saturation shape

- GPC switch requests: round-robin per lane over ports. Per port, not per SM: a
  load SM (4 ports) gets up to 4× a TMA SM (1 port). On the part, SMs in a capped
  GPC share within 1% for one kernel; mixed kernels were not measured.
- GPC switch responses in SimX: **one arbiter shared by all ports**
  (`types.h:2038, 2074-2084`), not "per port, round-robin over lanes". An RTL
  crossbar has one arbiter per output. Parity gap.
- Byte gate: with the credit binding (646 on 16 lanes) lanes get equal shares; when
  it does not bind the prototype skips or doubles lanes (§5).
- The shape: see B4. A token gate with a closed-loop source gives a sharp bend (my
  model: 1,259 at 6 SMs, the part 1,203). The part is slightly softer at the knee
  and exactly flat after; the prototype is much softer and below the cap.

### (e) Correctness

- Address → partition → slice → channel is a bijection for power-of-two slices, if
  the channel-local address drops one bit group per select: for example bit 12 for
  the partition and bits 8–11 for 16 slices. Both selects are XOR folds, so the
  dropped bits are recovered from the select value and the rest. The design says
  "drop the slice-select bits" without saying which, and does not mention the
  partition bit. Both must be in the shared helper, with the round-trip test.
- For 24 or 48 slices per partition a hash followed by a modulo does not give equal
  channels or an inverse. A remap table with a defined inverse is needed. Open, and
  the SimX crossbar's 64-output limit comes first.
- One line in one slice: holds, because the slice granule (256 B) is a multiple of
  the L2 line (128 B) and the partition granule a multiple of that. It fails if a
  profile sets the slice granule below the L2 line size: assert it.
- Atomics and LR/SC: still resolved in the home slice. Unaffected.
- Same-word order: kept per (port, line) by the lane hash. Not kept for fences (B2).
- The remote-copy option breaks "one line in one slice" by design and needs the
  coherence rules; correctly marked as a separate step.

### (f) Knobs

- Step 1, off: literally the old code under `#if` (§5). Cycle-identical.
- Step 1, on with degenerate settings: equivalent, not identical.
  `noc_g1_bw22` (group of 1, 4 lanes): 135,458 cycles against 137,037 (−1.2%),
  because requests are routed by address, not by port.
- Step 2: identity is achievable only if `SLICE_BIND = 0` keeps today's two
  independent functions verbatim. A "unified function with default parameters"
  cannot reproduce `locate()`'s 1 KB hash and the bank hash at once. Also note the
  SimX/RTL omega mismatch at identity (§2 item 6) exists today.
- `h200g4x8` by knobs only: yes, if step 3 implements `GROUPS_PER_PARTITION`
  generally. The author already ran it for step 1 (`noc_g4_*`).
- `VX_CFG_PLATFORM_MEMORY_PEAK_BW` in the knob table has no effect on timing.

### (g) RTL realisability and parity

| Element | RTL module | Concern |
|---|---|---|
| SM and uplink delays | `VX_mem_bus_delay` | none; exists |
| GPC switch | **not `VX_mem_xbar` as written** (drops `tag.value`). `VX_stream_xbar` plus `VX_mem_bus_arb`-style tag insert | 64–72 inputs × 16 outputs with round-robin: needs the tree of 8-input slices the L2 uses, so 2 cycles, not the 1 the budget assumes. Tag grows by 7 bits; SimX tag must stay under 32 bits |
| byte gate | new `VX_noc_rate_gate` | granting up to 10–11 of 16 lanes in one clock with a running credit is a 16-deep subtract-and-compare chain. Use a per-clock grant count (prefix sum of valids against `credit / 64`) |
| partition router | 1 → 2 demux on the hash | the hash of 16 address bits is in the path; register it |
| response merge per lane | new, missing from the design | arbiter per lane |
| link | delay + gate | none |
| slice = bank with its own port | `VX_cache.sv` omega bypass at identity | changes today's RTL latency by 4 cycles each way |
| core mask | `VX_kmu_bus_arb`, not `VX_kmu.sv:179` | the mask must reach every level of the hierarchical arbiter |

- FPGA drivers (opae, xrt) do not use `locate()`, so slice → channel binding there is
  a separate change.
- Parity at small size: the design's case "G = 4, U = 2, B = 96" is exactly where the
  prototype's gate misbehaves (§5). Add cases for: a fence under load, the CP path
  during a capped stream, and one with ample credit.
- Today's SimX/RTL gaps that the new parity cases will hit first: omega at identity,
  `L2_FWD_HOLD`, the LRU model.

### (h) Omissions that can flip a conclusion

| Omission | Study at risk | Why |
|---|---|---|
| remote-read copy and local-first tag check | SM-sharing isolation; shared-memory bypass | warm data is half far (+174 cycles) and effective L2 capacity is wrong (the part: 65.8% effective). Hot-data results change sign with it |
| same-line merge per GPC (LRC) | GPC limit; any workload where SMs read the same lines (shared weights) | the part delivers up to 2.1× the cap per GPC for shared lines; the model caps them |
| LRC coalescer | GPC limit | SMs of one GPC reading the same lines pass up to 4× the unique-line rate |
| an uncalibrated bystander effect (B3) | SM-sharing isolation | the part halves a bystander's rate; the model's coupling is accidental and unmeasured |
| per-port fairness | co-run inside a GPC | load SMs get up to 4× a TMA SM |
| soft roll-off near the memory roof | everything quoted "as a fraction of the roof" | the part delivers 87% at the nominal fill count |
| one GPC per partition | placement inside a partition | accepted; use `h200g4x8` |
| write direction of the gate | any write-heavy kernel | never measured on the part |

## 4. Independent model

`/tmp/echung67/nocv/nocmodel.py` (cycle model, about 300 lines): closed-loop SMs
(rate and window), per-lane round-robin switch, token gates, partition router, link
lanes, per-slice round-robin crossbar, slices with MSHR and a rate-limited channel,
response path with one grant per port per clock. Design parameters of `h200g2`;
per-SM 210 GB/s over 4 ports; DRAM-sourced unless noted.

Zero-load latency: 295 / 469 / 554 / 728 (§3b).

Packed in one GPC, GB/s:

| SMs | part (GPC6) | model, B_x = 646 / W = 16 | diff | model, B_x = 212 / W = 4 |
|---|---|---|---|---|
| 1 | 205.5 | 212.0 | +3.2% | 212.0 |
| 2 | 410.4 | 420.0 | +2.3% | 419.9 |
| 4 | 819.7 | 840.2 | +2.5% | 839.4 |
| 6 | 1,203.4 | 1,259.1 | +4.6% | 740.9 |
| 8 | 1,280.1 | 1,227.4 | −4.1% | 813.3 |
| 10 | 1,283.8 | 1,217.3 | −5.2% | 837.9 |
| 12 | 1,284.6 | 1,232.0 | −4.1% | 786.8 |
| 16 | 1,283.2 | 1,224.1 | −4.6% | 813.1 |

- With the non-binding link the model is within 5% of the part everywhere. The +2–3%
  below the knee is the 210 against 205 input. The −4–5% above it is slice
  imbalance at 83% channel load (§3a), which the part does not have at 28%.
- With B_x = 212 the model is 35–42% under the part from 6 SMs on: B1.
- L2 hits only: 1,260 at 6 SMs, 1,278–1,279 at 8–16. The part's L2-resident medians
  are 1,285–1,548 with a shared window (partial merging) and 1,273–1,289 with
  disjoint data: the model matches the disjoint case within 1%.
- Spread 4 + 4 SMs: 1,542 GB/s = 99.9% of the roof. The part at the same offered
  load delivers about 87%. Explained: my SM is rate-limited and my channel is a hard
  rate; the part's SM has a fixed 192 KB in flight and slows as latency grows.
- Isolation in this configuration: one SM in GPC 1 keeps 210–215 GB/s beside a
  saturated GPC 0. Here the SMs are rate-limited with a large window, so added
  latency does not lower their rate. With window-bound SMs, as on the part, the
  same model halves the bystander: §8.2.

Against the author's SimX runs (`results/noc_*`, whole-run bytes / cycles × 1.98):

| Run | shape | SimX | model | note |
|---|---|---|---|---|
| `noc_g2d_bw_pk8` | 2 × 16, cap 646, loads, 8 packed | 583 | 600 | −3% |
| `noc_g2d_bw_pk16` | 16 packed | 1,079 | 1,200 | −10%; offered 1,200 < cap, the gate should not bind |
| `noc_g2d_bw_sp16` | 8 + 8 | 1,101 | 1,200 | −8% |
| `noc_g2d_dxa_pk16` | DXA, 16 packed | 871 | 1,280 (cap) | **−32%** |
| `noc_g2d_dxa_sp16` | DXA, 8 + 8 | 1,086 | min(16 r, roof) ≈ 1,540 | −29%; the author's risk 6 (rate falls without any network) |
| `noc_g4_bw_pk8` | 4 × 8, cap 323, one GPC | 511 | 600 (offered < 640 cap) | **−15%** |
| `noc_g4_dxa_pk8` | DXA, one GPC | 477 | 640 (cap) | **−25%** |
| `noc_g4_dxa_pk16` | DXA, two GPCs | 948 | 1,280 | −26% |

- Loads agree within 10% where the gate should not bind.
- Wherever the gate binds or nearly binds, SimX is 15–32% below my model and below
  its own configured cap. Not explained by my model. Candidates, in the order I
  would test them: launch and tail cycles in the whole-run average (a single SM
  shows 75–81 GB/s in 32-core runs against 104 in the 22-core run); credit lost each
  clock under bursty arrivals; bank stalls behind blocked responses (B3); the
  one-response-per-port-per-clock limit on DXA port 0.
- `noc_g2d_dxa_pk1` takes 77,042 cycles where `pk4` takes 66,617 for four times the
  bytes. Whole-run cycles are not a usable metric at this size.

## 5. The prototype patch

Review by `.agents/skills/code-review` and `simx-review`. Impact class: R3 (cycle
model of the cluster memory path, new config and DCRs, no RTL). My copy:
`vortex-exp/src_nocv`; builds under `/tmp/echung67/nocv/`, `VORTEX_VM_MEGAPAGE_HEAP=1`.

Provenance:
- `src_nocv` = HEAD export + the patch, file for file (checked by reverse-applying).
- The author's runs come from `src_noc_run1`, whose `noc_uplink.h` differs from the
  patch in the idle test (`empty()` against `size() == 0`). The results were not
  produced by the code in the patch.
- The patch (16:12 version) no longer contains `profiles/h200h.json`, which design §6
  still lists; the file exists only in `src_noc`.

Default-off cycle identity (my builds: reference = my copy with the patch reversed,
equal to `src_noc_ref`; `h200s`):

| Case | reference | patched, knobs off | PERF lines differing |
|---|---|---|---|
| `bwstream -n64 -u4 -b32`, 4 cores | 84,224 instr / 23,526 cycles | 84,224 / 23,526 | 0 of 14 |
| `demo`, 3 cores | 119,808 / 204,573 | 119,808 / 204,573 | 0 of 11 |
| author: `bwstream` 22 cores | 137,037 | 137,037 | 0 (log diff) |
| author: `bwstream_dxa` (HEAD's app) 22 cores | 56,464 | 56,464 | — |
| author: `gemm_block` 1 core | 653,498 | 653,498 | — |

- Verdict: **cycle-identical with the knobs off**, and it has to be: the old binds
  are compiled under `#else`; the only shared changes are a KMU term that is zero
  and an `assert`.
- Not usable: `amo` at default settings aborts with the same issue timeout at
  200,000 cycles in both trees (no switch involved); `gemm_block` at 2 cores and the
  second batch (`amo` with a long timeout, `fence`, `sgemm`) did not finish on the
  shared node.

Switch on, 8 cores in 2 groups of 4, U = 4, 64 B/clk per group, `bwstream -n64`
(524,288 B per SM; one SM alone runs at 48 B/clk after about 8,400 launch cycles,
from the 13,900 / 19,367-cycle pair at `-n32` / `-n64`):

| Active SMs | cycles | steady rate, B/clk | of the cap |
|---|---|---|---|
| core 0 | 19,367 | 48 | not capped |
| core 4 (group 1) | 19,489 | 47 | not capped |
| cores 0–1 | 25,284 | 62 | 97% of 64 |
| cores 0–3 | 44,792 (twice, all counters equal) | 58 | 90% of 64 |
| cores 0–7 (both groups) | 48,081 | 106 | 83% of 128 |

- All runs PASSED. Deterministic: the repeated run is identical in every PERF line.
- With the patched gate the plateau is within 3–10% of the cap, much closer than the
  author's published runs (68–80% of the cap). The steady rates rest on my launch
  estimate (±1,000 cycles is ±3%).
- The plateau sags as SMs are added (97% at 2, 90% at 4): not flat.
- **Two saturated groups slow each other by about 8%** (53 B/clk each against 58
  alone), with the memory at a quarter of its roof. On the part two saturated GPCs
  lose 0.4–1.2% (2,537–2,558 against 2 × 1,284). So SimX couples saturated groups
  more than the part does, while a bystander's loss in SimX is unmeasured.
- Per-core finish times could not be taken: my instrumented copy (a `vx_printf` at
  the end of each CTA) never finished, the unmodified app does; I did not find out
  why. So the bystander run in SimX is still to do.
- `fence -n64` with the switch on (8 cores, groups of 4, 64 B/clk): **not finished
  after more than 3 CPU-minutes and 19 minutes of wall time**, where the stream runs
  above took 15–30 CPU-seconds. The control (`fence`, switch off, 2 cores, reference
  tree) passes in 355,893 cycles. The two differ in core count and in the cap, so
  this is a suspected hang, not a proven one. My `vx_printf` copy of `bwstream`
  also never finished with the switch on. Both need a run with the switch off at
  the same size before B2 is called confirmed by test. `flush_coherence` was not
  reached.
- Not run: U = 2 with and without an ample gate (the defect is shown by the exact
  port of the loop below), the `-l` latency test with groups on (the author has it:
  906 against 902), a non-power-of-two group size.


Findings:

| Sev | Finding | Where |
|---|---|---|
| F1 | Fence order through hashed lanes (B2) | `cluster.cpp` `uplink_sel`; `cache.cpp:3434-3449` |
| F1 | Gate round-robin pointer is updated inside the scan (`req_rr_ = (p + 1) % P` while `k` keeps counting). An exact port of the loop gives: U = 2 → one lane per clock, or the same lane twice; U = 4 / 8 / 16 with ample credit → U − 1 lanes per clock, two lanes at half rate. With 646 on 16 lanes it is correct (10.094 lines/clk, equal shares). The design's own parity case (U = 2, B = 96) is in the broken range: 1.0 line/clk instead of 1.5, or 1.5 with a lane granted twice | `noc_uplink.h` on_tick, both loops |
| F1 | A lane can pass two packets in one clock (same cause), which no RTL lane does | same |
| F1 | Response-only charging with a single bank response queue couples all GPCs; uncalibrated (B3) | `noc_uplink.h`; `cache.cpp:1975, 2112` |
| F2 | Credit above `rate + line` is discarded every clock, so a bursty stream passes less than the rate (B4) | `noc_uplink.h` `cap_` |
| F2 | The new tag-overflow check is an `assert`; release builds use `-DNDEBUG` (`sim/simx/Makefile:138`). Same for `assert(bytes_per_cycle != 0)` and the core-count check in `kmu.cpp`. Use `__assert` or a `static_assert` on the tag width | `types.h:1998`; `noc_uplink.h`; `kmu.cpp:37` |
| F2 | Responses use one arbiter for all ports, not one per port as §1.3 says | `types.h:2038, 2074` (existing class) |
| F2 | Write acknowledgements are charged a full line; the design's "a write request carries a line" is charged too, so a write costs two lines of credit across the two directions. Nothing measured supports either | `noc_uplink.h` |
| F2 | The CP and PTW clients bypass the switch and the gate. The design does not say so in §1.3 | `cluster.cpp` (unchanged binds) |
| F2 | `VX_types.toml` moves `VX_DCR_KMU_STATE_END` to 0x02D; the RTL has no decode, so rtlsim ignores the mask and places CTAs on masked cores while the app sizes its grid for the unmasked ones. No address collision | `VX_types.toml`; `processor.cpp:449` |
| F3 | All cores masked → the KMU never dispatches (hang). The mask persists across launches | `kmu.cpp` `lane_ready` |
| F3 | `bwstream_dxa` without `-l` or `-M` no longer runs the same instructions: 22 cores 59,730 instructions / 55,431 cycles against 59,004 / 56,464 | `tests/regression/bwstream_dxa/kernel.cpp` |
| F3 | `__unused(bind_l2_link)` and a lambda duplicated as `bind_delay` | `cluster.cpp` |
| — | Determinism: no unordered containers, no pointer-ordered state, all state reset in `on_reset`; `last_cycle_` reads the global cycle count, fine in one domain | — |
| — | Cardinal rule: kept; the gate and switch talk through channels only | — |

## 6. Questions only a real-H200 measurement can settle

Settled since the first draft by `h200-l2-structure.md`: the Fan conflict, near/far
latencies, partition count, sides and hash, replication, fabric bandwidth, whether
the cap applies to L2 hits.

Still open:
1. Where a saturated GPC's backlog sits. Repeat the bystander run with the saturated
   GPC reading local-only pages, then remote-only pages; and with bystanders reading
   local-only. If bystanders that share no slice-side path with the aggressor still
   slow down, the shared queue is not in the slices.
2. Is the per-GPC limit on requests or on returns? One GPC with reads that merge
   (shared lines) plus a second kernel on disjoint lines: does the disjoint kernel
   keep its share?
3. The curve at 5, 6, 7, 8 SMs in one GPC, loads and TMA, three repeats. The knee is
   still an asymptote crossing (6 SMs: 1,170–1,221; 8 SMs: 1,279–1,281).
4. Writes: 63 GB/s per SM, linear to 935 GB/s at 18 SMs, no cap reached
   (`h200_l2s_gpcsweep.txt`). "646 B/clk each way" is unmeasured. A write kernel
   that reaches more than 71 GB/s per SM is needed.
5. Does the cap follow the SM clock? Every single-GPC run was at 1.98 GHz. A run
   under the power cap (1,570–1,850 MHz) or a locked lower clock would tell whether
   the limiter is "bytes per SM clock" or a fixed rate.
6. Two kernels in one capped GPC (TMA beside loads): the shares. Settles per-port
   against per-SM arbitration.
7. Slice granule and slice → channel binding: the probe needs a kernel that is not
   itself capped at 109 GB/s per SM, and a way round same-line merging.
8. The memory roll-off between 16 and 32 spread SMs with nothing else running, with
   repeats; and the cold 16 KB transfer with a warm TLB (the 1,867 target).
9. Repeats of every `tw` configuration (each is one 0.3–1.0 s window), on a second
   die.

## 7. What awaits the design

- The design (16:36) predates `h200-l2-structure.md`. When it absorbs it, re-check:
  B_x and W for `h200g2` (B1), the bystander target (B3), the per-SM ingress as one
  shared limit (§8.1), latencies re-tagged from "lit." to "meas.", the 256 B granule
  at low confidence, and the LRC omission with its size.
- Steps 2 and 3 have no prototype; §3 (e), (f), (g) for them are review of the text.
- Not done here for lack of machine time (the node ran at load 70–100): the
  per-core isolation run in SimX, the U = 2 gate runs, the degenerate-settings
  latency run, an rtlsim run.

## 8. `h200-l2-structure.md`: recomputation and consequences

### 8.1 Headline numbers, recomputed from the raw files

Counting: `gpu/l2s_lab.cu` `tw` kernel, fixed window, bytes completed inside it,
GB = 1e9, time from `globaltimer` (`:317, 363, 395, 780`). One run per configuration.

| Claim | Raw value | Verdict |
|---|---|---|
| per-GPC 1.28 TB/s, reached at 8 SMs | 1,279–1,285 (loads), 1,281–1,285 (TMA) for 8–18 SMs; all 8 GPCs at 12 SMs 1,283.5–1,285.0 (`e1:340-382`); L2 disjoint 1,270–1,289 | confirmed |
| fabric 1.28 TB/s per direction | side A alone 1,283.8, side B alone 1,283.9; both at once 1,281.0 + 1,280.8 (`local:154, 172, 262`) | confirmed, measured in both directions at once. "Fabric" is a reading |
| local-only 4.63, remote-only 2.56 | 4,628.7 (2 × 2,314.3) and 4,634.2 (TMA); 2,561.7 and 2,561.3 | confirmed |
| latency 290 / 467 / 546 / 733 | near p50 293–297 (local-home 290–294); far p50 466–467; cold near-home median 545–548; far-home 730–736 | confirmed. All at 1,974–1,980 MHz, so 146 / 236 / 276 / 370 ns |
| bystander 192 → 88 / 106 | TMA: 191.5–192.3 alone; 88.3–88.5 same half, 105.8–105.9 other half; aggressor 79.8–80.0 per SM (1,278.0). Loads: 196 → 92.5 / 132.2 | confirmed. One run each |
| shared buffer 2.75 TB/s at 18 SMs | 2,753 (256 KB), 2,276 (4 MB); disjoint 1,286 | confirmed (§1.5) |
| per-SM ingress 215–230 shared by TMA and loads | same CTA: 78.1 + 137.0 = 215.1; 124.5 + 105.1 = 229.6. Alone: TMA 196–204, loads 218 | the sum caps: confirmed, 109–116 B/clk. With 4 KB tiles the sum is 174–197, below loads alone, so it is not one constant. Port or in-flight budget: not separated |
| "two packed GPCs = 2.28" was an artefact | 28 pairs, 12 + 12 SMs: 2,537.3–2,557.8. Same half 2,537–2,542, opposite halves 2,555–2,558. The old 2,283.4 (`h200_tpc_network.txt:499`) is 32 × 71.4 = n × slowest SM with 18 + 14 SMs | confirmed for loads. For TMA the overlap rate is 18 × 71.4 + 14 × 83.7 = 2,457, still 4% under. Same-half pairs are 0.7% lower, systematically |
| rule: total = min(GPCs × 1.28, 2.56 per half, ~4.6) | holds within 0 to −6% when every active GPC is saturated (one half with 3 or 4 GPCs: 2,563–2,565) | **fails by −28 to −33% with bystanders, and by −5 to −14% for 2–4 SMs per GPC spread**. It is a rule for saturated GPCs only |

Inferences in the note:
- Partition hash {12, 13, 15, 17, 19} plus a page constant: fact within 8 MB.
- 256 B slice granule: low confidence (§1.2).
- Replication of remote reads: strong direct evidence; where the copy lives is
  inferred.
- The 280 / 305 split of near hits: the pattern is real (agreement 0.84–0.95 within
  GPCs {0–3} and within {4–7}, 0.15–0.17 across); its meaning is not known. It is
  ±12 cycles and the design can leave it out.

Clock:
- The SM clock fell to 1,568–1,853 MHz only in the chip-wide saturation runs
  (`e1:82-94, 136-148`, `e2:40, 46`), under a 560 W power cap. Every latency,
  per-GPC, pair, bystander and ingress run was at 1,974–1,980 MHz.
- GB/s come from wall-clock windows, so they are comparable with the earlier
  `h200_tpc_*` and `h200_borrow_*` figures. The 648 and 109–116 B/clk conversions
  use 1.98 GHz runs and need no correction.
- The chip-level load totals taken at 1,568–1,658 MHz (4,332–4,464) are about 2.5%
  low; TMA at 1,797 MHz about 1.1% low. A B/clk conversion of those runs at 1.98 GHz
  would be off by up to 1.26×.
- The earlier logs record only the maximum clock, not the running one. Their cap
  (1,274–1,285) and roof (4,626–4,632) agree with the new ones within 0.5%, on a
  different die and with a different timing method.

### 8.2 The model against the bystander effect and the saturation rule

Half-scale copy of the part in my model: 2 halves × 4 GPCs × 8 SMs, GPC cap
323 B/clk, 323 B/clk per direction between halves, side memory 1.157 TB/s on
16 slices, one bystander SM per other GPC (58% of memory, as on the part), each SM
with 970 lines in flight (192 GB/s alone). GB/s per SM:

| Variant | bystanders alone | + saturated GPC (8 SMs): same half / other half | saturated GPC, per SM | total, GB/s | + unsaturated GPC (3 SMs) |
|---|---|---|---|---|---|
| part (16-SM GPC, TMA) | 192 | **88 / 106** | 80 | 2,655 (< 2,687 alone) | 180 / 182 (6 SMs) |
| A. gate on responses; one response queue per slice | 185 | **84 / 86** | 80 | 1,233 (< 1,294 alone) | 184 / 185 |
| B. gate on responses; a response queue per source, one fill queue per slice | 185 | 75 / 83 | 80 | 1,199 | 183 / 185 |
| C. gate holds requests at the GPC | 185 | 178 / 181 | 79 | 1,890 (= sum) | 184 / 185 |

- **What reproduces bystanders slowing in both halves: a return-side limit per GPC
  together with a per-slice FIFO on the return path** (the slice's response queue in
  A, its fill queue from memory in B). The saturated GPC's requests enter the slices
  of both halves; their responses cannot leave faster than the GPC's limit; the
  slice queue is blocked at its head and every other SM's lines wait behind it.
- Holding the excess at the GPC's own port (C) does not: bystanders lose 2–4% and
  the total is the plain sum, which the part contradicts (2,655, not 3,965).
- The model reproduces the same-half figure within 5% and the direction of the
  total. It gives the other half 86 against 106 (−19%): on the part the far half is
  hit less, so something there is not shared FIFO-style, or far lines of the
  aggressor are fewer in those queues than my 50/50 split puts there. Not explained.
- It also reproduces the control: an unsaturated GPC leaves bystanders alone (the
  part: −6%, the model: −0.5%).
- For the design: charging responses (as the prototype does) is the placement the
  part's behaviour supports. Charging requests instead, which I would otherwise have
  recommended for isolation, is ruled out by the measurement. The response-side
  queue that takes the backlog must be chosen and sized on purpose; in Vortex it
  is the bank's single core-response queue plus the 130-entry uplink buffers.

Saturation rule in the model (from §4 and the runs above): one GPC = its cap; a
half whose GPCs all read mixed pages = 2 × the per-direction cap, because each SM's
window fills with far requests and the near half waits behind them at the lane
heads. The same mechanism gives the part's "one half never exceeds 2.56 TB/s".

### 8.3 `h200g2` with the measured values: what binds

Part: GPC 1.28, per-direction link 1.28, side memory 2.31 (TB/s), ratio 1 : 1 : 1.8.
`h200g2`: GPC 1.28, side memory 0.77 (one stack at 96.5%), ratio 1 : B_x : 0.6.

| Placement, pages | part | `h200g2`, B_x = 1.28 | `h200g2`, B_x = 0.42 (design default) |
|---|---|---|---|
| packed in one GPC, mixed | 1.28, GPC | 1.28, GPC (far half 0.64 of a 0.77 side) | **0.84, link** |
| packed, local pages only | 1.28, GPC | **0.77, side memory** | 0.77, side memory |
| packed, remote pages only | 1.28, GPC = link | **0.77, other side's memory** | 0.42, link |
| spread 8 + 8, mixed | 2.56, GPCs | **1.54, memory** | 1.54, memory (0.39 far per direction, just under the link) |
| spread, local only | 2.56 | 1.54, memory | 1.54 |
| spread, remote only | 2.56, link = GPCs | 1.54, memory | **0.84, link** |
| all SMs, local / remote | 4.63 / 2.56 | 1.54 / 1.54 | 1.54 / 0.84 |

- With B_x = B_g the GPC limit shows (packed, mixed) and the link never binds:
  local-only and remote-only pages give the same totals, where the part gives 1.8×.
- With the design's B_x the local/remote contrast shows (1.8×, as the part) but the
  GPC limit does not: a packed GPC is link-bound.
- No B_x gives both, because side memory (0.77) is below the GPC cap (1.28) here and
  1.8× above it on the part. B_x between 0.64 and 0.77 TB/s (323–390 B/clk) keeps a
  packed GPC gate-bound and remote below local, but only by 0–17%, and my model
  loses 4–6% to head-of-line effects at that margin.
- Packed with local-only or remote-only pages is memory-bound at 0.6 of the cap. A
  page-set experiment on `h200g2` therefore says nothing about the GPC or the link.
- A bystander in `h200g2` is always in the other half (one GPC per partition): the
  same-half case (88 against 106 on the part) cannot be reproduced.
- Recommendation: `h200g2` with B_x = B_g for GPC-limit and isolation studies, and
  no local-against-remote page claims from it. Side memory reaches the part's
  1.8 GPC caps only with 3 stacks per side, i.e. at full size; the 64-SM / 3-stack
  shape has 1.16 TB/s per side (0.9 caps), closer but still memory-bound for one
  packed GPC on local pages.

## 9. Rev. 3 of the design (17:22): what it changed and what is left

Read: §0, §1.1–1.7, §4.2–4.5, §9. Not re-read line by line: §2, §3, §5, §6.

Checked and agreed:
- `h200g2` "what binds" table (§4.2) matches my §8.3: packed mixed 1.28 (GPC),
  spread 1.544 (memory), packed local-only or remote-only 0.772 (memory), fabric
  never binds; stated as deviations.
- Knob values: B_x 648 / W 16; `PLATFORM_MEMORY_LATENCY` 174 (546 − 290 − 82);
  L_x 88 for far − near 177; MSHR 256; memory per SM 1.375×; 640 KiB.
- `h200g4` (4 × 16, 3 stacks): side memory 1.16 TB/s against a 1.28 cap; packed 16
  against spread 16 = 1.28 / 2.32 = 1.8×. Arithmetic right. Its fabric does not
  bind either (2 GPCs on a side: 2 × 1.28 = the side's 2 × fabric limit, and memory
  binds first).
- The bystander mechanism (§1.5) is the one my model supports, with the
  request-side gate as the control.

Still wrong or open in rev. 3:
1. **§4.2 contradicts itself**: the deviation table says the same-half bystander case
   cannot be reproduced in `h200g2`; the "Preserved" list below it says "the
   bystander mechanism (same and other half)". Only the other-half case exists.
2. **The new per-SM ingress gate holds responses at the socket.** A streaming SM
   sits at its ingress limit all the time, so its responses wait at the heads of the
   GPC's response lanes, and those lanes are shared by the other SMs of the GPC. On
   the part, SMs in an unsaturated GPC do not slow each other (6 SMs: 195 each
   against 196 alone). Unless the switch gets a response queue per destination
   port, this gate will add an in-GPC slowdown the part does not have. Limiting the
   SM's issue rate (requests) has no such side effect and fits "in-flight-bound".
3. **B2 is accepted but the prototype is unchanged**; every result with a fence or a
   flush between memory phases is unsafe until it is. See the fence run in §5.
4. **The gate was rewritten** (round-robin pointer, credit kept across clocks,
   wake-up). Credit that accumulates needs a stated bound: an unbounded bucket lets
   a GPC that was idle burst far above 648 B/clk, which the part's flat plateau
   does not show. I have not reviewed or run the new code; my §5 runs are from the
   16:12 patch.
5. `L2_SLICE_GRANULE` 256 is tagged "meas. (medium)". The probe supports it at low
   confidence (§1.2).
6. §4.2 "all 32, mixed: fabric 0.772" is the two directions together; per direction
   it is 0.386.
7. Bystander depths are "fitted once to the E-1 table". In RTL they are structure
   (a 2^n delay FIFO per lane, one bank response queue), so the fit must be done on
   values the RTL can have, and the fitted run must also keep the unsaturated
   control (−6%) and the pair result (−1%). My small SimX runs show two saturated
   groups costing each other 8% (§5), which is already more than the part.
8. The model gives the other half 86 where the part gives 106. Rev. 3 notes it;
   nothing in the design produces a same-half / other-half difference except the
   shared fabric direction, which my model also has. Open.
9. §7 is empty again and there is no §8; the validation list of §1.5 refers to runs
   (`noc4_by_*`) that I could not check.
10. Replication is now step 3b with write-to-home + invalidate. Not reviewed beyond
    the text; it needs its own correctness review (a copy and an atomic on the same
    line, an invalidate racing a far read).
