# sm-rate: why one simulated SM moves memory at half the H200 rate (running log)

Agent `sm-rate`, started 2026-11-22. Branch `feature/sm-rate` (worktree `vortex-exp/wt_smr`, base 4001ed275).
Frozen source `vortex-exp/src_smr_base` (= 4001ed275, third_party from src_wl_n, identical submodule revs).
Scripts `vortex-exp/slurm/smr/` (bundle.sbatch: one build, many points `name|app|args|xconf|perfclass`; sub.sh; col.sh).
Results `results/smr_<name>/`. Perf classes are exclusive: 1 core, 4 dcache, 5 L2, 7 memory, 16 DXA.

## Log
- 11-22: setup. b0 = baseline points (DXA copy, bwstream, ldstream, warp_rate) at several perf classes.
- 11-22 13:30: b0 baseline reproduced (verified by me, SimX, src_smr_base, PASSED checked):
  - DXA copy `bwstream_dxa -e4096 -n256 -d4 -c0`: default grid (3 CTAs) 244,761 cyc = 51.4 B/clk whole run;
    1 CTA 156,274 = 26.8. Transfer latency (engine start->finish) 2212 / 1162 cyc.
  - 1 CTA depth sweep: d1 1643 cyc/tile, d2 831, d4 610, d8 616. d8 with 2 or 3 CTAs = same total rate as
    1 CTA, because depth 8 x 16 KB = 128 KB leaves ONE CTA slot in 228 KB of shared memory (CTAs run in turn).
    => one issuing warp cannot issue faster than one 16 KB transfer per ~610 cycles (48 instr/tile, 12.7 cyc/instr).
  - bwstream -n2048 -u4 -b32: 40.9 B/clk, load_lat 656. Shape has 64 warps x 4 loads x 128 B = 32 KB in flight:
    Little's law bound 32768/656 = 50 B/clk. The part's 218 GB/s kernel (l2s k_bw) has 32 warps x 8 x 512 B = 128 KB
    in flight (16 B loads): NOT the same shape. Like-for-like is ldstream.
  - ldstream lean 128 KB: 69.8 B/clk (load_lat 699); probe shape 38.5.
  - warp_rate sts_own 64w: 4.02 cyc/store; lds_own 2.04 cyc/load; these are SHARED-memory accesses.
- Structure read in RTL (not yet all confirmed by sweeps):
  - LMEM (VX_lmem_unit g_dual_arb + VX_lmem_engine): engine A takes ONE LSU block's request (8 lanes) per cycle,
    engine B (read only) a second; writes use A alone and B takes nothing while a write waits.
    => shared stores 8 lanes/cycle (a 32-thread store = 4 cycles), shared loads 16 lanes/cycle (2 cycles).
  - dcache bank (VX_cache_bank.sv:411-431): one of {replay, fill, flush, core req} enters per cycle per bank;
    fill held while the forward chain drains (fwd_pending). DCACHE_WORD=32 B so a 64 B line = 2 core requests.
  - DXA: 8 workers -> gmem_arb -> ONE line/cycle port (VX_dxa_core GMEM_OUT_PORTS=1) -> unit MMU -> socket port 0;
    smem_wr drains one 64 B line per beat (half a 128 B row), lmem_arb 8:1 -> one beat per cycle.
- Running: s1a-d (knob sweeps), s1s (stores, src_smr_k1 with new warp_rate kinds 13-15), i1a/i1b (src_smr_i1,
  instrumented: SMR_DXA_STATS, SMR_PORT_STATS, SMR_BANK_STATS, PHASE_TIMES in bwstream_dxa).
- 11-22 13:45 sweeps s1 (verified, SimX, src_smr_base / src_smr_k1; tables via slurm/smr/tab.py):
  - Stores/loads on shared memory, 64 warps, cycles per instruction: sts_own 4.02, sts_own8 (sd) 4.03,
    sts_own4 (4-byte-spaced sw) 12.0, lds_nouse 2.02, lds_nouse4 2.02, lds_own 2.04/load.
    NUM_LSU_LANES=16: sts_own 2.02, sts_own4 6.0, lds 1.03. One warp sts_own: 6.71 (8 lanes), 3.91 (16 lanes); part 3.98.
    => LMEM takes one block request per cycle for writes, two for reads; a same-word pair of write lanes is a conflict
    (extra pass, 2 cycles each).
  - DXA copy 3 CTAs x D4 (B/clk whole run): base 51.4; L2 link latency 47: 51.7; memory latency 150: 57.7;
    16 workers: 60.4 (port at 0.99 line/cycle); 2 gmem ports, 8 workers: 52.0 (transfer latency 2212 -> 2022);
    dcache banks 8: 50.7; MSHR/pending 1024/2048: 51.4. 1 CTA: 26.8-27.1 in every variant.
  - ldstream lean 128 KB: 69.8 base; 69.9 (MSHR 1024/2048); 71.4 (8 banks, load_lat 699 -> 1095); 69.9 (link 47);
    70.3 (memory latency 150). Probe shape 38.5 in every variant (IPC 1.45).
  - bwstream -u4 -b32: 40.9; link 47: 44.5; -u8 is SLOWER (29.6); 8 banks 29.5 (but 41.7 with MSHR 2048). Open.
  - ldstream -u16 spills registers (1.1M stores): not usable.
- Units: gemm_tmem_decode prints B/clock; its 100.5 GB/s = 50.8 B/clock = the copy's rate. No conflict with 52.1 B/clock.
- 11-22 13:55 instrumented runs (src_smr_i1/i2: SMR_* counters, experimental; commits on feature/sm-rate):
  - DXA 3 CTAs x D4, 8 workers (j1_base_dx3): 239,903 active cycles; workers running 7.08 (issuing 4.54, waiting on
    responses 2.54), idle 0.92; requests sent in 81% of active cycles (0.82 line/cycle); 893,561 worker-cycles blocked
    at the port (3.7 per cycle); queue non-empty only 765 cycles. => the single gmem port is the bottleneck and it
    still idles 19% of the time: every worker is one transfer at a time, the 8 run in step, and after the last line
    is issued each waits a memory round trip for its tail. 16 workers (12 transfers outstanding) fill the port to 0.99.
    SimX writes two half-row beats in one cycle 13% of the time (two workers); the RTL has one smem bus (one beat).
  - DXA 1 CTA D4 (j1_base_dx1): 1.96 workers running, 6.04 idle; port used 43% of cycles. Issue-bound by the kernel.
    Phase times per tile (PHASE_TIMES, 5 extra csr reads; 733 cyc/tile): issue 323, wait 124, read 152, wait2 27,
    loop 107. The kernel computes `j % depth` and `i % depth` with a runtime depth (two divisions per tile).
  - Loads, dcache bank counters (i1_base_*; per bank, 4 banks):
    ldl: 66.5K lines per bank in 235K busy ticks = 3.54 ticks per line; inputs core 135.5K (2 per line), fill 66.5K;
    forward window active 162K ticks, a fill waiting 159K ticks (68%), all behind the forward window; forward held
    by a full response queue 28.8K. bw: forward window 267K ticks (4.07/line), held by response queue 119K, fill
    waiting 220K (54%). ldp: forward held 170K (43%), bank idle half the time.
  - Separation runs (i2): ldl 69.5 with L2-resident data (lat 244), 70.9 with link+memory latency 0, 69.8 DRAM:
    the lean stream's ceiling is inside the SM, not memory. ldp 38.8-38.9 in L1-resident, L2-resident, zero-latency
    and DRAM runs: instruction-bound (IPC 1.45-1.47); GAPLESS=1 gives 53.5 (IPC 2.0). bwstream -u4 with zero latency
    47.3, -u8 47.8. ldl by warps: 8 -> 18.6, 16 -> 36.7, 32 -> 62.5, 64 -> 69.8. ldl by unroll: 21.8/42.4/67.1/69.8.
    MSHR/pending 256: ldl 51.4 (binds from below). GAPLESS=1: ldl 70.0 (no change).
  - Zero latency makes the DXA copy SLOWER (25.4 B/clk, transfer 4727 cyc). Open; rerun with counters queued.
  - Stores, more: lanes 32 (whole warp per request): sts_own 3.02, sts_own4 3.02, lds_nouse4 0.53 (1.9 loads/cycle,
    above the part), lds_nouse 1.53. LMEM banks 32: identical to base (banks are not the limit).
- 11-22 14:25 (all SimX, verified by me; k1/k2 = dcache sweeps on src_smr_i2, j1 = DXA counters on src_smr_i2/i3,
  h1 = SMR_SMEM_HALF experiment on src_smr_i4):
  - dcache word 64 B (-DVX_CFG_DCACHE_WORD_SIZE=64, one request per line): ldl 69.8 -> 93.5 B/clk; bank ticks per line
    3.54 -> 2.58. With CRSQ 16: 98.3. With 2 banks: 61.0 (bank saturated at 2.05 ticks/line). With 8 banks: 81.4.
    Word 32 with 8 / 16 banks: 71.4 / 74.1 (forward held by the response queue 54% of busy ticks at 8 banks).
    CRSQ 16 alone: 69.8. FWD_HOLD=0: 41.4. ldp unchanged by all (38.5); ldp + word 64 + GAPLESS=3: 55.4.
    bwstream -u4 unchanged by word size (40.5).
  - DXA, RING_NOMOD kernel (no division): 1 CTA D4 162,961 cycles (156,274 with the division). The division is not
    the cost. Phase times per tile, no-division kernel: issue 330, wait 112, read 238, wait2 26, loop 95.
  - DXA counters: 16 workers/1 port: port busy 96% of active cycles. 2 ports/16 workers: 53.8 (below 60.4), port
    split 150K/46K lines, responses blocked 135K, beats blocked 145K: the shared-memory write side binds.
    Decode kernel (gemm_tmem_decode -m16 -n8192 -k4096, KSTEPS=2 PIPE=6 XFER=8192): 51.3 B/clk in the multiply,
    8 workers always busy, port busy 80%, requests queued 89% of the time: same engine cap as the copy.
    8 KB tiles, 2 ports: 52.0, workers 7.98 busy with 6.39 waiting on responses: worker count binds small tiles.
  - SMR_SMEM_HALF (SimX-only experiment: a beat waits only for its own row half; RTL has ONE smem bus):
    3 CTAs x D4 / 7 CTAs x D2 / 8 KB tiles, B/clk whole run:
    1 port 8 workers 51.7 / 52.9 / 49.7; 2 ports 8 workers 59.1 / 62.7 / 63.5; 2 ports 16 workers 60.8 / 62.8 / 65.4;
    4 ports 16 workers 61.3 / 73.8 / 67.0. Without it: 2p8w 52.0 / 52.7 / 52.0; 4p16w 54.5 / 59.9 / 52.1.
    => ports and the smem write are co-limits; after both, responses blocked at the workers (one line per two cycles
    per worker) and, for 3 CTAs, the issuing loops (3 x one tile per 610 cycles = 80 B/clk at most).
  - Zero link + memory latency: DXA 25.4 B/clk; 6.84 workers issuing, port passes 0.40 line/cycle. Not an h200s
    condition (link 94); a property of the port path without the link pipeline.
  - SimX debug build (--debug) does not compile here (assert macro clash in ramulator headers): no trace taken.
- Queued: rt1 (rtlsim, warp_rate kinds 11-14 at 16 warps, src_smr_k1; SimX same args: sts_own 4.26, sts_own4 12.2,
  lds_nouse 2.36, lds_nouse4 2.36 cycles per instruction), objd (kernel disassembly), h1a1, k2a.
- 11-22 14:50:
  - Disassembly (results/smr_objd_dxa/kernel.dump): the bwstream_dxa loop is 48 instructions per tile: 3 branches,
    5 split/join, 3 barrier ops (expect_tx, 2 x arrive_and_wait), wgather + DXA issue, 2 remuw, 3 mul, 3 stack loads,
    1 shared load, ~26 plain ALU. 13 of them are control/SFU instructions, each a pipeline round trip for a lone warp.
  - Disassembly (results/smr_objd_bw/kernel.dump): bwstream -u8 issues 4 loads, 4 adds, then loads and adds mixed
    (the compiler reuses 4-5 temporaries): at most ~4 loads per warp in flight, in two groups. -u8 is a kernel
    artifact, not a hardware effect. -u4 issues its 4 loads before the adds.
  - SMR_LMEM_WR_MERGE (SimX-only experiment, src_smr_i5, all PASSED): sts_own4 64 warps 12.0 -> 4.02 cycles
    (one warp 12.5 -> 6.71); with 16 lanes 2.02; with 32 lanes 1.03 cycles = 0.97 stores per cycle (part 1.02),
    one warp 3.31 (part 3.98: 17% faster than the part). 8-byte-spaced stores unchanged (4.02 / 2.02 / 3.02).
  - Word 64 + zero latency: ldl 83.0 (below 93.5 with latency; zero-latency port effect). 2 ports + 32 workers: 59.9.
- Patch of every experimental change: notes/sm_rate_experiments.patch (= feature/sm-rate 4001ed275..974957321).
- Still running at report time: smr_rt1 (rtlsim, src_smr_k1, warp_rate -k11/-k13/-k12/-k14 -w16 -i2; results in
  results/smr_rt_k*w16; SimX same args in results/smr_rs_k*w16: cyc_per_iter_sm 8718 / 24937 / 4841 / 4843).
- 11-22 15:00 (o1, src_smr_i6, SimX, verified):
  - SMR_SMEM_ONEBEAT (one DXA beat per cycle, as the RTL's single SMEM bus): 3 CTAs x D4 51.4 -> 48.6 B/clk;
    7 CTAs x D2 53.5 -> 52.8; `-e4096 -n32 -g16 -d4 -c0` 176,203 -> 185,278 cycles (+5.2%). The rtlsim reference for
    that point (others' run, older tree) was 201,583 against SimX 182,421 (+10.5%): the two-beats-per-cycle freedom in
    SimX accounts for about half of that parity gap. Experimental; not the parity fix itself.
  - bwstream_dxa -c1 (each thread sums the tile it was sent): 3.5 B/clk (3 CTAs), 7.7 (7 CTAs). One warp per CTA and a
    single dependent FP accumulator: bound by the consumer warp's instruction rate (IPC 0.25), not by memory. Not
    comparable with the part's staged-then-consumed 157-170 GB/s (many consumer warps).
- 11-22 15:50 (w2, src_smr_k2 = k1 + warp_rate kinds 16-19; SimX, PASSED): one warp, cycles per instruction:
  int_ind8 3.84, int_dep 13.69, br_every1 20.33, sts_own 6.59, csr read 12.85, split or join 27.54 (55 per pair),
  barrier that is already satisfied 19.16, dependent remuw 16.87. 64 warps: csr 2.06, split/join 1.60, barrier 1.38.
  => the bwstream_dxa loop's 5 split/join cost ~138 cycles per tile, 3 branches ~61, 3 barrier ops ~58, 2 remuw ~34.
  bwstream_dxa's kernel argument is not __UNIFORM__ (bwstream's is), which is why uniform tests compile to split/join.
  Queued: u1 (UNIFORM_ARG variant, src_smr_i7).
- 11-22 16:12 (u1, src_smr_i7, SimX, PASSED): bwstream_dxa with `__UNIFORM__` on the kernel argument (UNIFORM_ARG):
  1 CTA D4 156,274 -> 128,033 cycles (26.8 -> 32.8 B/clk, 500 cycles per tile, 39.5 instructions per tile); D8 the
  same (32.6); D1 1549 cycles per tile. Phases per tile: issue 261, wait 119, read 158, wait2 32, loop 63.
  3 CTAs x D4 unchanged (50.6: engine-bound). With 2 ports + 16 workers + SMR_SMEM_HALF: 62.6 (3 x D4), 65.0 (7 x D2).
- 11-22 17:05 rtlsim confirmation (job smr_rt1, h200s, 1 core, src_smr_k1, 16 warps, -i2; all PASSED, instruction
  counts equal), cyc_per_iter_sm rtlsim vs SimX: sts_own 8707 vs 8718 (-0.1%); sts_own4 24915.5 vs 24937 (-0.1%);
  lds_nouse 4883 vs 4841.5 (+0.9%); lds_nouse4 4885 vs 4843.5 (+0.9%). The 4-cycle store and the 12-cycle
  4-byte-spaced store are RTL behaviour, and SimX matches it.
- State at the end: branch feature/sm-rate (worktree wt_smr), not pushed; patch notes/sm_rate_experiments.patch;
  frozen copies src_smr_{base,k1,k2,i1..i7} (hardlinked to each other: never edit a file in place);
  no job of mine left in the queue.
- 11-22 evening: new assignment (team-lead): implement LMEM fixes 1/2 and test kinds; match the part (no "never exceed"
  rule); targets H200 one CTA 4-byte: stores and loads 1.000 per clock at 8/16/32 warps, one warp 4.06 / 5.05 cycles.
  - Branch feature/sm-rate rebased onto f55faaa2d. Source copies: src_smr_n1 (rebased experiments), src_smr_n2 (LMEM knobs WIP).
  - Knobs VX_CFG_LMEM_WR_MERGE / VX_CFG_LMEM_WR_ROW (default 0): RTL VX_lmem_engine.sv (riders: store lanes with no
    conflict against any lower pending lane of their bank), VX_lmem_unit.sv (g_store_engines: engine per block, bank
    grant in rotating order, reads gated while a store is at an input or in a store engine); SimX local_mem.cpp mirror.
  - Tests: smem_rate (= my warp_rate kinds 9-20; warp_rate restored to upstream), lmem_wr_merge (directed bytes).
  - ICE helpers: slurm/smr/isub.sh (bundle on ICE; PROF=none for no profile; DRIVER=rtlsim), ifetch.sh, ice_bundle.sbatch.
  - ICE jobs: l1a/l1b (h200s SimX off/mrg/row/both -> results smr_l1_*), pra (rtlsim small shape both+off -> smr_pr_*),
    psa (SimX small shape -> smr_ps_*). Small shape: 16 warps x 32 threads, issue 4, ALU 16, LSU 8 lanes, 16 banks.
  - bwstream drift (409K -> 449K): two levels selected by timing; slow level = decode held by full per-warp instruction
    buffers (ibuf stall 56-58%); IBUF_SIZE=8 or FETCH_INFLIGHT<=2 remove it (results smr_q1_*). Sent to team-lead.
- 11-22 late (ICE; SimX h200s 1 SM unless said; all PASSED; feature/sm-rate head 6c412ef06 + later test commits):
  - Knobs OFF = identical to the lead's baselines on f55faaa2d: fetch_squash 10,018; warp_rate 113,382; tmem_roof
    -n512 -i8 62,075; bwstream -n2048 -u4 -b32 449,189; gemm_block 562,462; gemm_tmem_decode 1,320,528 (smr_l3_off_*,
    smr_l3g_off_*). gemm_block and decode are also unchanged with both knobs ON.
  - smem_rate, cycles per store or load instruction (cyc_per_iter_sm / (128 x W)), W = 1 / 8 / 16 / 32 / 64:
    sts_own4 (4-byte): off 12.46 / 12.07* / - / - / 12.02; merge 5.68 / - / - / - / 4.02; row 12.46 / 3.11 / 3.06 / 3.03 / 3.02;
    both 5.68 / 1.59 / 1.345 / 1.28 / 1.27; both + LANE_DISPATCH_GAPLESS=3 4.80 / 1.47 / 1.12 / 1.03 / 1.03.
    sts_own (8-byte spacing): off 5.68 / - / - / - / 4.02; row or both 5.68 / 2.24 / 2.05 / 2.03 / 2.02.
    loads (lds_nouse, lds_nouse4, lds_own, lds_own4): unchanged by either knob (2.02 per load at 64 warps, one warp 5.79).
    (* from l3_g3off rows; see results smr_l1_*, smr_l3_*.)
  - Directed test lmem_wr_merge passes with off/merge/row/both at 2, 16, 64 warps (h200s SimX) and at the small shape
    on rtlsim and SimX. war_order identical in all four (342,345). ldst_order needs larger args at h200s (not run there).
  - Small-shape parity (16 warps x 32 threads, issue 4, ALU 16, LSU 8 lanes, 16 banks, XLEN 64, no profile), rtlsim vs
    SimX per iteration: store kinds 11/13/15 at 1/8/16 warps within 0.16% in off, merge, row, both. Read kinds differ
    the same in all four configs (lds_own4 16 warps -1.77%, lds_own -0.26%): not from this change. Knob-off rtlsim on
    the tree before the change (src_smr_n1) = after: lmem_access 220,431, war_order 334,086, ldst_order 2,220.
  - OPEN parity item: lmem_wr_merge -w16 with ROW only: rtlsim 4887 vs SimX 5176 (-5.6%); both knobs +0.9%. Bisected
    (-c mask): only the cases where >= 4 lanes write the same bytes (cases 8, 16, 32) differ, +25..32 cycles in RTL
    each, and only with >= 3 warps per LSU block (12 or 16 warps; 8 warps equal). LMEM request and replay counts are
    equal in both models. Stream kind sts_same (smem_rate -k21) running on both models to isolate.
  - bwstream_dxa uniform argument: own commit 6c412ef06 with before/after at 1/4/11/22 cores.
  - bwstream -DBWSTREAM_HOIST: -u8 557,939 -> 448,858 cycles but instructions double (841,856 vs 432,448): the
    form of the hoist costs instructions; -u4 gets slower with it (449,189 -> 485,356). Needs a better form.
- 11-23 early: stream kind sts_same (smem_rate -k21, every lane the same word), small shape, ROW on: rtlsim vs SimX
  +0.14% (16 warps), +0.06% (12), +0.09% (8); sts_own4 at 12 warps +0.01%. So the engines and the bank grant agree in
  steady state; the lmem_wr_merge -w16 ROW-only difference (-5.6%) is a transient of one backlogged store instruction
  per warp followed by a barrier (>= 3 warps per block, >= 4 lanes on the same bytes). NOT explained. Next step: LMEM
  request/pass trace on both models for `lmem_wr_merge -w12 -c8` (RTL: DBG_TRACE_MEM lines in VX_lmem_engine/unit).
  - lmem_rdport and lmem_access pass in off/merge/row/both at h200s (SimX).
  - NOT run: the new CI parity cases through ci/testcase.py; rtlsim at the CI shapes (16 threads); h200s-size rtlsim.
  - State: feature/sm-rate head 471078edb, everything committed, nothing pushed; no ICE or Phoenix job of mine left.
    Source copies src_smr_n1..n6 (n6 = head). Phoenix copies share third_party by hardlink.
- 11-23: the ROW-only transient is located (trace SMR_LMEM_TRACE on both models, `lmem_wr_merge -w12 -c8`, results
  smr_tr_* / smr_ts_*): store takes and passes at the LMEM are cycle-identical in rtlsim and SimX (384 passes in 386
  cycles, same take times). The difference is after the burst: the first copy-out load reaches the LMEM 37 cycles after
  the last store pass in RTL, and is already waiting (taken 6 cycles after) in SimX. Cause: between an LSU block and the
  LMEM the RTL has a ONE-entry buffer (VX_lsu_mem_path.sv:128 LOCAL_OUT_BUF(1)); SimX's channel into LocalMem holds TWO
  (default capacity). With the LMEM backed up, SimX's LSU has handed over one more request, so its scheduler is empty
  (the scope-0 barrier's drain condition) one LMEM service interval earlier: ~32 cycles when four engines share one bank
  with 8 passes per request. Pre-existing, knob-independent; hidden with the knobs off because the single write engine's
  drain bounds both models. Check: SimX with a 1-entry channel (SMR_LMEM_IN_CAP=1) gives 2194 against rtlsim 2190 on
  that case (2153 before) but halves every stream's rate (a 1-entry SimX channel is not a pipe buffer), so it is not
  the fix. A proper fix is a SimX pipe-buffer form for that hop (the LMEM's ready is from registers with the knobs
  off; with LMEM_WR_ROW it depends on the request's type), a base-model change needing its own parity pass. NOT done.
  - With BAR_DRAIN_SCOPE=1 (what h200s uses), small shape: lmem_wr_merge -w16 row -1.5%, both +0.9%, -w12 -c8 +0.26%;
    streams within 0.12% (smr_er_* / smr_es_*).
- Branch rebuilt on dd94ad142 as a clean series: 983902e3f tests, 1eb169ac8 lmem knobs, 37093a18a bwstream_dxa uniform
  (= merge candidate), 9f0abaf45 experiments NOT FOR MERGE. Old history: tag sm-rate-pre-rebase (190c058a4).
  Copy src_smr_n9 = 37093a18a. Jobs: ci9 (pytest parity cases, XLEN 64), l9 (h200s four configs), pr9/ps9 (small parity).
- 11-23 later: branch feature/sm-rate on dd94ad142: 983902e3f tests; 71bebb13f lmem knobs (+ CI cases keyed xlen [64],
  changelog); bd9927d95 bwstream_dxa uniform = MERGE CANDIDATE; 3e587e453 experiments NOT FOR MERGE (head).
  (An earlier build of this series, 37093a18a, had a mis-merged core.yaml: fixed; src_smr_n9 = bd9927d95.)
  - Planner (pytest ci/test_runner.py, XLEN 64, ICE): the six parity cases pass; cycle gaps 0.16 / 0.16 / 0.12 / 0.02 /
    0.02 / 0.01% (results smr_ci9, smr_ci9b). pytest was pip-installed --user on ICE.
  - Knobs off on the candidate = pristine dd94ad142 (h200s SimX, smr_d0*, smr_l9*): fetch_squash 10,018; warp_rate
    113,382; tmem_roof 62,075; bwstream 452,761; war_order 342,988; lmem_rdport 488,417; gemm_block 576,342;
    gemm_tmem_decode 1,320,528.
  - Four configurations re-taken on dd94ad142 (h200s SimX): sts_own4 instr/cycle at 8/16/32/64 warps: off 0.083 x4;
    merge 0.243/0.247/0.248/0.249; row 0.320/0.328/0.330/0.331; both 0.621/0.738/0.777/0.786; one warp 12.38 / 5.62 /
    12.38 / 5.62 cycles. sts_own: off 0.244..0.249, row/both 0.448/0.489/0.492/0.495, one warp 5.57. lds_own and
    lds_own4 unchanged in all four (0.694/0.902/0.974/0.982; one warp 6.04 / 6.01).
  - Small-shape parity on the candidate (smr_pr9_* / smr_ps9_*): store and load streams within 0.15% in all four;
    lmem_wr_merge -w16: off (see ps9), merge +1.96%, row -1.47%, both +0.85% (the LSU-to-LMEM buffer depth, above).
  - No job of mine left on ICE or Phoenix.
- 11-23, review of dd94ad142..bd9927d95 (lmem-review): land after fixes. Branch now: bd9927d95 + 27111bf5a (store
  engines' writes reach the reservation commit stage; WR_ROW refuses one LSU block; two RUNTIME_ASSERTs) + eeae410bc
  (rider merge as AND-OR, comments, design docs; logic_gate not runnable: no CIRCT/foldsim on Phoenix or ICE) +
  6b394f724 (tests: lmem_lrsc_store, lmem_wr_merge cases 0x100/0x200/0x400 and any thread count, smem_rate -k22 mix,
  two more parity cases). Experiments commit is now only the tag sm-rate-experiments (3e587e453).
  Copies: src_smr_n10 = 6b394f724; src_smr_n9x = bd9927d95 + the LR/SC test (to show it fails before the fix);
  src_smr_d0 = pristine dd94ad142.
  Jobs (ICE): qra/qrb (rtlsim small shape + EXT_A, four configs -> smr_qr_*), qs (SimX same -> smr_qs_*), xr/xs (pre-fix
  LR/SC rtlsim/SimX -> smr_xr_*, smr_xs_*), y0 (pristine rtlsim identity -> smr_y0_*), zdef (default config
  lmem_wr_merge -> smr_z_def_*), l10/l10g (h200s SimX -> smr_l10*), ci10 (planner, 8 cases -> smr_ci10).
  Lint: 6b394f724 failed rtlsim lint with WR_MERGE=1 (sel_payload data bits unused after the AND-OR recode); fixed and
  squashed: series is now 27111bf5a, a0ce53f58, 5ca827aa3 (src_smr_n11). qr_off/qr_row (n10) differ from n11 only inside
  g_merge; qr2_both / qr2_mrg re-run from n11.
  Before the fix (src_smr_n9x): rtlsim WR_ROW lmem_lrsc_store fails (sc=0, word=3000+t on stored-to words), SimX passes;
  knobs off passes. After (n10): rtlsim row passes -w4 (1191 cyc) and -w16 -s5 (1307); SimX 1187 / 1302.
  PRE-EXISTING, not this branch: pristine dd94ad142 rtlsim, small shape + EXT_A, lmem_access fails on lrsc_word
  (expected +512, got +13 / +9; 988,010 cycles; "CP has not retired" warning); SimX passes. smr_y0_lacc on ICE.
- 11-23 review-fix results. Head 50cae5c52 (27111bf5a, a0ce53f58, 50cae5c52 on bd9927d95); src_smr_n12 = head
  (n11 + two tensor_wg cases lmem_rdport-race-wr-row, model_parity-lmem-rdport-starve-wr-row: 3 passed, gap 0.63%).
  h200s SimX knobs off (smr_l10_off_*, l10g) = pristine: fsq 10,018; warp_rate 113,382; roof 62,075; bw 452,761;
  war 342,988; lmem_rdport 488,417; gemm_block 576,342; decode 1,320,528. Knobs on: war 342,988, decode 1,320,528.
  rtlsim small shape + EXT_A knobs off: war 334,086, ldst 2,220 = pristine (y0); lmem_access fails the same way in
  pristine and every config (pre-existing lrsc_word).
  SimX vs rtlsim, small shape (25 points per config): off worst -0.73% (k20w16; war_order +2.88% is the known
  pre-existing one); mrg worst -0.76%; row worst -4.57% (lmem_wr_merge -w16 all cases: 6779 vs 7104; by case:
  old cases 0xff -1.74%, each new case alone <=0.26%); both worst -2.0% (same test). Store/load stream kinds <=0.73%.
  Fairness, smem_rate -k22 (every 4th warp sw x128, others lw x128), cyc/iter store warps / load warps:
    h200s SimX 64 warps: off 49,324 / 49,370; merge 32,968 / 32,963; row 26,534 / 41,201; both 10,471 / 26,848.
    small 16 warps rtlsim: off 9,126 / 12,455; mrg 8,324 / 8,330; row 6,291 / 10,388; both 2,728 / 6,806
    (SimX 8,327/12,434; 8,321/8,330; 6,287/10,388; 2,728/6,804).
  Loads alone (k14w64, h200s) take 16,524: with both knobs the loads end 16,377 after the stores do, i.e. they make
  about 1% progress while a block stores. Total time is shorter than off in every config, but the veto is real.
- 11-23, delta review: land with both knobs off. Lead merged 50cae5c52 locally as bd7aa26ba. Follow-ups on
  feature/sm-rate-f (on bd7aa26ba): a12c2963e (WM_XROW with the other row on the bank's lowest lane; byte-disjoint
  assertion in both models). Copies: src_smr_n13 = a12c2963e; src_smr_n13m = same with the SimX rider rule's
  same-word clause dropped (mutation: the test must fail). Jobs fr (rtlsim), fs (SimX), fm (mutation).

OPEN after the merge (not to do now):
  1. Turn bit for loads under LMEM_WR_ROW. rd_gate is a veto with no turn: while any block stores, the loads and
     atomics of all blocks make ~1% progress (smem_rate -k22, numbers above). Must be done BEFORE any profile sets
     the knobs.
  2. Read-side treatment of the local memory (loads are one block per engine per cycle; not touched here).
  3. XLEN 32: every knob-on run and CI case is XLEN 64 (bank word 8 B). Merge at 4-byte words is unmeasured.
  4. LSU-to-LMEM buffer: RTL has one entry (VX_lsu_mem_path.sv LOCAL_OUT_BUF(1)), the SimX channel into LocalMem
     holds two; SimX's LSU reports empty early under backlog at a scope-0 barrier. Knob-independent. It is the
     suspected cause of the lmem_wr_merge -w16 gaps (row -4.57%, both -2.0%); not traced for the new cases.
  5. PRE-EXISTING rtlsim failure on pristine dd94ad142 (also in every knob config): lmem_access with EXT_A at
     16 warps x 32 threads. LR/SC count word lmem[4241]: expected +512 per block, got +13 (block 0) and +9
     (block 1); 988,010 cycles, with a "[VXDRV] CP has not retired a command in 10s" warning. SimX passes.
     To reproduce (no profile, XLEN 64, build tree configured from a pristine copy):
       CONFIGS="-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=32 -DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_NUM_ALU_LANES=16 \
         -DVX_CFG_NUM_LSU_LANES=8 -DVX_CFG_LMEM_NUM_BANKS=16 -DVX_CFG_EXT_A_ENABLE" \
         ./ci/blackbox.sh --driver=rtlsim --app=lmem_access
       (same line with --driver=simx passes). The kernel's LR/SC loop is bounded (tests/regression/lmem_access/
       kernel.cpp ~200), so a store-conditional that keeps failing leaves the count short rather than hanging.
     Logs: ICE ~/scratch/vortex-ice/results/smr_y0_lacc (pristine), smr_qr_off_lacc.
  6. No knob-on perf_gate; logic depth of the knob-on paths unmeasured (no CIRCT/foldsim on Phoenix or ICE).
  Follow-up head is 7227ce092 (amended: SimX also asserts merged lanes are of one word, because SimX writes lane by
  lane and cannot fail functionally on a wrong rider rule). src_smr_n14 = 7227ce092; n14m = mutation in RTL + SimX.
  Results: rtlsim (fr_*, RTL of a12c2963e = 7227ce092) lmem_wr_merge -w16 off 9,617 / mrg 6,081 / both 5,279, all
  PASSED; SimX (gs_*) 9,585 / 6,091 / 5,199, row 6,759, all PASSED. Mutation: rtlsim fails functionally (bytes of
  word A's high half missing: "byte 1028: got 00, expected 02"), SimX stops on the new assertion.
- 11-23 NEW TASK (lead, user-approved): read side + fairness, branch feature/lmem-rd off a470ffa5c (lead's local head;
  h200s now sets WR_MERGE=1, WR_ROW=1 in local commit 88077548c). Design approved: knob VX_CFG_LMEM_RD_ROW (needs
  WR_ROW=1): each block's store engine becomes its block engine for all plain requests (loads on the banks' port B,
  responses to the block's register in the read engine's place); read engine B not built; engine A keeps atomics,
  with a turn bit (amo_last) against plain requests; one grant, a bank to one engine per cycle (ONE-ROW rule, sum 1.0);
  rotation as WR_ROW already has it (first asking engine after the last first taker always gets its banks ->
  bound NUM_BLOCKS issuing cycles). Lead asked the H200 agent for the mixed measurement; if stores and loads each
  keep 1.0, switch the grant to two bank sets (taken_rd / taken_wr): keep that a local change.
  Verified in code: warp -> LSU block is fixed: block = (wid % ISSUE_WIDTH) % NUM_LSU_BLOCKS (VX_gpu_pkg wid_to_isw,
  VX_lane_dispatch batches port i*BLOCK_SIZE+b -> block b; one VX_lsu_unit in VX_execute). A 32-thread op is 4
  packets of 8 lanes through ONE block.
  Engine change: pass_want now includes the response credit (stores unaffected); new output pass_rw.
  New tests: lmem_rw_order (in the regression list), smem_rate -f <pct> fairness bound for -k22.
  WIP commit on feature/lmem-rd; src_smr_r1; jobs rs (SimX off/both/rd), rr (rtlsim rd) -> smr_rs_*, smr_rr_*.
