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
