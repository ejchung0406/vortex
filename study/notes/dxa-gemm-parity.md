# dxa-gemm-parity: remaining SimX/RTL gap on the h200s DXA copy and gemm_block (running log)

Agent `parity-dxa`, started 2026-11-25. Branch `fix/dxa-gemm-parity` from 553ec0818, worktree `vortex-exp/pdx_wt`.
Everything this agent creates carries the prefix `pdx_` (scratch `/storage/scratch1/7/echung67/pdx/`, slurm/pdx/).
Gap at start (rtlsim / SimX): copy 48,702 / 45,248; gemm_block 472,826 / 443,681; smem_rate k22 140,238 / 139,931.

## 2026-11-25 · Re-reading the previous reduced trace (w8 + VM + 256 slots, 32 tiles)
- RTL keeps fewer DXA lines in flight than SimX (mean 944 vs 1,102 over both ports) while its per-line memory
  latency is shorter (issue to unit-MMU response p50 722 vs 769): the RTL issues more slowly, not the memory.
- Turnaround of a warp (last capture of tile k to admission of tile k+2) agrees: mean 433 vs 414, median 394 vs 391.
- With exactly one worker pending on a port, RTL issues in 61% of the cycles, SimX in 88%; RTL has 761 one-cycle
  and 204 three-cycle no-issue holes in that state vs 189 and 6. RTL's per-warp finish times spread 38.0K-43.6K, SimX
  39.2K-40.2K. So the request side between a worker and the unit MMU stalls more often in RTL.
- Reading the unit MMU (RTL VX_mmu PARK_REG=1, WRITE_ENABLE=0, RSP_OUT_BUF=2): every request on a page with a live
  miss-station entry joins it through the one-entry park stage (one park per two cycles for all lanes) and the
  page's lanes hold while it is staged; a re-armed entry can keep this going. SimX models join and re-arm
  (mmu.cpp stage_drain / step 2b). Next: trace the unit-MMU lanes per cycle in both models.

## 2026-11-25 04:40 · Unit-MMU lane trace; the reduced RTL reference moves between builds
- Source `pdx/src_trace1` = 553ec0818 + the previous PTRACE diff + a per-cycle trace of the unit-MMU lanes (`PX`
  lines: valid, accept, hit, join, hold, park, park taken, replay, out ready/blocked). Shape w8 + VM + 256 slots,
  overlap + LPC, `bwstream_dxa -e4096 -n32 -d2 -c0`. Phoenix 16126633 (rtlsim) / 16126634 (simx).
- The unit MMU is not the stage: parks are 5-7 per lane in RTL and 6 in SimX over 57,344 lines; a lane waits on its
  downstream (out stage not ready) in 8.2K/7.4K RTL vs 9.8K/10.2K SimX cycles. Hypothesis "the RTL park chain caps a
  page at one line per two cycles" FAILED (the chain never forms here).
- The same configuration and the same RTL (7966e9661 and 553ec0818 differ only in profiles/h200s.json; my change adds
  only `$display`s) gives 42,732 RTL cycles in this build against 47,783 in the previous trace build, same 9,394
  retired. SimX is 44,734 in both. The sign of the reduced gap flips (now SimX 4.7% slower). The rtlsim model uses
  `randReset(2)` with seed 50: every register without a reset starts at a pseudo-random value, and any change to the
  generated model (here a `$display`) can redraw them. Next: same build, seeds 1-3 and zero-init (PDX_RTL_SEED /
  PDX_RTL_RESET env added to processor.cpp of the frozen copy), Phoenix 16127063.

## 2026-11-25 05:00 · The 47,783 reduced RTL reference ran with a different buffer placement
- Seeds 1-3 and zero-init on one build (16127063) all give 42,732: no register without reset moves this run.
- The two RTL traces differ from the first line on: the source lines are at VA 0x2b0000.. in the old build and
  0x102b0000.. in mine (SimX: 0x101b0000.. for warp 3, the same base). Under VM the runtime reserves a 256 MB pinned
  slab at the bottom of the heap (device.cpp resolve_pinned_size, VX_CFG_VM_PINNED_REGION_SIZE), so a libvortex.so
  compiled without the VM CONFIGS places every buffer 256 MB lower. The old RTL build.log shows no libvortex.so rebuild
  in the app step (the SimX build of the same job and both of mine do): that RTL ran a stale, non-VM runtime. With the
  same placement the pair is 42,732 RTL / 44,734 SimX (SimX 4.7% slower, not 6.4% faster).
- So every earlier reduced VM pair (hbm_w8_vm 46,520, w8 VM + 256 slots 47,783/27,335) compared different physical
  layouts; the "VM adds 3 points" step is not established. The non-VM reduced pairs do not depend on the slab.
  The h200s references fold VM into the profile at configure time, so their default runtime already has the slab
  (both h200s build logs carry the same profile CONFIGS); still to be confirmed from addresses.
- Reduced shapes now all put SimX slower than RTL (hbm_w8 45,317/43,659; w8vm256 44,734/42,732) while h200s puts SimX
  7% faster. Ablation from w8vm256 toward h200s submitted (each RTL + SimX, PTRACE+PX trace build):
  +L2 (33 ways, 8.25 MB, MSHR 256), +TEX/TFU (adds a unit-MMU lane and a socket-port input), +TCU/TMEM/TCU row port,
  +dcache/LSU sizes, and h8 = the full resolved h200s CONFIGS with 8 warps.

## 2026-11-25 06:10 · Placement race confirmed; DRAM streams match; ablation results so far
- Rebuilding the old trace source exactly (src_oldtrace, 16128021) gives 42,732 with the VM runtime; my source with
  only its `$display`s removed (16128022) gives 47,783 with a stale runtime (no `libvortex.so.tmp` in build.log, source
  VA 0x2b0000). So the 47,783 is the stale runtime, not the RTL. It is a race in the build: `make -C sw/runtime/stub`
  without CONFIGS followed seconds later by the app make with CONFIGS sometimes leaves the old libvortex.so (both
  RTL and SimX builds were hit: w8l2/w8tex/w8dc RTL and w8l2/w8tex SimX in the first ablation round). My sbatch now
  deletes libvortex.so and stub_config.stamp before the app make; every result is checked by the first ISSUE address.
- DRAM request trace (sim/common/dram_sim.cpp send_request, env PDX_DRAM_TRACE, same code in both models), w8vm256 at
  the VM placement: the 57,356 read (channel, address) pairs are the same multiset in RTL and SimX; RTL spans 45,086
  DRAM ticks, SimX 46,751. Address mapping is not a source of difference.
- SimX at the VM placement, 32-tile copy: w8vm256 44,734; +L2 (33 ways, 8.25 MB, MSHR 256) 45,248; +TEX/TFU 44,734;
  +TCU/TMEM 44,734; +dcache/LSU sizes 45,484; h8 (all h200s CONFIGS, 8 warps) 45,248 = the h200s SimX value.
  RTL singles and h8 queued (Phoenix priority).

## 2026-11-25 08:05 · The h200s direction appears with the dcache/LSU sizes; warps stall 13K cycles in RTL
- RTL at the VM placement (32-tile copy, SimX in brackets): w8vm256 42,732 (44,734); +TEX/TFU 42,732 (44,734);
  +L2 33 ways/8.25 MB/MSHR 256 45,146 (45,248, 0.2%); +dcache 256 KB, dcache MSHR 512, LSU pending 512 48,074
  (45,484: RTL 5.4% slower, the h200s sign). +TCU/TMEM still building.
- In that w8dc RTL run the transfers themselves are shorter than SimX's (span mean 1,793 vs 2,174), but some warps
  stop between transfers: warp 6 waits 13,017 cycles after tile 4 (t0+7,049 to t0+20,066), warp 5 6,646 after tile
  11, warp 0 2,662 after tile 15, and all three resume within 500 cycles of t0+20,066. Turnaround median 397 (SimX
  395) but mean 610 (SimX 430). Without the dcache/LSU sizes no turnaround exceeds ~800 in either model.
- Splitting the three settings (dcache size / dcache MSHR 512 / LSU pending 512), Phoenix 16132343-48.

## 2026-11-25 09:00 · The ingredient is DCACHE_MSHR_SIZE=512
- Single settings on w8vm256, RTL / SimX, 32-tile copy at the VM placement: dcache 256 KB 43,856 / 45,631; dcache
  MSHR 512 **54,470 / 44,744 (RTL 17.9% slower)**; LSU pending 512 42,732 / 44,734. SimX hardly moves with the MSHR
  (+10), RTL gains 11,738 cycles.
- MSHR 512 RTL turnarounds: warp 2 stops 27,195 cycles after tile 2 (t0+3,740 to t0+30,935), warp 5 12,892 after
  tile 15; both resume within 500 cycles of t0+30,450. All other turnarounds are below 660.
- Next (submitted): the same shape with a barrier-drain hold trace in VX_wctl_unit (cycles an arrive/barrier waits,
  and of those how many with an LSU block busy and with the socket MMUs busy), plus DEBUG=1 rtlsim and SimX runs
  for the instruction-level picture.

## 2026-11-25 09:25 · The MSHR-512 cost is a warp that stops after its loop branch until an unrelated barrier unlock
- A drain-hold trace in VX_wctl_unit (src_trace2) shows no long barrier drain: 345 holds, the longest 56 cycles.
  Hypothesis "BAR.EXPECT waits for the core-wide LSU/MMU drain, which SimX does not model" FAILED for this stall
  (SimX's lsu_drained() indeed lacks the RTL's mmu_empty term, VX_wctl_unit.sv:190 vs sfu_unit.cpp:86, but it holds
  nothing here).
- A per-warp commit trace (src_trace3, `PCM`) on the MSHR-512 shape: RTL warp 2 commits the loop's back branch
  (PC ...238) at 7,047 and its next instruction (the loop head ...1a0) at 33,785, a 26,738-cycle gap; warp 5 the
  same, 12,523 cycles, ending at 33,786. At 33,770 warp 4 commits its BAR (...224, SFU); 15 cycles later both
  stuck warps go on. Every other gap in RTL and every gap in SimX is under 2,500 cycles (the BAR wait at ...224).
- So a runnable warp is left unschedulable after its branch resolves and is woken only by a barrier unlock that does
  not concern it. That looks like a lost wake-up in the RTL front end (VX_warp_sched stall/unstall or fetch-ahead
  credit), with the dcache MSHR size only moving the timing onto the race. Tracing VX_warp_sched state per warp
  (src_trace4, `PW`: stalled, ibuf, in flight, drain, branch hit, squash/drop, barrier unlock), Phoenix 16134846.

## 2026-11-25 10:30 · Not a lost wake-up: one instruction-cache miss takes 26,740 cycles
- Correction to the 09:25 reading: the warp-scheduler trace (src_trace4, `PW`) shows warp 2 unstalled by its branch at
  7,045 (stalled 1->0) with one fetch still in flight and its squash drain set, so it cannot be picked; no barrier
  unlock is involved. The fetch in flight is a fetch-ahead past the loop branch into the next 64-byte code line
  (PC ...240), which the loop never executes.
- Instruction-cache path trace (src_trace5, `PI`, at the socket): the icache sends the miss for line 0x6000009
  (PA 0x180000240) to the socket arbiter at 7,018 (accepted at once) and its response comes back at 33,758. Warp 5's
  fetch-ahead into the same line merges into that miss; both warps go on at 33,785/33,786. The DXA streams ~0.7 lines
  per cycle on each port the whole time. Tracing that line inside the L2 (core request, memory request/response, core
  response; src_trace6, `PL`), Phoenix 16136649.

## 2026-11-25 11:15 · Same stall in the full h200s RTL; the L2 holds a hit
- L2 line trace (src_trace6): the request for line 0x6000009 enters the L2 on input 0 at 7,164 and the L2 answers it at
  33,643 without any memory request for that line (the line was already in the L2: a request from input 5 hit it at
  1,446 and was answered at 1,525). So an L2 hit is held inside the L2 for 26,479 cycles. Bank in/out trace queued.
- Full h200s, traced rtlsim on rover (src pdx_trace4 = commit + warp-scheduler trace, build 39 min, 82 GB; reproduces
  48,702 / 9,394 retired; SimX 45,248): warps 6 and 1 stop after the loop branch (...238) for 14,845 and 13,609 cycles,
  warp 0 for 2,139, all going on at 21,719-21,722; warp 6 has its branch resolved at 6,872 with one fetch in flight
  and its squash drain set, exactly as in the reduced shape. No SimX gap exceeds 2,576. The 3,454-cycle h200s copy
  gap is this stall.
- SimX descriptor-fetch stage (ADMIT->ALLOC 2 cycles and one dispatch per two cycles, as VX_dxa_core.sv's fetch FSM;
  worktree change, src_fix1): h200s copy 45,248 -> 45,379, gemm 443,681 -> 438,325, k22 unchanged. Real structural
  match, but it moves gemm away from RTL.

## 2026-11-25 12:15 · Inside the L2: a lazy refetch waits behind memory fills
- Bank in/out (src_trace7): the request enters L2 bank 4 at 7,165 and the bank answers it at 33,621.
- Bank 4 internals for the line (src_trace8, `PK`): the kernel upload (CP, L2 input 5) wrote the line at 1,447 as a
  write-miss allocation without fetch (lazy fetch), leaving it partial. The icache read at 4,371 found it partial
  (tag match, partial miss), allocated MSHR 61, and was served at 4,381 by a data-less refetch loop. The read at
  7,166 finds it partial again, allocates MSHR 26, and its fill (id 26) arrives at 33,608, then replays and hits.
- DRAM trace with completion times (src_trace9): no DRAM request for that line in the whole run, and the longest DRAM
  read takes 681 ticks. So the fill for id 26 is the data-less refetch loop (lazy queue), and that queue waits
  26,442 cycles in bank 4. Both models serve the lazy queue only in a cycle with no memory response at the bank
  (RTL VX_cache_bank.sv:392 `lazy_sel = ~mem_rsp_valid && lazyq_valid`; SimX cache.cpp:1296 `else if
  (!lazy_q_->empty() ...)` after the mem_rsp_in branch). Windowed bank counters next (src_trace10, `PQ`, both
  models): how often bank 4 has a memory response waiting.
- 12:05: my kept RTL build trees filled the scratch quota; deleted (only pdx_ paths); builds now in node-local /tmp.

## 2026-11-25 12:30 · Cause 1: SimX's L2 does not hold a forwarded fill on a full response queue (L2_FWD_HOLD=0)
- Bank counters (src_trace10, `PQ`, per 1,000 cycles), MSHR-512 shape: RTL L2 bank 4 has a memory response waiting
  in 999/1000 cycles from ~5K to ~37K, accepts ~90 fills per 1,000, and spends ~906/1000 cycles with `fwd_pending`
  (a forwarded fill's chain head waiting); bank 7 the same. SimX bank 4 has a response waiting in 5-40% of cycles.
  So the RTL bank's fill path is held ~10 cycles per DXA line, the responses queue behind it, and the lazy queue
  (served only with no response waiting) starves: 26K cycles for the icache's partial-line refetch.
- RTL: VX_cache_bank.sv:1366-1371 `fwd_head` claims the chain head without `crsp_queue_ready` and `fwd_pending`
  (fwd_active && replay_valid) keeps fills out (`fill_mux ... && ~fwd_pending`, :416; `mem_rsp_ready`, :435) while
  the 2-entry core-response queue (L2_CRSQ_SIZE 0) is full: 16 banks share the 2 socket response ports.
- SimX: cache.cpp:1736-1740 holds the head only with `config_.fwd_hold`; the L2's comes from VX_CFG_L2_FWD_HOLD
  (cluster.cpp:118), default 0 ("SimX only ... 0 keeps the model's replay", VX_config.toml:466-469, commit
  462002754 "Not confirmed against rtlsim"). profiles/h200s.json sets DCACHE_FWD_HOLD=1 but not L2_FWD_HOLD, so
  SimX's L2 replays where the RTL's holds. Testing SimX with -DVX_CFG_L2_FWD_HOLD=1 (Phoenix 16141001-04).

## 2026-11-25 12:50 · L2_FWD_HOLD=1 is not the fix; gemm's excess is at the barrier after the MMA
- SimX with -DVX_CFG_L2_FWD_HOLD=1: h200s copy 45,248 -> 40,034 (further from RTL 48,702), gemm 443,681 -> 443,514,
  k22 unchanged; w8vm256 44,734 -> 38,812. Hypothesis "turning on SimX's L2 forward hold reproduces the RTL" FAILED:
  SimX already keeps fills out while a forward is active (cache.cpp:1229, 1296 `!fwd_active_`), and with the hold its
  L2 streams faster, not slower. The RTL's starvation needs its own response-queue timing (2-entry CRSQ draining
  into a 16-bank -> 2-port crossbar), which the SimX bank does not have cycle for cycle.
- Diagnostic RTL (scratch copy only, src_diag1: lazy fill ahead of memory fills in VX_cache_bank.sv:392/435/453) to
  confirm the copy mechanism: w8dmshr and h8 queued.
- h8 (all h200s settings, 8 warps) reproduces the h200s copy exactly in both models: RTL 48,702, SimX 45,248.
  h8 gemm -n128 (2 CTAs): RTL 139,968, SimX 140,127 (0.1%).
- h200s gemm, commit trace (src pdx_trace4, rover; 472,826 RTL / 443,681 SimX, same counts as the references):
  chunk period per CTA 3,547 vs 3,342 (+205). From the release of the chunk's first barrier (PC ...230), the loads
  and the MMA commit 10-42 cycles later in RTL, but the second barrier (...2dc, sync, after the MMA) is released 179
  cycles (median 130) after the CTA's slowest warp commits its last MMA uop, against 45 (median 42) in SimX. That
  barrier, not the operand path, holds ~+134 of the +205 per chunk. RTL drain-hold trace (`PB`) in the same run:
  821 held stall-barriers, mean 168 cycles, 170 over 200; 1,587 held arrives (BAR.EXPECT), mean 125. Measuring the
  same holds in SimX (src_trace11, Phoenix 16141641).

## 2026-11-25 13:10 · The starvation is the whole copy stall; without it the RTL is faster than SimX
- Diagnostic RTL (scratch copy src_diag1 only; VX_cache_bank lazy fill served ahead of memory fills): MSHR-512 copy
  54,470 -> 41,665 RTL cycles, PASSED, no commit gap above 1,934 (SimX 44,744). So the 12K-cycle excess of that shape
  is entirely the lazy-refetch starvation. h8 (= h200s copy) with the same diagnostic queued.
- Correction to the 12:30 heading: L2_FWD_HOLD is a real structural difference (RTL VX_cache_bank always holds the
  forward head; SimX holds only with the knob, default 0, not set by h200s), but it is not what starves the lazy
  queue. With the hold SimX lets core requests keep entering the bank while the forward waits (as the RTL's
  `creq_grant` does), which is why SimX streams faster with it (h200s copy 40,034; w8vm256 38,812; w8dmshr 38,635),
  overshooting the starvation-free RTL (w8vm256 42,732, w8dmshr-diag 41,665).

## 2026-11-25 13:25 · gemm: the last warp's barrier takes 324 cycles after its MMA in RTL, 151 in SimX
- SimX drain holds (src_trace11/12, `PB2`, h200s gemm): stall barriers held 970 times, mean 284 cycles (LSU sent /
  stores landed core-wide); arrives (BAR.EXPECT) 494 times, mean 41. RTL (h4 run, `PB`): stall barriers 821 x 168,
  arrives 1,587 x 125. So SimX's barrier drain is not shorter overall; the RTL arrive holds are longer (RTL arrive
  drain includes `mmu_empty` = dMMU and iMMU empty, VX_wctl_unit.sv:189-195; SimX's has no MMU term,
  sfu_unit.cpp:86-93 -> lsu_drained()).
- Per chunk and CTA, from the slowest warp's last MMA uop commit to that warp's barrier (...2dc) commit: RTL 274 (med
  204), SimX 131 (med 110); release after it 25 vs 27. For the last-arriving warp: loop branch (...2d8) resolves with
  the MMA; the barrier is decoded ~9 cycles later (warp-scheduler trace) and commits 324 (med 260) cycles after the
  branch in RTL vs 151 (med 127) in SimX. In the sampled RTL case (warp 5, t=202,244-202,532) there is no drain hold
  at all, so the time goes between decode and the wctl unit or in the shared-unit handshake. Instrumenting the wctl
  execute stage (`PBX`: cycles a BAR waits at execute and why: response buffer, shared-unit ready, drain, state) in a
  second h200s build (rover pdx_h12b_gemm or Phoenix 16143128, whichever starts first; the other is cancelled).
- Rover's build slot was taken again by the same hand-held `flock ... cat` session while my first restart waited.

## 2026-11-25 14:15 · Cause 2 (gemm, partial): SimX's barrier-arrive drain ignores the MMUs
- RTL VX_wctl_unit.sv:189 `lsu_core_drained = (& lsu_sched_empty) && mmu_empty`, used by every arrive
  (BAR.EXPECT/vx_barrier_arrive) and, at scope 0, by every barrier (:195); `mmu_empty` is the socket's dMMU and iMMU
  `empty` (VX_core.sv:170, VX_socket.sv:420), which counts a lane's incoming and output stages (VX_mmu.sv:662-669).
  SimX sfu_unit.cpp:92 used core_->lsu_drained() (LSU queues and pending reads only, lsu_unit.cpp:214).
- Change (worktree): Mmu::drained() = empty() and no request in a lane's input or output channel; Core::
  lsu_core_drained() = lsu_drained() and, under VM, both core MMUs drained; the arrive/scope-0 drain uses it.
  h200s SimX: gemm 443,681 -> 451,686 (gap to RTL 472,826: 6.16% -> 4.47%), copy 45,248 -> 45,272, k22 unchanged.
  With the descriptor-fetch stage as well: gemm 451,469, copy 44,736.
- Disk: the scratch inode quota (1M files for the user) was full again at ~13:55; my copies are now lean (no study/,
  docs/), ~2.9K files.

## 2026-11-25 15:20 · Drain change: baselines, copy holds, a refinement that failed
- h200s SimX baseline set with the drain change (pdx_hn_fix2; base = hn_* on rover): fsq, wr, roof, bw, war, ldl, ldp,
  k14, k13, k22, lma identical; dx32 45,248 -> 45,272; dx7 337,648 -> 334,714; dx3 157,887 -> 161,250; dec 1,150,323 ->
  1,154,406; gb 443,681 -> 451,686. With the descriptor-fetch stage too (pdx_hn_fix3): dx32 44,736, dx7 337,805, dx3
  158,461, dec 1,155,630, gb 451,469.
- Arrive holds, h200s copy: RTL 127 x 8.3 cycles; SimX 62 x 16.0 before, 103 x 10.1 after. gemm: RTL 1,587 x 125;
  SimX 494 x 41 before, 1,598 x 39 after (count now matches, length still short).
- Counting only an MMU lane's input (not its output channel) changes nothing (identical cycles everywhere): the
  effect comes from requests the MMU has passed on but its downstream has not taken, which SimX's bound output channel
  reports as the endpoint's occupancy (simobject.h:1038). That is the nearest SimX quantity to the RTL out_pipe's
  valid; it is an approximation.
- On w8vm256 (no starvation; SimX was 4.7% slower than RTL) the change moves SimX 44,734 -> 45,736: that shape's SimX
  pessimism is the L2 fill/forward path, which this change does not touch.
- Descriptor-fetch stage set aside (diff in slurm/pdx/pdx_fix1_desc_fetch.diff): a true RTL structure but moves h200s
  gemm away (443,681 -> 438,325 alone) and is not a cause of either gap.

## 2026-11-25 16:15 · Commit e19c1f0e2; vm.yaml parity apps
- Committed on fix/dxa-gemm-parity: e19c1f0e2 "simx: a barrier arrive waits for the core's MMUs as well as its LSU"
  (sim/simx/core.{h,cpp} lsu_core_drained, sim/simx/mem/mmu.{h,cpp} drained, sim/simx/sfu_unit.cpp:92).
- vm.yaml model_parity apps at XLEN 64 (no rv32 toolchain in vortex-tools), SimX before/after: sgemm 870,463,
  vecadd 409,540, vecadd-wide 12,490, rt_raycast 168,955, gfx_tex 154,564, gfx_raster 63,639, gfx_om 881,217,
  bar_vis 32,117 all identical; dxa_copy 36,165 -> 36,195. RTL leg not rerun (RTL unchanged).
- No model_parity case for it: no small configuration found where the difference exceeds 5%.

## 2026-11-25 16:50 · gemm remainder: the post-MMA barrier waits ~400 cycles between decode and the SFU in RTL
- Second h200s RTL build (Phoenix 16143128, src_trace12: commit, warp-scheduler, drain-component `PB2` and wctl
  execute-stage `PBX` traces; the rover attempt was cancelled while still waiting for the build slot).
- `PBX` (cycles a BAR waits at the wctl execute stage, and why): sync barriers mean 6.2 (all drain), arrives 114.9
  (all drain); response buffer, shared-unit ready and handshake state never hold. So the RTL's slow post-MMA barrier
  is not in the drain or the shared barrier unit.
- For the barrier after the MMA (...2dc): branch resolve -> barrier decode 9 cycles; decode -> arrival at the wctl
  execute stage mean 418 (median 352); execute -> commit ~6. SimX (src_trace13, `PBXS`): branch/MMA commit -> barrier at
  the SFU mean 291 (median 254), SFU -> commit 5.5. The remaining ~120 cycles per chunk are in the issue/dispatch path
  (ibuffer, scoreboard, operand collection, the slice's issue lock while other warps' 32-uop TCU ops dispatch, SFU
  dispatch queue). Locating it needs a per-stage issue trace of the RTL at 64 warps; both h200s builds of this task are
  used, and the 8-warp shape (2 CTAs) has no gemm gap (139,968 / 140,127).

## 2026-11-25 17:30 · State at hand-off
| h200s, one SM (rtlsim / SimX) | before | after e19c1f0e2 | gap after |
|---|---:|---:|---:|
| bwstream_dxa -e4096 -n32 -d2 -c0 | 48,702 / 45,248 | 48,702 / 45,272 | 7.0% (RTL L2 starvation) |
| gemm_block -m16 -n1024 -k4096 -s1 | 472,826 / 443,681 | 472,826 / 451,686 | 4.5% |
| smem_rate -k22 -w64 -i16 -f150 | 140,238 / 139,931 | 140,238 / 139,931 | 0.2% |
- RTL arrive holds in gemm (partial PB2, 416 arrives): mean 162 cycles, MMU busy in 154 of them, LSU not empty in 147.
- The 8-warp diagnostic of the lazy-first RTL (Phoenix 16144101) and the second h200s gemm RTL (16143128) hit their time
  limits (builds of ~3 h on their nodes); 16143128's traces up to the limit are in results/pdx_t16b_h200s_rtlsim_gemm.
- Cleaned: every scratch source copy and build tree of mine (scratch/pdx keeps only *.flags). Rover keeps
  src/pdx_trace1, pdx_trace4, pdx_trace12 and results/pdx_h*; worktree vortex-exp/pdx_wt (branch fix/dxa-gemm-parity).
- Open: (1) RTL VX_cache_bank lazy queue starves behind memory fills (RTL bug, not changed); (2) without it SimX is 5-7%
  slower than RTL on the reduced copies: the L2 bank's forward/fill path under a full response queue (L2_FWD_HOLD 0 too
  slow, 1 too fast); (3) gemm: post-MMA barrier waits ~120 cycles longer in RTL between decode and the SFU;
  (4) descriptor-fetch stage (diff kept); (5) build race leaving a non-VM libvortex.so (1-second mtimes on scratch).
