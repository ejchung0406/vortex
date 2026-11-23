# dxa-rate: DXA engine at two lines per clock (running log)

Agent `dxa-rate`, started 2026-11-23. Branch `feature/dxa-rate` (worktree `vortex-exp/wt_dxarate`, base fd151ee95).
Goal (lead, user-approved "A2"): remove the DXA engine's three internal limits behind knobs, default = today.

## Design (sent to lead 11-23)
- (a) VX_CFG_DXA_GMEM_PORTS (existed SimX-only): RTL honours it. Socket port i (1..P-1) = 2-input priority arb
  (dcache first, DXA), UMMU lane per port. VX_dxa_core: P independent N/P:1 arbs over contiguous worker slices
  (= SimX TxArbiter structure; VX_stream_arb N:P is different: i%P with one shared group grant).
- (b) VX_CFG_DXA_LMEM_ROW: lmem arbiter grants RR lead + per other row part the first worker whose beat lies in that
  part (same core, no `last`). Part addresses ride the DMA attr; VX_lmem_unit uses them per bank for DMA writes.
  SimX: DxaCore beat arbiter; part channels sent together, unused part = byteen 0 placeholder (LocalMem ignores).
- (c) VX_CFG_DXA_LINE_PER_CYCLE: smem_wr pend refills in the cycle it moves to the fill buffer.

## Log
- 11-23: implemented (uncommitted WIP in wt_dxarate): RTL VX_dxa_core per-port slice arbs, VX_socket port i arb + UMMU
  lanes, VX_dxa_lmem_arb (new), VX_lmem_unit DMA_PARTS, smem_wr refill; SimX make_emission/commit_emission refactor,
  tick_lmem_row, capture rule, LocalMem ignores byteen-0 DMA writes. Test tests/regression/dxa_rowmix (byte check).
  Helpers: slurm/dxr/mksrc.sh (frozen copy src_dxr_<name>), slurm/dxr/lrun.sh (local SimX point), rover/dxr_lint.sh.
  Lint of h200s on the Phoenix login node is killed (memory); lint runs on rover (dxr_lint_h200s_{on,off}.log).
- First SimX h200s numbers, all knobs on + 16 workers (src_dxr_wip1, local): dx3 222,071 cyc = 56.7 B/clk (base 51.4),
  dx7d2 437,198 = 67.2 (base 53.5); both PASSED. Below the lead's 105: investigating with counters (src_dxr_st1,
  temporary DXR_STATS, not for commit).
- 11-23 counters (src_dxr_st1/st2, slurm/dxr/stats_patch.py): with contiguous port slices the second port carried
  little (dispatch fills low workers first) -> workers interleaved over ports (worker i -> port i % P) in RTL+SimX.
  dx7d2 437,198 -> 403,725 (72.7 B/clk); dx3 222,071 -> 228,316; e2k 470,460 -> 507,372 (more HOL blocking of
  responses at full worker holding registers; LMEM beats 65% paired in dx7d2).
- rtlsim small config (wip2): dxa_rowmix -x8 failed on RTL only: right-edge OOB columns read memory. PRE-EXISTING:
  VX_dxa_addr_gen checks outer dims only (is_oob uses dim_count[0..3] = dims 1..4); SimX fills cfill. Test changed to
  keep tiles inside the right edge. OPEN item for the lead.
- Commits: 86804ae61 (engine, 3 knobs), f234cc97f (dxa_rowmix + CI cases). rover src dxr_c2 = f234cc97f, dxr_base =
  fd151ee95. h200s lint is ~110 GB on rover: run one at a time.
- Knobs-off identity, h200s SimX (wip2 = same SimX code as c2): fsq 10,018; warp_rate 113,382; tmem_roof 62,075;
  bwstream 452,761; gemm_block 576,342; decode 1,320,528 = all identical (rover dxr_i2_*).
- 11-23 SimX-only experiments (src_dxr_st3 = 1fabd3d4a-ish + stats + DXR_PEND_DEPTH; results/dxr_x_experiments.txt),
  h200s, all knobs, cycles dx7d2 / e2k / dx3 (29.4 MB / 29.4 MB / 12.6 MB):
  2 ports W16 403,725 / 507,372 / 228,316; + 4-line worker holding 402,429 / 467,436 / 205,104;
  4 ports W16 330,327 (88.9 B/clk) / 507,774; 4 ports + holding 4: 332,585 / 483,673; 2 ports W32 403,725 / 449,152.
  Reading: dx7d2 (14 transfers) is bound at the ports: each of 2 ports passes ~0.56 line/clk while ~5 workers/clk wait
  at them, 4 ports reach 1.37 line/clk in all (0.3-0.4 each): the limit is behind the DXA's ports (socket port ->
  L2/DRAM), shared, near the plain-load stream's ~1.46 line/clk (ldstream word64 93.5 B/clk). LMEM is not saturated:
  beats in 69-78% of cycles, 65-75% of them two lines. e2k/dx3: worker holding (HOL blocking of a port's responses)
  and worker count matter. dx3: three issuing loops.
- rtlsim vs SimX small config knobs on (dxr_c2/c3): copy d1 6470/6466, d3 33053/33046, d5 23544/23540, mcast 3563/3559,
  bw 14932/14864, sgemm2_dxa 8,954,119/8,953,592, rowmix -x8 -r3 -g4 146,284/146,376; all PASSED both.
- 11-23 h200s SimX table (rover dxr_h_*, src dxr_c3; cycles, B/clk over the run; decode = B/clk in the multiply):
    config          dx3             dx7d2           e2k             decode
    base W8         246,415 51.1    545,019 53.9    591,441 49.6    51.38
    P2 W16          -               502,381 58.4    532,275 55.2    -
    P2+ROW W16      193,490 65.0    405,437 72.4    418,807 70.1    58.94
    P2+ROW W8       202,662 62.1    448,021 65.5    458,675 64.0    58.35
    all3 W16        228,316 55.1    403,725 72.7    507,372 57.9    59.04
    all3 W8         236,066 53.3    393,703 74.6    446,020 65.8    58.31
    all3 P4 W16     187,595 67.1    330,327 88.9    507,774 57.8    58.94
  FAILED PREDICTION: LINE_PER_CYCLE was to remove response blocking; with ROW it makes dx3 -15%, e2k -17% (memory side
  returns fewer lines/clk: e2k gsent 0.90 vs 1.09 line/clk; capblk 139K vs 269K, i.e. less blocking but slower).
  Cause not traced (hypothesis: request interleaving across streams changes DRAM locality). h200s enable = P2+ROW+W16,
  LINE_PER_CYCLE left 0.
  Plain loads: bwstream -n2048 -u4 -b32 452,761 -> 416,774 with GMEM_PORTS=2 alone (SimX port-1 arbiter adds request
  slots in front of dcache port 1); ldstream 244,316 -> 243,988; ROW+LPC alone: bwstream 452,761 (unchanged).
  RTL port-i arbiter made unregistered (REQ/RSP_OUT_BUF 0) to match SimX's 2-input arbiter (no delay). Final commits:
  62ec83be6 engine, a6441d504 tests (rover src dxr_c4).
- Knobs-off CI DXA cases (default config, XLEN 64) base fd151ee95 vs c3, both drivers: identical (copy d1/d3/d5, mcast,
  kmajor, bwstream_dxa, sgemm2_dxa). rowmix -x8 -r16 -g8 knobs on: rtlsim 1,553,041 vs SimX 1,554,922.
- 11-23 final (src dxr_c4 = a6441d504; profile commit 319a8c4e3 on top, SimX-identical to c3 measurements):
  small config knobs on, rtlsim vs SimX: rowmix -x8 -r3 -g4 146,288/146,376; -x8 -r16 -g8 1,553,203/1,554,922;
  bwstream_dxa 14,932/14,864; copy -d3 32,957/33,046; mcast 3,563/3,559; sgemm2_dxa 8,951,689/8,953,592;
  vecadd -n16384 380,940/381,439 (dcache port 1 shared with the DXA); kmajor PASSED both.
  h200s rtlsim, all three knobs + 16 workers, bwstream_dxa -e4096 -n32 -d2 -c0: rtlsim 58,091 vs SimX 54,238 (+7.1%),
  both PASSED; build 49 min, 79 GB. Knob-off reference on the same shape running (dxr_hr4_off_dx7s / dxr_hs_off_dx7s).
  Enabled profile, other counts: gemm_block 576,342 -> 548,222; fsq 10,019; warp_rate 113,383; roof 62,078;
  bwstream 416,774; ldstream 243,988.
- h200s parity, bwstream_dxa -e4096 -n32 -d2 -c0 (c4): knobs off rtlsim 79,547 vs SimX 70,681 (+12.5%, pre-existing gap);
  all three knobs + 16 workers rtlsim 58,091 vs SimX 54,238 (+7.1%). RTL gain off->on -27%, SimX -23%. Both PASSED.
- STATE at end: branch feature/dxa-rate = 62ec83be6, a6441d504, 319a8c4e3 (head). Not pushed, not merged. No rover point
  of mine running. Frozen copies src_dxr_{wip1,wip2,st1,st2,st3} (experiments; st* carry temporary DXR_STATS).
- 11-23 follow-ups (lead): head 636d15961 = 1d4f28856 engine (+ SimX RegMerge/RegSplit for shared socket ports),
  3a6a62ddb dxa_rowmix, f124ca7d0 dxa_corun (new test; bwstream_dxa untouched), 636d15961 h200s enable.
  1. SimX shared port now transparent: RegMerge/RegSplit (sim/simx/regslice.h) replace the port's breq/brsp slices,
     input/output 0 = exactly the RegSlice. h200s ports=2: bwstream 452,761 (= base), ldstream 244,316 (= base),
     fsq/wr/roof = base. Small config ports=2, vecadd -n16384: rtlsim 380,940, SimX 380,927.
     DXA numbers moved slightly (enabled P2+ROW+W16): dx3 194,155 (64.8), dx7d2 407,209 (72.1), e2k 452,229 (64.9),
     decode 58.92; gemm_block 549,024. With LINE_PER_CYCLE: 207,845 (60.5) / 407,318 (72.1) / 499,306 (58.8).
     Small-config knob-on parity rtlsim/SimX: rowmix 146,288/146,393, 1,553,203/1,553,913; bw 14,932/14,864;
     copy -d3 32,957/33,040; mcast 3,563/3,559; sgemm2_dxa 8,951,689/8,951,571; dxa_corun 55,059/55,047.
  2a. Single transfer (bwstream_dxa -e4096 -n64 -d1 -g1 -c0): 100,123 cycles = 10.47 B/clk in base, enabled and
     ports=4 alike (transfer span 1,144.9 cycles for 256 lines); with LINE_PER_CYCLE 90,838 (11.5, span 999.8).
     OPEN: the part streams one transfer at 1.67 line/clk; here one transfer = one worker <= 0.5 line/clk (1 with
     LINE_PER_CYCLE). What would close it: a worker assembling two lines of one row per beat (option B), or a transfer
     split over two or more workers (dispatch by line ranges), together with a capture rate of >= 1 line/clk.
  2b. Co-run (dxa_corun -e1024 -n64 -d2 -x8 -L20 -s1024 -c0, h200s SimX, VX_DBG_STALL_TIMEOUT raised; with the default
     200,000 a copy warp's issue timeout fires): copy alone span 47,426 (44.2 B/clk), loads alone 338,253 (7.75);
     together copy 384,963 (5.45), loads 340,402 (7.70) - IDENTICAL with knobs off and with ports=2+row, and ports=4
     gives 384,345/340,110. So in this co-run the DXA port priority does not bind: the copy CTAs' warps (one shared load
     per tile, barriers) wait behind the load warps in the core, and the copy ends after the loads. The dcache-first
     priority itself CAN hold a DXA port back for as long as the dcache requests every cycle (port 0 already could:
     the DXA is last there); not shown by a run.
  3. h200s rtlsim model builds (rover): knobs on (fd151ee95 tree + profile WR_MERGE/WR_ROW + my knobs, no RD_ROW)
     peak 79 GB, 49 min; knobs off peak 77 GB, 44 min.
  OPTION for the user (not enabled): DXA_GMEM_PORTS=4 (ROW, 16 workers): dx3 250,199 (50.3), dx7d2 356,635 (82.3),
     e2k 506,055 (58.0), decode 59.01, one transfer 10.47; vs ports=2 64.8 / 72.1 / 64.9 / 58.92. Only 7xD2 gains;
     dx3 loses 22%. Cost: two more socket-port 2:1 arbiters (ports 2-3 shared with dcache banks 2-3), two more
     unit-MMU lanes (TLB lookup each), two more per-port worker arbiters.
