# Open threads (as of 2026-11-20, early morning)

Everything in flight, so it can be picked up again. "Decide" means the user
has to confirm before it is written up or acted on.

## A. Memory model moved into the main tree — NOT committed yet
- Working tree of `vortex` (branch `feature/dxa-borrowing`) has, uncommitted:
  `HBM3E` type, `VX_CFG_PLATFORM_MEMORY_{BANK_GROUPS,SCHED_WINDOW,INTERLEAVE_SIZE}`,
  shared `DramSim::Config::locate()` (SimX + rtlsim), windowed Ramulator
  controller built by `third_party/Makefile`, profile `h200s`, `bwstream -b`,
  new test `tests/regression/gemm_block/`.
- Check runs from the main tree matched the experimental copy to the cycle
  (`mt_bw_b32_k22` 708 GB/s, `mt_gemm_k1` 657,166, `mt_tma_k1_*`).
- Waiting on: `mt_h200q_bw_k2` (16,680) vs `mt_h200q_bw_k2_old` and
  `par3_simx_plain` (3,964) vs `par3_simx_plain_old` — do existing profiles and
  the default behave exactly as before? Then commit.
- Before committing: add to `h200s` the DXA engine count and
  `VX_CFG_DXA_MAX_INFLIGHT` (thread D), run `tests/unittest/dram_sim` and
  `dram_bank_model`, run the repo's review skills (AGENTS.md).
- `JESD238B.01.pdf` sits in the repo root, excluded via `.git/info/exclude`;
  never commit it.
- Experimental copies under `vortex-exp/`: `src_gran`, `src_rq` (EXP_* macros,
  EXP_RQ env), `src_visit5`, `src_rtlfix` (RTL dispatch change), `src_olddsp`
  (old SimX dispatch). Delete once the main tree is committed and verified.

## B. Decode GEMM kernels
- `gemm_block` passes in load and DXA mode. 1 SM on the target memory:
  decode 25.3, load 16.0-17.1, DXA 25.0 GB/s per SM.
- Running: `gb_*_k11`, `gb_*_k22` (five variants each). Question they answer:
  does kernel time per SM hold from 1 to 22 SMs (old `gemm_decode`: +2.4% at
  11, +27.6% at 22 on the target memory).
- Existing upstream GEMM tests do not run the decode shape (M=16 rejected, or
  30x slower). Decide: keep `gemm_block` as the study workload; wgmma later?
- All DXA numbers so far used 16 engines per SM (thread D).

## C. rtlsim checks (long)
- `par3_rtlsim_plain`, `par3_rtlsim_new`: SimX vs rtlsim with old and new
  memory settings, default 1-core config. SimX: 3,964 / 4,029.
- DXA dispatch, RTL change in `src_rtlfix` (three files under `hw/rtl/dxa`):
  small SM `dsp_simx` (31,777), `dsp_simx_old`, `dsp_rtl_base`, `dsp_rtl_fix`;
  full SM `dspF_*`. Expect rtl_fix < rtl_base and rtl_fix ~ simx.
- Compute roof: small `cr_{simx,rtlsim}_{base,fpu,disp,gpr}` (SimX IPC 0.44,
  1.10, 1.50, 1.30 — eight GPR banks LOWER than four here, unexplained);
  full SM `crF_{simx,rtlsim}_{base,new}`.
- Old `par2_rtlsim_w4/_w16` (h200q, old profile) still running; baseline only.
- If the RTL change verifies, port the three files to the main tree.

## D. DXA engine count and in-flight limit
- `VX_CFG_NUM_DXA_CORES = max(1, NUM_CORES / 8)` is the workers inside each
  socket's DXA core, so an SM has 1 / 2 / 4 / 16 workers at 1-15 / 16-23 / 33 /
  132 SMs. No profile sets it. The config comment and
  `docs/designs/dxa_async_copy_multicast.md` (sections 2, 3, 7) still describe
  a cluster-shared engine; the code moved it into the socket in `7d6950de5`.
- `VX_CFG_DXA_MAX_INFLIGHT` defaults to 8; runs used 64 or 256, passed by hand.
- Running: `eng_w{3,4,8}p{1,2,4}_n{8,32}` (engines x memory ports) and
  `inf_f{8,32,64,128,512}_n{8,32}` (in-flight limit, 4 engines, 2 ports).
  Target: about 101 GB/s per SM, the real H200 with four 16 KB transfers.
  Expectation: 4 engines needed (one engine is bound to ~35 GB/s by the memory
  round trip), 256 the smallest power of two that does not bind.
- Still to check before reporting upstream: upstream's current `master`, open
  PRs and issues; whether a later phase of the "socket migration" proposal
  covers the count.
- Decide: report as an issue (draft in English when asked); values for `h200s`.
- If one memory port is not enough: first find why a port passes about half
  its nominal 127 GB/s, before adding ports to the RTL.

## E. Things said earlier that were wrong (check the proposal text for them)
- "Vortex's DXA is a cluster-level shared pool, Virgo-style; moving it per SM
  is the top unbuilt item." Wrong since July: it is per SM. Virgo-style sharing
  is NOT the current state and would have to be built to compare against.
- "H200 has 48 banks per pseudo-channel" -> 32 is the likely value.
- "All GEMM tests are mine" -> upstream has several.
- Still to do: go through the earlier conversation for other structural claims
  not checked against code (shared MSHR, shared-memory write width, SM-to-L2
  structure) and list them with the code location.

## F. Not started / parked
- Co-run baseline on the new configuration (compute SMs + bandwidth SMs,
  separated; real H200 ratio 1.00), then the motivation experiment (few-SM
  bandwidth task cannot fill the memory).
- Borrowing design (2 SMs first) — plan was presented, not approved.
- Row timings (tRCD/tRP/tRAS/tRC/tRRD/tFAW) sensitivity; vendor values unknown.
- An SM's plain-load streaming is 45 GB/s (real 107): outstanding-miss limits.
- GEMM at 22 SMs still +28% with the old one-warp kernel.
- Push to the fork: `e113ec0df`, `a7b9d8a9d` are committed locally, not pushed.
- FP16/BF16/FP8 comparison; synthesis cost of every RTL-visible change.

## Documents
- `upstream-pr-candidates.md` — eight topics, status and evidence.
- `vortex-design-inefficiencies.md` — core and DXA findings from 2026-11-19.
- This file.

---

# Update 2026-11-20 (later)

## Baseline on the final profile (`h200s` at commit ef048c662, frozen copy `src_final`, results `fn_*`, table by `vortex-exp/fn_table.py`)
Settings: 16 real HBM3e channels, 8 bank groups, window 256, 1 KB mixed interleave; 8 DXA workers x 256 in flight; SM miss limits 512; dcache banks 4.

| | 1 SM | 4 | 11 | 22 | real H200 |
|---|---|---|---|---|---|
| plain stream, CUDA block shape, GB/s | 53 | 208 | 547 | 734 (92%) | 90% of peak with all SMs; 107 per SM alone |
| plain stream, own-stream shape | 86.5 | 213 | 526 | 738 | |
| DXA copy | 101 | 296 | 629 | 728 (91%) | ~101 per SM |
| decode GEMM load (`gemm_block -s0`), GB/s per SM | 16.0 | | 15.2 | 14.3 (39% of peak) | FP16 cuBLAS 16-20 per SM; 44-48% with all SMs |
| decode GEMM DXA d1-d3 | 25.7 | | 25.4 | 18.5-19.4 (52%) | FP8 TMA 29-35 to half the SMs, 19-21 with all (53-59%) |
| `gemm_decode` | 27.2 | | 27.3 | 19.4 | |

Compute: `corun -b0` IPC 3.31. `gemm_block` M=64: 4.73 M (load) / 3.62 M (DXA) cycles on 1 SM, +12% at 11 SMs (DXA).

Open from this table:
- own-stream shape falls from 86.5 per SM at 1 SM to 53 at 4 SMs with the memory at 27% of its peak. Not looked at.
- `gemm_block -s1` with MTILES=16 (M=256) FAILS its check (30 of 262,144 values wrong on the first run; failed again from the frozen copy). Load mode passes. A real bug in the DXA mode at large M; not investigated.
- L1 cache cut to 32 KB (what the shared pool leaves): load mode +1%, DXA mode +9% on 1 SM.

## Decided since the first half of this file
- A: committed and pushed (601a1d482..ef048c662). 8e8d21228 (role split) is local only.
- D: 8 DXA workers, 1 port, 256 in flight (profile). RTL needs the tag change in `vortex-exp/src_rtltag` (`VX_gpu_pkg.sv`, `VX_dxa_gmem_req.sv`); it elaborates. Not yet in the main tree.
- SM miss limits 512 (profile). Dcache banks stay 4; an agent is looking for why 4 banks give 87 GB/s (8 banks give 105) and for a fix that keeps 4 (`vortex-exp/src_dcache`, patch to `notes/dcache_bank_fix.patch`).
- Co-run scenario: Qwen3.5-9B decode beside Z-Image-Turbo (`corun-workloads.md`); both roles in one `gemm_block` launch, split by SM.

## New findings to follow up
- rtlsim on any H100-shaped profile stops at time 0: `VX_tcu_bbuf.sv:101` wants LMEM banks == B block words x sub-blocks (32 at 32 threads); the profiles set 16 (8 B banks = the part's 32 x 4 B). SimX has no such check. rtlsim runs now use copies with `VX_CFG_TCU_WGMMA_ENABLE` false (`src_rtlfinal`, `src_rtltag0`).
- Tensor core: the compute-bound GEMM does ~150 FLOPs per clock per SM against ~3,800 on the part (figure from memory, to verify); 35% of its instructions are fragment loads. An agent is measuring the unit alone (`tests/regression/tcu_roof`). Plan: wgmma kernel, then set the unit, then a reference figure in the profile.
- L1 cache and shared memory are separate structures, each at full size: 484 KB per SM against the part's 256 KB pool; the same in every profile from Volta on (v100 224 vs 128, a100 356 vs 192, rtx5080 228 vs 128). Upstream chose this knowingly (profile sources). Use a 256 KB split for any synthesis figure.
- Kernel cycle counts move ±2% with any change to the binary; differences of that size mean nothing.
- Real-GPU measurement of the two models: holder job queued (agent).
- Running agents: gemm_block co-run rates and weight reuse; tcu_roof; dcache banks; real GPU.

---

# Update 2026-11-20 (afternoon)

Pushed to the fork up to 348263623 (role split, dcache hold knob, tcu_roof, co-run rates, per-role walk, RTL tag width).

## Corrections to the section above
- "4 dcache banks give 87 GB/s, 8 give 105" was an artifact of differencing two run lengths in the own-stream shape. Whole-run, 1 SM: 47 -> 57 GB/s with the miss limits at 512; bank count +-4%. Commit ef048c662's message and the `h200s` profile `sources` text still say 87: fix when the profile is next edited.
- Own-stream shape numbers (86.5 etc.) are unreliable for the same reason; use the block shape (`-b32 -u4`).

## Dcache fill-forward hold (`VX_CFG_DCACHE_FWD_HOLD`, cbc696a98, default 0)
- RTL holds the fill-forward head when the 2-entry bank response queue is full (`VX_cache_bank.sv:1356-1360`); SimX replayed it. With hold (frozen copy, `hd_*`): block stream 51 -> 77 GB/s (1 SM), 202 -> 301 (4), 539 -> 730 (11 SMs = 91% of peak; real H200 fills 90% with half its SMs), 22 SMs unchanged (714). GEMM and DXA copy unchanged. CRSQ=30: no gain, not adopted.
- rtlsim parity run `par4_rtlsim_blk` (bwstream -n64 -b32, 1 SM) still running; SimX gives 25,824 (replay) / 20,266 (hold). When it lands: make hold the default if rtlsim matches hold, set it in `h200s`.

## rtlsim results so far (h200s, 1 SM, WGMMA off, tag fix)
- `crF`: rtlsim new 35,006 cycles vs SimX 33,419 (+4.7%). Base still running (SimX 118,418).
- `dspF` (bwstream_dxa -n8): rtlsim base 66,067 vs SimX old dispatch 61,767 (+7%). Fix still running (SimX 57,629). If fix < base, port `src_rtlfix`'s three files (`VX_dxa_{worker,core,dispatch}.sv`).

## wgmma tile buffer (`notes/wgmma_tbuf.patch`, copy `src_wg`; not in the main tree) — DECIDE
- Target 989.5 TFLOPS / 132 / 1.98 GHz = 3,786 FLOPs per clock per SM (third-party: tensor clock 1.83 GHz -> 4,096 = FEDP2K roof).
- Now 340 (FEDP2K): one shared CTA-keyed B buffer forces the four blocks into lockstep; no prefetch; one 128 B port.
- Knobs `VX_CFG_TCU_BBUF_ROWS` (per-block 2-slot B buffer + prefetch) and `VX_CFG_TCU_TBUF_PORTS`. `wg_*`, i512, FLOPs/cycle:

| ports | N=16 tile | N=64 tile |
|---|---|---|
| old structure | 341 | 328 |
| 1 | 948 | 1,326 |
| 2 | 1,513 | 2,384 |
| 4 | 1,988 | 2,989 |

- Bytes of operand per clock for 4,096 FLOPs: 320 (64x16 tile), 128 (64x64), 80 (Hopper m64n256k16). Vortex cannot do N=256 (accumulators in 32 FP registers), so the part's 128 B port and the part's FLOPs cannot both be met. Recommended to the user: 2 ports, stated as compensation for the tile size. Awaiting the user's choice. RTL side is a sketch only; it would also remove the `VX_tcu_bbuf.sv` bank-count assertion.
- wmma is capped at 256 FMA per block (all three register sources used); `sgemm_tcu_wg` is not sped up by this (bound by staging).

## Real H200, the two models — see `corun-workloads.md`
DiT 1.7% of memory bandwidth, proportional to SMs; decode matmul kernels 65-68%, 72% of speed on 20% of SMs, ~95-125 GB/s per SM with few SMs. **[corr. 2026-11-21]** "1.7%" counted weights only (measured 25-29% of the roof); the 20% point was under MPS, where cuBLAS computed NaN, and is invalid (`h200-go-nogo.md` §3; 21:10 below). Decode wall-clock in HF transformers is host-bound.

## gemm_block co-run (9167e8e3e, 69e11596b)
- `-r` rounds, `-o` solo role, `-R` compute-role weight reuse, `rate_*` and `rate_steady_*` lines; memory role has its own one-tile walk (`-DBW_TILES`).
- Co/solo with FIFO L2 (steady window): memory role 0.95-1.03 (one outlier 0.85), compute role 0.86-1.04. Cause found by the agent: the shared L2 replaces FIFO, so a stream evicts the compute role's hot lines; with `-DVX_CFG_L2_REPL_POLICY=2` (pseudo-LRU) the compute role ran at solo speed. Real GPU ratio is 1.00.
- Running: `l2p1_*` vs `l2p2_*` (46 runs, frozen copy `src_l2` = HEAD, FWD_HOLD=1): stream, decode GEMM, DXA copy, M=64, co-run and solos at 2 and 4 SMs. DECIDE with the user: pseudo-LRU L2 in `h200s` (real policy unknown).
- First generation of CTAs on a core runs in step and 17-34% faster than later ones (4-tile walk); not explained. Use the steady window.
- MTILES>1 walk spills 24 loads + 24 stores per K step: `mma_sync` pins 24 of 32 FP registers.
- DXA mode cycle counts move up to 6% with code placement.

## Bugs being traced (agent, copy `src_m16`, patch to `notes/m16_dxa_fix.patch`)
- `gemm_block -DMTILES=16 -m256 -n1024 -k4096 -s1` (ring depth 2): 1-30 wrong outputs, always tile 0, rows 8-12, cols 12-15; `-d1` and `-s0` pass. Points at SimX (FP registers parked across a DXA completion), not verified.
- Runtime segfault before the kernel when one buffer is ~38-42 MB on 1-2 cores (`-n5120 -k4096`), in `make_cp_hooks` dram_read.

## Still to add to `upstream-pr-candidates.md`
dcache hold divergence; wgmma B buffer/lockstep and bbuf assertion; L1 + shared memory 484 KB; FIFO L2 cross-SM interference; the two bugs above once understood.

## Update 2026-11-20 (evening): SimX correctness fixes, pushed up to 51a4cce00
- dab77b7f2 + 51a4cce00: dcache same-word ordering decided at admission like the RTL's S0 hazard. Before, an older replayed load could read a younger store (wrong results with MTILES>=4 spills; errors under 1% passed the host check silently). New test `tests/regression/war_order` (fails before on h200s and a 16x16 write-back config, passes after). One-tile cycle counts unchanged; MTILES>=4 results from before are provisional. Reviewed twice, nothing blocking.
- b974a9e98 (+ rtlsim in 51a4cce00): CP host addresses tagged with bit 62; a device address could alias a host heap region (crash at ~40 MB uploads, or a silently missing 64 KB chunk).
- Review follow-ups not done: LR/SC reservation not invalidated by a write-through merge replay (cache.cpp ~1995, only matters for a write-through LLC with atomics); loads demote on any-word writes where the RTL is per word (cycle/counter divergence); war_order not in ci/testcases; foldsim/firesim runtimes still untagged; run war_order on rtlsim.
- Rerunning the whole matrix on the fixed code: `ordp1_*` (FIFO L2) and `ordp2_*` (pseudo-LRU), 72 runs, copy `src_ord`, FWD_HOLD=1, DXA depths 1-4.
- First L2 comparison (`l2p*`, pre-fix): pseudo-LRU brings the compute role's co/solo from 0.78-0.94 to 0.98-1.08, solo runs within 1% (stream 11 SMs +2.7%). Memory role at 2 SMs with loads stays 0.79-0.86 under both: separate cause, open.
- Verified the reruns build from the copy's sources: war_order passes on src_ord and fails (125 errors) with only cache.cpp reverted. ordp*/l2p* MTILES=4 cycle counts are identical before and after the ordering fix because the hazard did not arise in them. rtlsim: hold matches (20,570 vs 20,266), dispatch fix 66,067 -> 60,307, compute roof 118,350 / 35,006 vs SimX 118,418 / 33,419. Pushed up to b0adb2663 (h200s FWD_HOLD=1, RTL DXA dispatch).

## Update 2026-11-21: memory-role "0.86" in the 2-SM load co-run is not interference
- Agent result (runs `bwco*`, instrumentation `notes/bwco_instrumentation.patch`, copy `src_bwco`): a load-mode memory-role core has two stable states of its own. Fast: its 16 CTAs run in lockstep generations, ~262K cycles per walk. Slow: CTAs staggered, ~306K. 263.5/306 = 0.861. Runs switch state on their own (also with 1 core and nothing else); the 0.87 compared a co-run in one state with a solo in the other. Like-state ratio 0.99-1.00. Matched pairs over N/rounds range 0.87-1.18.
- No memory-side stage is slower in the slow state (L1 miss round trip 550 vs 561, L2->memory 382 vs 383); fewer misses are in flight (73 vs 61) and barrier waits grow 3.3K -> 15.9K per warp per walk. Why lockstep is 15% faster, and what triggers a switch, are not explained.
- DXA mode has no such states (walk 150-156K throughout): ratio 0.98-1.00.
- The `-o1` solo mode's fences push runs into the slow state early; a 1-core run or fence-free idle CTAs (`-D`, in the patch) is the better solo baseline.
- Consequence: load-mode windowed rates carry about +-15% of state noise. Compare by state-separated per-CTA walk times, or average several N/rounds variants. The pseudo-LRU conclusion for the compute role rests on the per-generation comparison (stream vs no stream vs pseudo-LRU), not only on windowed ratios, but its 1.00-1.07 spread is this noise.

## Tensor memory (agent, copy `src_tmem`, `notes/tmem_roof.patch`)
- TMEM engine on h200s, FEDP2K, one 128 B port: 976 / 1,324 / 1,611 / 1,804 FLOPs per clock at N = 64 / 128 / 256 / 512; 1,950 with A copied into TMEM. Roof 4,096, part 3,786. Port carries 34 B/clock: not the limit. Limit: B prefetch is one block ahead and takes 4 cycles for 2 compute cycles. GEMM test 1,292-1,399 (staging-bound). wmma, DXA and gemm_block cycles identical in a TMEM build; wgmma excluded. Lone issuing warp gives one instruction per ~13 cycles.
- Running: prefetch depth 2 and 3, A by bank rows, the issuer limit, GEMM check again.

## Update 2026-11-21 (night): four findings that change the plan
- **Barrier waits for the whole core's LSU to drain** (agent, `bwco_instrumentation.patch`, runs `bwco5-8`). `VX_wctl_unit.sv:185` (`bar_drain`), SimX `sfu_unit.cpp`: a barrier passes only when no load/store of ANY warp of ANY CTA on the core is queued or awaiting a response; the single SFU block is blocked behind it (the `csrr tid` after each barrier too). This is the cause of the two states in load mode (lockstep 262K vs staggered ~300K per walk): CTAs can only release barriers at core-wide drain moments. Random CTA starts always end in the slow state, so the slow state is the honest one; the decode baseline `-m16 -n1024 -k4096` (all 16 CTAs launched together) is a lockstep figure: 15.9 GB/s per SM vs 14.8 staggered. With a per-warp drain (`VX_BAR_DRAIN=warp`, SimX experiment, results still pass): load mode 6.5-7.8 -> 11.6 B/cycle (x1.5-1.7), no state switching, decode baseline 22.5 GB/s per SM, co/solo 0.99. SimX matches the RTL here; it is a design property, stronger than CUDA's bar.sync. Changing it is a hardware change (RTL + SimX). DECIDE.
- **Tensor memory** merged on branch `feature/tmem-merge` (worktree `vortex-exp/wt_tmem`, 3 commits, not pushed): merge 7252188f7, tmem_roof 74505b18e, SimX knobs cae6b3be6. SimX/rtlsim agree within 0.05-1.5% with knobs off; RTL B turnaround measured 4.0 cycles per block. Knobs on (SimX only): 3,684 FLOPs/clock at N=512. Open: RTL for the knobs; `VX_tcu_bbuf.sv` assertion (`B_BLOCK_WORDS * SUB_BLOCKS == NUM_BANKS`) fails on h200s because the profile's 16 banks are 8 B wide (= 32 logical 32-bit words per row, the part's 128 B/clock): the buffer should count logical words (NUM_BANKS x XLEN/32), not the profile change its banks.
- **Design-space study** (`noc-and-design-space.md`, artifact https://claude.ai/artifact/2E7qgQ3Gcm3FDJGKG57bfw): three per-SM resources (request machinery, shared-memory ingest 128 B/clock shared by writes and reads, compute). Borrowing adds only the first; staged-then-consumed caps near 127 GB/s per SM in Vortex (1.25x). Vortex has no network (4 private 94-cycle links per SM into one L2). Hopper already allows software borrowing inside a cluster. Co-residency is a strong software baseline. Real-H200 experiments E1/E2/E3 running (agent, holder job).
- **22-SM split** (`split_*`, 9 of 32 done): memory role alone with DXA is linear in SMs, 25-26 GB/s per SM (b=4: 100, 6: 150, 11: 285 GB/s; 22: ~425). That per-SM figure is the kernel's consumption, not the request limit (copy does 101). Co-run windows only 20-35% of the run: redo with matched rounds.
- Compute-role kernel on TMEM: agent writing `tests/regression/gemm_tmem` in `src_tmemk`.

## Update 2026-11-21 (early morning): real-H200 E1-E4 overturn the "100 GB/s per SM" premise
Raw: `results_gpu/h200_borrow_*.txt`; sources `gpu/borrow_*`; holder job 15953169 (39 min, cancelled). All TMA runs checksum-verified; SM of every CTA read back with `%smid`.
- **The per-SM cap is ~210-219 GB/s (~110 B/clock), not ~100.** The 100 figure was 4 x 16 KB in flight (latency-bound). One SM alone reaches the cap with 32 KB transfers at D>=6 (208), or with several issuing warps (3 x D4: 205; 7 x D2: 211). 16 KB transfers from one thread stop at 142 (one transfer per ~115 ns per issuing thread; 8 KB gives 71). Same ceiling for L2-resident and DRAM data, local and remote issuers. Plain loads stop at ~115 GB/s.
- **E2: software borrowing works but adds nothing over local issue.** Helpers 1/2/3/7/14 -> 75-79 / 147 / 200 / 215 / 215 GB/s write-only into one SM; issuers inside the consumer CTA give the same. Staged-then-consumed: 100-170 depending on consumer warps (157-170 best); reads alone 251-253 GB/s (127 B/clock) with 7-16 warps, so writes and reads partly share the shared-memory bandwidth. Consumer that also issues while helped: 100-155. Remote transfer latency: +40% cold, ~2x warm. A helper CTA cannot be made small (a cluster is one launch: same block size and shared memory for every CTA).
- **E3: co-residency is poor.** TMA stream beside the FMA kernel keeps 0.08-0.22x (FMA keeps 0.94-0.97); plain loads 0.17-0.33x (FMA 0.63-0.80). cuBLAS FP16 GEMM (`nvjet_hsh_320x128...`, 384 threads x 168 registers, 0.189 MB smem) uses 64,512 of 65,536 registers per SM: nothing else fits.
- **E4: a barrier does not wait for other CTAs' loads** on the real part; lockstep vs staggered starts make no difference (2503-2536 cycles per iteration either way). Shared-memory access itself is ~12x slower while neighbour CTAs stream global loads (shared load/store path, inferred). Supports changing Vortex's core-wide barrier drain.
- **Consequences.** (1) The research idea as stated (borrow other SMs' TMA engines to exceed one SM's request limit) has no bandwidth to gain on H200: a well-written kernel already saturates the SM's ingest. (2) The real limits are per-SM ingest (~215; ~160 staged), then placement/uplink (the MPS sweep fell from 107 to 77 GB/s per SM at 8 -> 22 SMs; unexplained), then memory. A memory-bound job needs ~21-28 SMs to fill 4.5 TB/s. (3) Sharing SMs is where the loss is (0.08-0.22x): isolation for co-resident jobs ("memory lane") and bypassing shared memory are the live hardware directions. (4) Calibration targets change: Vortex DXA tops out near 101-111 GB/s per SM (one 64 B/clock port); the part does 142 (16 KB) and 208 (32 KB). Vortex shares LMEM bandwidth fully between writes and reads; the part only partly.
- Placement agent (TPC masks, `notes/tpc-placement-and-borrowing.md`) redirected to: placement/uplink at the 210 GB/s ceiling, co-residency trade-off curve, borrowing only as a short check.

## Update 2026-11-21 (04:30): TPC masks and placement on the real H200 (`tpc-placement-and-borrowing.md`)
- Per-launch TPC masks work without root (`gpu/tpc_smctrl.h`, `tpc_preload.so`); all 66 live TPC bits validated against `%smid`. Topology of the 8 GPCs recovered.
- **A GPC returns at most ~1.28 TB/s** (about 6 SMs' worth). 16 memory SMs packed: 1.28 TB/s; spread over 8 GPCs: 3.1-3.5 TB/s (2.4x). 32 spread: 4.56 TB/s; chip roof 4.63. Both SMs of a TPC: no penalty.
- MPS packs a client's SMs into few GPCs (inference, strongly supported): explains the old 107 -> 77 GB/s per SM.
- Borrowing inside a GPC adds nothing; distance irrelevant.
- Co-residency: the stream is starved (0.6-6.7%) if compute launched first or has ILP 4; keeps 100% only if resident first with compute ILP <= 2 (compute then loses 9-25%).
- Best today: disjoint SMs spread over all GPCs, <= 5-6 memory SMs per GPC. Beside a CUTLASS FP8 GEMM on the other 100 SMs: memory job 4.49 -> 3.7 TB/s, GEMM -10%.
- LithOS picks a TPC count per kernel, not a placement; no GPC/memory awareness; evaluated on A100. No public code found.
- Hardware directions left: GPC egress / cross-GPC paths; issue priority or a reserved lane for a co-resident memory warp; bypassing shared memory.

## Update 2026-11-21 (evening): critique, design studies, review findings

Decisions by the user
- Network model baseline `h200g2`: 2 partitions, each one GPC of 16 SMs, 16 L2
  slices and one HBM3E stack (32 SMs, ~1.6 TB/s). Deviation: memory per SM is
  1.375x the part (**[corr. 2026-11-21]** was "1.43x"; like for like it is
  (2 / 32) / (6 / 132) = 1.375, `noc-design-verification.md` §1.4). The build
  waits for the in-flight patches; design first.
- SimX wall-clock optimisation started on branch `feature/simx-perf`
  (cycle counts must stay bit-identical).

New notes
- `framing-critique.md`: reviewer-style critique. Verdict "revise".
- `gpc-limit-design.md`, `smem-bypass-design.md`: both conclude "not a
  proposal on its own". `sm-sharing-design-space.md` is still being written.
- `related-work-placement.md`: placement is taken (GPCPlace, Fan et al.).
- `noc-design-verification.md`: independent verification of the network design
  (in progress).

Corrections to earlier statements
- "Memory job starves to 0.6-22% when sharing SMs" holds only beside the
  synthetic FMA kernel. Beside a cuBLAS GEMM (`h200_borrow_e3.txt` lines 14-21)
  memory keeps 0.63-0.78 and the GEMM drops to 0.25 at 132 memory blocks; the
  two alternate on SMs.
- Qwen decode printed `'!!!!…'` in 4 MPS runs and 2 co-runs; those timings come
  from wrong output. Cause unknown. **[corr. 2026-11-21]** Cause found (21:10 below): cuBLAS NaN under MPS
  percentages; those runs are invalid.
- Two packed GPCs give 2.27-2.28 TB/s, not 2 x 1.28.
- "cuBLAS FP16 small M: 16-20 GB/s per SM" is an all-SM, chip-limited figure;
  on 8-14 SMs it is ~52.
- Decode kernels sit at the tensor core's bound for an M padded to 64 rows
  (117 GB/s per SM; 59 for a 128-row tile), not at the ingest ceiling.
- Barrier scope 1 (h200s): review found a store stage the RTL condition does
  not see (the dcache flush arbiter's buffer), no parity run with VM on, and a
  description narrower than the code. The "15.8 -> 22.2 GB/s per SM" figure is
  (**[corr. 2026-11-21]** 15.08 -> 21.06, 21.03 after the fix; 22:40 below)
  unconfirmed until the fix branch `feature/bar-drain-fix` reports.
- DXA worker count: 1 at 1-15 cores, 2 at 16-23, 16 at 132 (the findings list
  says 1 at 1-8; to be corrected in both languages).
- LR/SC at large configurations is slow, not livelocked (cycles scale linearly
  with cores, ~quadratically with warps).

Real-H200 experiments owed (one holder job at a time)
1. L2 structure + bystander/saturation/L2-aggregate tests (running).
2. Go/no-go for SM sharing: lane-sized decode kernel beside a DiT-shaped
   production GEMM, both launch orders, 1 s windows, clock/power logged.
3. Go/no-go for the shared-memory bypass: swap-AB wgmma decode kernel in three
   operand variants on one SM.
4. Cause of the `'!!!!'` outputs; rerun the MPS decode numbers.

## Update 2026-11-21 (night): L2 structure, network design verdict, shapes, accounts, what is owed

Decisions by the user
- Network model: the GPC limit is a **return-side gate**, because that is what
  reproduces the bystander effect (prototype: -30% for a bystander with the
  return-side gate, -3% with a request-side one; the part: -45 to -54%).
  Replication of remote reads is to be built. Per-SM rates are calibrated
  before any placement number is compared with the part. Both `h200g2` (fast
  iteration) and `h200g4` (ratio-dependent results) are used.
- Upstream PRs are low priority. The PR effort was wound down (19:20); the
  branches are kept (15 local `pr/*`, nothing pushed, no PR or issue opened),
  verified at XLEN 64 only (no 32-bit toolchain here). Final state:
  `pr/notes/INDEX.md`.
- Functional work is consolidated on `feature/bar-drain-fix`, to be merged into
  `feature/dxa-borrowing`. Performance optimisation (`feature/simx-perf`) and
  the network model stay on their own branches.
- ICE, a free cluster, is validated for SimX: cycle counts bit-identical to
  Phoenix on three reference runs (`results/icev_*_ice` against `_phx`).
  Scripts in `vortex-exp/ice/`.

H200 L2 / memory-side structure (`h200-l2-structure.md`; holder 15976443, 43
min, a third die; raw `results_gpu/h200_l2s_*`)
- Structure: 96 L2 slices (8 per FBP, 12 FBPs, 60 MiB, ncu). **2 partitions** of
  4 GPCs: side A = GPCs {0, 1, 5, 7}, side B = {2, 3, 4, 6}. Partition of an
  address = PA12 ^ PA13 ^ PA15 ^ PA17 ^ PA19 ^ a per-2 MB-page constant (zero
  error on 65,536 lines; identical to GemAtlas's H20 hash); 4 KB interleave.
  A remote read leaves a copy in the reader's half. Slice routing fits a 256 B
  granule (bits 8, 10, 11 matter, 7 and 9 do not): **it argues against the
  1 KB interleave** in `h200s` (medium confidence).
- Latencies (SM cycles at 1980 MHz): L2 hit near ~290 (sub-classes ~280 / ~305),
  far ~467; DRAM miss with a near home ~546, far home ~733; 7-10% of cold lines
  at ~960-975. A DRAM miss fetches 64 B.
- Limits: **per GPC 1.28 TB/s** (~648 B per SM clock) of unique lines, reached at
  8 SMs, for DRAM and L2-resident data alike (shared lines merge and go past it:
  2.75 TB/s at 18 SMs, which is Fan's 2.8). **Cross-partition fabric 1.28 TB/s
  per direction** (remote-only chip 2.56; local-only 4.63; mixed 4.58). Total =
  min(GPCs x 1.28, 2.56 per half, ~4.6). Writes 63 GB/s per SM, no GPC limit
  reached. The earlier "two packed GPCs 2.27-2.28 TB/s" was a makespan
  artefact (all 28 pairs give 2.54-2.56).
- **Bystander effect**: one saturated GPC (16 SMs) cuts unsaturated SMs in every
  other GPC from 192 to 88 (same half) or 106 (other half) GB/s per SM with TMA
  (93 / 132 with loads); the chip total falls below the sum of the parts. With
  6 SMs in that GPC the others keep 177-182. Mechanism open; a dedicated
  experiment is running (`h200-bystander.md`, draft).
- **Per-SM ingress**: one ceiling shared by TMA and loads, 215-230 GB/s
  (110-116 B/clock) together, not 196 + 218; shared-memory bandwidth is not
  the limit (TMA keeps 174-180 with readers taking 54-99). Near 223 / far 206
  GB/s for loads. Two SMs of a TPC give exactly 2x.

Network design verification (`noc-design-verification.md`, being edited)
- Verdict: **sound with required changes.** The structure (GPC switch, two
  partitions, slices bound to channels, one inter-partition link) is
  realisable, the zero-load latency budget matches our H200 within 2%, and the
  knobs-off prototype is the old code path cycle for cycle; not ready to build
  as written.
- Blocking items and their state against rev. 3: B1 `h200g2`'s link default hid
  the GPC limit (accepted: B_x = B_g = 648, closed); **B2 fences are not ordered
  through address-hashed lanes** (accepted, not implemented; every result with
  a fence or flush between memory phases is unsafe until it is); **B3 no
  mechanism or target for the bystander effect** (mechanism and target added,
  open until a SimX run shows it); **B4 a GPC plateaus below its configured
  cap** (gate bugs reported fixed, re-measurement and review not done).
- Still open in rev. 3: the new per-SM ingress gate holds responses at the
  socket and would slow SMs of an unsaturated GPC, which the part does not
  show; credit accumulation needs a bound; replication with write-to-home +
  invalidate needs its own correctness review; the other-half bystander value
  (86 vs the part's 106).

Network model shapes (`noc-model-design.md`, being edited)
- `h200g2` (reference): 2 partitions x 1 GPC x 16 SMs x 16 slices x 1 HBM3E
  stack = 32 SMs, ~1.6 TB/s, 20 MiB. Packed in one GPC 1.28 vs spread 1.54
  TB/s; the fabric never binds; only other-half bystanders exist. **Memory per
  SM is 1.375x the part** (48 GB/s per SM achieved against the part's 35; the
  earlier "1.43x" is corrected above).
- `h200g4` (exact ratio): 64 SMs = 4 GPCs x 16, 2 per partition, 3 stacks.
  Memory per SM, SMs to fill memory (17%), L2 per SM and the GPC/memory ratio
  per side match the part; packed 16 vs spread 16 = 1.8x.
- `h200g4x8` (4 GPCs of 8): the cost of `h200g2`; separates the GPC effect from
  the partition effect.

Charge accounts and clusters
- CPU jobs are billed on Phoenix. CPU jobs now run on `gts-hkim358` (the
  account is given at submission; `run_point.sbatch` still names
  `gts-hkim358-paid`, which the GPU holder jobs keep using).
- ICE is being set up: see `ice-cluster.md` (in progress) and, for the SimX
  validation, above. ICE has no charge account
  (association "coc", partition `ice-cpu`, 18 h, QOS `coc-ice` 50 submitted
  jobs per user: `ice/run_point_ice.sbatch`).

Still owed
- `dxa_mc_parity.patch` (item 31): waiting for its rtlsim reference run
  `par_h2_dxa32_rtl` (running), because one `h200s` point moves +20%.
- RTL for the per-warp rate (items 22, 29: `VX_CFG_FETCH_INFLIGHT`,
  `VX_CFG_FFLAGS_ACCUM`): deferred until a research direction is chosen.
- Barrier scope 1 on `h200s`: unconfirmed until `feature/bar-drain-fix` is
  verified and merged (item 18).
- Real H200: the go/no-go experiments A, B, C (`h200-go-nogo.md`, in progress) and the bystander characterisation (`h200-bystander.md`, draft) are
  running.

## Update 2026-11-21 (21:10): go/no-go results on the H200 (`h200-go-nogo.md`)

- A (SM sharing): a lane-sized decode kernel (1 warp, 32 registers, 33 KB shared
  memory) never co-resides with a production tensor-core GEMM (cuBLASLt and
  CUTLASS, BF16/FP8, M=4096, K=3840, N=3840/15360). Lane first on all SMs: the
  GEMM makes no progress (0.1-0.3 TFLOP/s against 600-1220). GEMM first: whole
  SMs alternate; lane 0.09-0.34 of solo on 12-48 SMs, GEMM 0.59-0.79. CUTLASS
  with one fewer stage still does not co-reside (inference, unverified:
  registers are allocated per sub-partition). Where SMs are shared, the lane
  starves: 0.051 beside an elementwise kernel (stall is not "not selected"),
  0.002 beside FMA ILP 4 (90% "not selected"). Decision rule outcome: go,
  residency direction. Caveat: the lane itself reached only 8.7-10.2 GB/s per
  SM with its consumer (30.5 TMA-only).
- B (shared-memory bypass): not decided. The test kernels reached 52.6-66.2
  GB/s per SM, below cuBLAS on the same SMs (119-125), so they do not probe
  the hardware limit.
- C: cuBLAS 13.4 (PyTorch 2.15+cu132) returns NaN or wrong values for BF16
  GEMMs under an MPS active-thread percentage (NaN at 104/92/52/26 SMs, correct
  at 132/78/12; 18 of 30 standalone shapes bad at 80%). With TPC masks the
  output is correct. CORRECTION: every Qwen number taken under MPS 80/70/40/20%
  and the co-runs at LLM 20/40% came from runs computing NaN; do not quote them.
- CORRECTION: "the DiT step uses 1.7% of memory bandwidth" counted weights only.
  Measured with ncu: 178-209 GB per forward = 1.16-1.36 TB/s, 25-29% of the roof.
- Branches on the H200 (`results_gpu/h200_warprate_nottaken.txt`): a not-taken
  conditional branch costs about what a taken one does (11-12 cycles), no
  history effect. The part stops fetching at a branch; no prediction.
- Per-warp rate in Vortex: pipelined sequential fetch chosen (variant C);
  one-fetch-in-flight (A) saves only 1 cycle. With it the next per-warp limit
  is the issue path (~3.8 cycles per instruction against the part's 1.2).
- DXA timing patch: the rtlsim reference for h200s `-e4096 -n32` is 201,583
  cycles; SimX today 182,421 (-9.5%), with the patch 219,161 (+8.7%). Not
  applied; a second, depth-dependent difference remains.

## Update 2026-11-21 (22:40): bystander effect, cause (`h200-bystander.md`)

Measured on a second die (atl1-1-02-014-9-0), 1,178 trials, 1 s windows.
- Trigger is backlog, not bytes: one GPC delivering the same 1.27-1.28 TB/s
  leaves same-half bystanders at 168 GB/s (aggressor depth 3 x 32 KB per SM)
  or 88 GB/s (depth 6); 196 alone. 8 vs 16 aggressor SMs: 174 vs 88.
- Not DRAM (an L2-resident aggressor cuts bystanders to 0.45x), not the fabric,
  not clock or power (1980 MHz, 372 W of 560 W).
- Harm follows the L2 slices serving the aggressor's data (local pages hurt
  that half's local readers 0.57x, other half 1.00x; and the reverse).
- A shared-buffer aggressor (few unique lines) pushes 2.30 TB/s out of its GPC
  and bystanders stay at 0.99x.
- Isolated same-half near hit: 292 -> 1444 cycles (p50).
- Software mitigation works: an in-flight cap of 3 x 32 KB per SM costs the
  aggressor 0.5% and restores bystanders to 0.86/0.92x; spreading the same
  rate over 2-4 GPCs costs nothing and gives ~0.90x.
- Practical size: a decode-like lane beside one packed memory-heavy tenant
  runs at 0.31x; the chip moves 2.15 TB/s against an ideal 4.39.
- Inference (not observable): credit or head-of-line blocking from the
  congested GPC's port back into those slices.
- Die variation: this die's L2 halves are {0,2,6,7} / {1,3,4,5}; one "GPC"
  group delivered 1.47 TB/s.
- For the simulator: slices need a shared response buffer across destinations,
  the GPC port returns credits at 1.28 TB/s, SMs need full in-flight windows,
  and same-line merging inside the GPC must exist.

Merged today: `feature/bar-drain-fix` into `feature/dxa-borrowing` (18deea5da,
pushed). Scope 2 was not built: the scope-1 starvation did not reproduce on the
work branch (it came from changes in the load-calibration copy). Decode figure
corrected: scope 0 -> 1 is 15.08 -> 21.06 GB/s per SM (21.03 after the fix),
not 15.8 -> 22.2. Started: `feature/noc` (steps 0-1), `feature/warp-rate`
(pipelined fetch in RTL and SimX, 49 of 52 parity rows cycle-exact).

## Update 2026-11-22: merges, build defect, profile changes, what is open

Work branch `feature/dxa-borrowing`: pushed head d1e537603; local (waiting for
h200s-size rtlsim references) 4001ed275, 602af5c84, 0c75a1dcd.

### Merged and pushed today
- a2341f5f7 SimX host-speed stack (59 commits). Cycle-identical in three build
  modes: 17 study cases (fresh trees, profile applied to the whole build), 310 CI
  cases as CI runs them, 310 with `--perf=1`. Wall speedup 2.0-7.0x on h200s
  cases (table in `simx-perf.md` section 14).
- 73b994588 + bb5d16d87 `VX_CFG_LMEM_TCU_RDPORT` (the tensor core's
  shared-memory operand reads use the banks' read port); h200s sets it.
  gemm_tmem_decode one SM 1,633,486 -> 1,324,983 cycles (81.5 -> 100.5 GB/s).
- 447824a88 knobs, default = old timing: `VX_CFG_LANE_DISPATCH_GAPLESS`
  (bits 1 ALU, 2 LSU, 4 SFU, 8 FPU, 16 TCU), `VX_CFG_FETCH_REQ_BUF`,
  `VX_CFG_FLUSH_REQ_BUF`, `VX_CFG_OPC_MERGE_BUF`.
- d1e537603 build fix: `sw/runtime/libvortex.so` gets a flag stamp and
  rebuilds when CONFIGS change.

### The build defect (why "same configuration" gave different cycles)
The host runtime stub was built once by `make software` and never rebuilt for
`--profile` or CONFIGS. Under `VX_CFG_VM_ENABLE` the stub reserves a 256 MB
pinned slab (`sw/runtime/common/device.cpp:38`), so a default-built stub put
every buffer 256 MB lower. Same instructions, different cycles: h200s bar_vis
165,633 (stale stub) vs 154,182; bwstream one SM 156,327 vs 107,249 (-31%);
gemm_block 665,219 vs 665,514. Only VM configurations are affected; the `-D`
parity sweeps without VM were not. `run_point.sbatch` numbers were always the
consistent kind. Earlier explanations (source copy, PERF flag, stale objects,
kernel library, config header) were wrong. Not traced: why a 256 MB shift costs
7-31%. Left: OpenCL/HIP apps link the kernel library's CTA entry, which still
has no stamp.

### h200s profile, local commits
- 4001ed275: FETCH_REQ_BUF=0, FLUSH_REQ_BUF=0, OPC_MERGE_BUF=0.
- 602af5c84: LANE_DISPATCH_GAPLESS=1 (ALU bit). The user accepted the
  branch-dense overshoot on 2026-11-22 (64 warps, a branch in every
  instruction: 1.77 instr/cycle vs the part's 1.16 with a 12.4 KB loop).
- SimX, one SM, d1e537603 -> 4001ed275 -> 602af5c84: bar_vis 154,182 /
  152,601 / 146,121; fetch_squash 10,495 / 10,074 / 10,016; gemm_block
  (-m16 -n1024 -k4096 -s1, MTILES=1) 665,514 / 632,810 / 574,100; bwstream
  (-n2048 -u4 -b32) 409,346 / 410,006 / 419,474 (slower, unexplained);
  gemm_tmem_decode (-m16 -n8192 -k4096) 1,324,983 / 1,322,752 / 1,319,938.
- Full before/after table (277 points): `results/wl_pf2_table.txt`.
- H200 branch-rate measurement (`results_gpu/h200_brcap_*`, preliminary): the
  1.16 saturation depends on code size (32-branch loop of 3.2 KB reaches 1.70
  branches/clock at 64 warps), so it looks like an instruction-cache capacity
  effect with many warps, not a branch limit. A footprint sweep is queued.

### bar_vis is bistable (do not use its cycle count as a parity yardstick)
Its noise CTAs read the same lines two iterations apart; the dcache holds
exactly the ~256 lines filled in between, so the second touch hits or misses
depending on a one-or-two-iteration skew between CTAs. A one-cycle difference
selects the regime. h200s-size SimX ranges 127k-165k across fix/knob
combinations with no relation to removed latency. At the final profile settings
with fixes (a)+(b), small shapes agree within 0.62% (rtlsim/SimX: 16x32 scope 0
42,518 / 42,698; scope 1 33,597 / 33,536; 8x32 43,747 / 43,477; with VM
45,122 / 45,022).

### Unmerged work
- `feature/lane-parity-r` (fix (a) LSU input capacity, fix (b) lane-dispatch
  block coupling, parity cases), being rebased onto 602af5c84; (c) = two
  one-cycle differences on the dcache miss path (lookup stage; miss-to-fill 34
  vs 35 cycles), being pinned in RTL.
- `feature/noc` (steps 0-1, head ffb5f9b74): review findings fixed; the
  bwstream default shift was the kernel image growing inside the measured
  window (fixed: timing kernels in a second binary). Open: lane-shape parity
  (ungated groups: RTL round trip ~105 cycles vs SimX 87; hypothesis: the L2
  gets 8 banks instead of 16 with 4-lane groups). Runs on ICE.
- `perf/fix3`: fedp reduce_f8 negative shift (results unchanged; 45M
  evaluations) and a DRAM read held across reset (never reached in 320 runs;
  unit test only).
- Parked, not to be merged: `perf/mt3` (multi-thread SimX; 425/425 runs
  cycle-identical, 3.5x at 22 SMs; TSan never ran), `perf/build2`.

### One SM's memory rate (agent `sm-rate`, `notes/sm-rate.md`, interim)
- Shared-memory stores: the local-memory unit takes one LSU block's request
  (8 lanes) per cycle for writes, 16 lanes for reads: 4.02 cycles per 32-thread
  store, 2.02 per load. 4-byte-spaced stores take 12 cycles (two lanes writing
  halves of one 8-byte bank word count as a conflict). Part: 1.02 stores/clock.
- DXA engine tops out near 52 B/clock for both the copy (part 103.5) and the
  decode kernel (part's tensor bound 58): one 64 B line per cycle on its memory
  port, and a 610-cycle issue loop per 16 KB tile (part ~228).
- Plain loads, like for like (128 KB in flight): 70 B/clock vs ~110. The
  "79 vs 218 GB/s" comparison used different in-flight depths.

### Other findings
- `tests/regression/dogfood` sub-test bar fails with default arguments on
  upstream/master too (1,008 result errors); CI never runs it that way.
- No SimX build without NDEBUG compiles (`__assert` macro collides with glibc).
- war_order passes on rtlsim (1,296,287 cycles; SimX 1,295,606) at 16x16 with a
  write-back dcache; CI case added locally (0c75a1dcd).
- The Phoenix job `slowdown` hosts the Claude session; never cancel it.

### Not owned by anyone yet
Shared-memory atomics (SimX 309,992 vs rtlsim 141,498 at 32 threads); DXA deep
in-flight +-9%; h200s-size rtlsim beyond four short tests (waits for `rover`);
logic depth of every new knob.

## Update 2026-11-22 (17:25): pushed f55faaa2d; agents stopped by the account's weekly limit (resets 2026-11-25 06:00 ET)

Pushed to the fork: `feature/dxa-borrowing` = f55faaa2d (final h200s profile:
FETCH_REQ_BUF/FLUSH_REQ_BUF/OPC_MERGE_BUF = 0, LANE_DISPATCH_GAPLESS = 1;
SimX lane-parity fixes (a)+(b) with parity cases; fedp reduce_f8 and DRAM
reset fixes; war_order CI case; findings 37-50).
SimX baselines on it (h200s, one SM, run_point): fetch_squash 10,018;
warp_rate 113,382; tmem_roof -n512 -i8 62,075; gemm_block -m16 -n1024 -k4096
-s1 (MTILES=1) 562,462; gemm_tmem_decode -m16 -n8192 -k4096 1,320,528;
bwstream -n2048 -u4 -b32 449,189; bar_vis 127,357 (bistable); sgemm -n64
792,351.

### h200s-size rtlsim references (lab server `rover`, RTL 602af5c84 = f55faaa2d RTL)
warp_rate 113,406 (SimX -0.02%); tmem_roof 61,749 (+0.5%); fetch_squash
10,068 (-0.5%); war_order 329,817 (SimX 344,196, +4.4%); bar_vis 145,029
(SimX 146,121 before the lane fixes, 127,357 after: bistable test).
**bwstream -u4 -b32: rtlsim 38,956 / 140,758 / 548,386 at n = 128 / 512 /
2048; SimX (602af5c84) 38,539 / 126,128 / 419,474; SimX f55faaa2d at n=2048
449,189. SimX is 18-24% fast and the gap grows with stream length. Not
root-caused: top priority.** Still running on rover when this was written:
gemm_block (SimX 562,462 after / 574,100 before), gemm_tmem_decode
(1,320,528 / 1,319,938); requested next: smprobe -k0 -n2 -i20 -d5000 with
-DPATH_STATS (SimX 77,636 before the lane fixes, 89,793 after; decides
whether fix (a) moved toward the RTL with the SM full), then -i40, -k1 -i80,
-d2000 variants; bwstream n=1024 rtlsim and SimX f55faaa2d at the three sizes.
Check with `vortex-exp/rover/rover_status.sh`, fetch with `rover_fetch.sh`.

### H200 measurements today (results_gpu/h200_smr_summary.txt, h200_brcap_summary.txt)
One CTA, 4-byte shared memory: 1.000 stores and 1.000 loads per clock at
8/16/32 warps (one warp 4.06 / 5.05 cycles). L1-resident loads: 128 B per
clock (two lines) regardless of width. TMA: no engine plateau up to 16
outstanding transfers, one ceiling of 105-108 B/clock, one transfer streams
1.67 lines/clock after ~340 cycles; issue cost 104 cycles bare, 290-302 full
loop. Branch rate with the SM full depends on code size (1.58 branches/clock
at 1.6 KB, 1.08 at 12.4 KB, 32 warps). Two CTAs of 32 warps on one SM: the
older takes 99.7%, so earlier "64-warp" part figures are 32-warp figures.

### Unmerged, with state
- `feature/noc` 2910aa910 (contains f55faaa2d): steps 0-1; ungated-groups gap
  fixed (L2 bank count no longer follows the group's lanes); open: DXA +
  limiter (+4.0 added) and DXA + groups + gate (-2.6) at the lane shape; the
  last commits are not independently reviewed. Tables: notes/nocb/fin5_res.txt.
- `feature/barvis-parity` b7d1f6953: ALU result switch alternation + integer
  unit hold + warp_rate mul_add kind and parity case; re-review verdict "land
  after one fix" = the XLEN issue below.
- `feature/sm-rate` (12 commits, experiments; notes/sm-rate.md): LMEM store
  diagnosis confirmed on rtlsim (4.3 cycles per store, 12.2 with 4-byte
  spacing; SimX within 0.1%). Implementation of fixes 1/2/7 NOT started.
  bwstream_dxa kernel argument lacks __UNIFORM__ (one CTA 26.8 -> 32.8
  B/clock when fixed); other kernels not scanned yet.
- `feature/lsu-accept`, `wce-cexp`: item E (held-path pop under back-pressure)
  in progress; item C found to be core-wide (every unit accepts one cycle late
  and returns one cycle early; fix = core slot-queue delay 2 -> 1 plus the
  commit arbiter's output register): finding only, no commit.
- Parked: `perf/mt3`, `perf/build2`.

### Defect found in our own CI cases
`ci/testcase.py:113-119` pins every model_parity / perf_gate case to XLEN 32;
the `xlen: [64]` key on the ~22 parity cases this work added is ignored, and
they were only ever measured at XLEN 64 (no 32-bit toolchain here). Decision:
make the planner honour an explicit xlen key (unkeyed cases stay at 32), with
a CHANGELOG correction. Not done yet. Upstream validates SimX timing at RV32
only; this study runs RV64.

### Other open items
smprobe -n2 at 64 warps moved +12-15% with fix (a) (steady, scales with
occupancy; suspected: barrier drain waits on a deeper LSU input) and
gemm_block / gemm_tmem moved -6.1% / -4.6% at full problem size only: both
provisional until the rover references land. DRAM boundary delay 3 vs 2 (D),
page walk (F), front-end start-up divergence at 64 warps with VM, DXA
half-row beats (SimX writes two per cycle 13% of the time; RTL one write
bus). Phoenix CPU account gts-hkim358 nearly spent: use ICE and rover.

## Update 2026-11-23 (01:45 EST): state before a context compaction

Work branch: pushed bd7aa26ba; local head a470ffa5c (= bd7aa26ba + 88077548c
h200s sets LMEM_WR_MERGE=1 and LMEM_WR_ROW=1 + merge of feature/sm-rate-f).
Not pushed yet: waiting for the knob-on SimX baselines (rover points ckv_*,
source copy vlmw, models slmw_*).
Merged since the 17:25 update: dd94ad142 (ALU result switch alternation +
integer hold; ci/testcase.py honours an explicit xlen key on parity cases),
bd7aa26ba (LMEM store knobs, default off; reviewed twice, LR/SC reservation
bug under WR_ROW found by review and fixed).

### Rules corrected by the user (also in memory)
- There is NO "never faster than the part" rule: match the part, either sign
  is the same kind of error.
- Delegate to Codex first to save Claude tokens. The plugin's Codex cannot run
  a local shell here (bwrap cannot create the Slurm scratch tmp dir); the user
  starts Codex by hand in the repo and tells it to read a prompt file under
  vortex-exp/codex-tasks/. Codex's 5-hour limit is tight: one task at a time.
- rover: nobody else uses it; use it freely (rover_point.sh: 2 rtlsim build
  slots, 6 SimX). H200 GPU jobs are not rationed.

### The big rtlsim/SimX gaps at h200s size (found on rover)
1. Fetch ahead (VX_CFG_FETCH_INFLIGHT=4): the "all ibuffers full, fetch one
   more" path never switches off at depth > 1, so warps chain behind the
   slowest load. bwstream -u4 -b32, rtlsim / SimX: n=512 depth 4 140,758 /
   126,128, depth 1 102,205 / 104,270; n=1024 depth 4 211,928 / 208,994;
   n=2048 depth 4 548,386 / 419,474, depth 1 395,381 / 405,204 (two-level
   behaviour: the chain forms or not). gemm_tmem_decode -m16 -n8192 -k4096:
   rtlsim 1,554,320, SimX 1,320,528 (-15%); gemm_block 608,849 / 576,342.
   Fix = pick a warp only while its credit < IBUF_SIZE (WIP 871067f3f on
   feature/fetch-credit, worktree wt_fcred; note notes/fetch-credit.md).
   SimX with the fix: n=512 104,279, n=2048 406,539. Full-size rtlsim of the
   fix on rover: source vfcred, model mfcred_h200s, points fcr_r_n512,
   fcr2_r_n2048, fcr2_r_smp (+ SimX twins). Codex (started by the user,
   prompt codex-tasks/fetch-credit.txt) is finishing it: step 1 review done,
   step 2 collecting results; it appends "Codex progress" to the note.
   Decode-kernel depth pair still running on rover: gtq_r_fi4 / gtq_r_fi1
   (-k1024; SimX 348,388 / 363,388).
2. Barrier beside loading neighbours (smprobe, 64 warps): probe iteration
   p50 rtlsim 485 (k0 i20) and 658 (k1 i80) against SimX 2,282 and ~1,350;
   total cycles rtlsim 50,477 / 90,995 / 147,024 / 85,145 against SimX
   89,793 / 160,587 / 265,536 / 154,184 (instruction counts differ: poll
   loops). Not the front end (RTL fetch stall 4%; SimX depth 1 p50 1,332;
   rtlsim depth 1 p50 500). SimX with BAR_DRAIN_SCOPE=0: p50 9,580
   (294,648 cycles); the rtlsim scope-0 model m602_h200s_bs0 was building
   (point spb0_r_k0). Cause not found; suspect what SimX counts as "queued in
   the LSU" for scope 1. Unassigned (a Codex task was planned).
### LMEM
Stores: knobs merged; h200s sets them locally. H200 (one CTA, 4-byte):
stores and loads share ONE row per clock (sum 1.000; 0.4 + 0.6 interleaved);
per block about 0.25 stores or 0.5 loads per clock; TMA writes barely touch
loads but contend with stores (results_gpu/h200_smr_summary.txt, Job 2).
Read side + fairness (knob VX_CFG_LMEM_RD_ROW, rotating grant, design note in
notes/sm-rate.md): approved, NOT implemented (agent stopped for token budget;
branch feature/lmem-rd may hold WIP).
### Other open
feature/noc 2910aa910 needs an independent review before merge (the Codex
review failed for lack of a local shell). Pre-existing: rtlsim lmem_access
with EXT_A at 16x32 fails its LR/SC count (details in notes/sm-rate.md).
C/E/D/F one-cycle items parked. Profile text for LANE_DISPATCH_GAPLESS still
says "accepted overshoot": reword to a plain gap. Findings lists / study/ need
a sync with everything since 2026-11-22 17:00.

## Update 2026-11-23 (03:00 EST)

Pushed head of `feature/dxa-borrowing` on the fork: `5f855c2ae`.

Merged since the 01:45 update
- `88077548c` h200s turns the shared-memory store knobs on. Knob-on SimX baselines (rover, source `vlmw`):
  gemm_block 576,342, gemm_tmem_decode 1,320,528, fetch_squash 10,018, warp_rate 113,382, tmem_roof 62,075,
  war_order 342,988, bwstream -n2048 452,761 (all unchanged); bar_vis 127,357 -> 148,003.
- `feature/fetch-credit` (Codex finished it: 4e95ed5c4, 4829b9222, 180e49dcb; merge 5f855c2ae). rtlsim h200s
  bwstream -n512 depth 4: 140,758 -> 102,481 (depth 1: 102,205; SimX 104,279). n=2048 rtlsim (`fcr3_r_n2048`)
  still running at the time of writing; expected about 395,000. h200s baselines after this merge not re-taken yet.
- Findings 51-56 and measurement notes M3-M4 in both language lists; study/ mirrored (66ad11f49, fd151ee95).

User decisions today
- No reviewer chains: I look over each change once myself and merge. A separate reviewer only for large
  changes (NoC).
- Shared-memory loads (`VX_CFG_LMEM_RD_ROW`, branch `feature/lmem-rd`): to be merged, not parked.
- Per-SM memory rate: option A2 for the DXA engine (two lines per cycle on the memory side, a full 128 B
  shared-memory row per cycle, workers take a line every cycle; 16 workers is our choice, not a part value)
  and B2 for plain loads (dcache word 64 B + response queue 16). The issue-loop cost (610 cycles per tile
  against 290-302) is not in A2.
- The decode-kernel gap is to be fixed.

Running
- Agent `dxa-rate` (Opus): A2 on branch `feature/dxa-rate`, notes in `dxa-rate.md`, rover prefix `dxr_`.
- Agent `sm-rate`: finishing `feature/lmem-rd` for merge (in-flight checks, why lmem_access is 6.5% slower
  with RD_ROW, commit series, h200s enable as a separate last commit). Interim: 4-byte loads 0.49 -> 0.79
  per clock; mixed store/load no longer starves (64 warps 10,471 / 26,848 -> 10,476 / 10,472 cycles);
  gemm_block 576,342 -> 540,149.
- B2, mine: rover SimX points `b2a_*` (base), `b2b_*` (word 64), `b2c_*` (word 64 + CRSQ 16), source `vlmw`,
  kernels ldl/ldp (ldstream -t2048 -b8 -u8 -m16 -a [-l]), bw, gb, dec, roof, war, wr. Next: rtlsim check, then
  the h200s profile.
- Barrier gap (finding 56), mine: SimX counters (`tmp/bar-probe` 75a6fe4f0, rover `bp_s_k0`): of 113,490
  blocked barrier evaluations, 103,029 are "an LSU block's request queue is not empty", 3,373 dcache
  adapter/coalescer, 2,668 own pending instructions, 1,148 dcache request out, 952 local-memory store. The
  queue rules read the same in both models (reads enter only with room in the pending table; leave when
  memory accepts), so the neighbours' loads leave more slowly in SimX. rtlsim at scope 0 (`spb0_r_k0`) running.
- NoC review by Codex: cut off by its limit after 7 minutes; `noc-review-codex.md` has 9 lines, verdict
  pending. It notes the author's +4.0% / -2.6% do not match fin5_res.txt (+5.94% / -0.75%). Resume with
  `codex-tasks/noc-review.txt` after the reset (about 06:20 EST).

New open item
- Decode kernel, SimX faster than RTL at either fetch depth (h200s, gemm_tmem_decode -k1024): depth 4
  rtlsim 401,699 / SimX 348,388; depth 1 404,210 / 363,388. Scoreboard stalls 21% / 23% and 12% / 11%. No owner
  yet; mine after the barrier.

Branches not merged: feature/lmem-rd (in progress), feature/dxa-rate (in progress), feature/noc (+ noc-apps,
noc-kmu; review pending), feature/lsu-accept / wce-cexp / wce-tb (parked one-cycle items C/E), perf/mt3 and
perf/build2 (parked, not to be merged), tmp/bar-probe (throwaway). Merged branches deleted: barvis-parity,
fetch-credit, sm-rate, sm-rate-f.

rover script changes today: `rover_point.sh` takes a per-tree build lock (two points sharing a MODEL raced
on the runtime build); use PERFCLASS, not PERF, to reuse a model; pass `</dev/null` to rover_start.sh inside
a read loop.

## Update 2026-11-23 (06:30 EST)

- fetch-credit: n=2048 rtlsim 395,176 (depth 1: 395,381; SimX 406,539). Done.
- Barrier gap (finding 56) narrowed: rtlsim scope 0 = 800,681 cycles (SimX 294,648), the sign flips. Runs separating
  barrier and loads (rover `spr_r_*`, `spq_s_*`; table in codex-tasks/barrier-gap.txt): the models agree within 1% when
  the neighbours only load, and disagree whenever the neighbours load AND take a barrier, even with no barrier in the
  probe. A loading warp's own barrier costs more in the RTL. Next: small-configuration repro and trace
  (codex-tasks/barrier-gap.txt).
- B2: dcache word 64 with ONE channel per block slows gemm_block +18% and war_order +23% (coalescer misses); with TWO
  channels the loss is gone. Agent `dc-chan`, branch feature/dcache-channels 879f87625 (knob VX_CFG_DCACHE_CHANNELS):
  h200s SimX ldstream -l 244,813 -> 187,314 (181,614 with CRSQ 16), others unchanged; parity 0.1%; Beacon: the channel
  costs +7.9k LUT / +13.9k FF; a 64 B AMO unit is 15k LUT per bank against 3.3k (open: narrow it). h200s rtlsim pending.
- A2: agent `dxa-rate`, branch feature/dxa-rate 319a8c4e3: 51.1 -> 65.0, 53.9 -> 72.4, 49.6 -> 70.1 B/clk, decode 51.4 ->
  58.9; h200s rtlsim 27% faster, parity gap 12.5% -> 7.1%. LINE_PER_CYCLE slower (off). Asked before merge: make the SimX
  shared-port arbiter transparent (bwstream 452,761 -> 416,774 in SimX only), single-transfer rate, co-run with plain
  loads. 4 ports give 88.9 B/clk on 7 CTAs x D2 (option for the user). Pre-existing: RTL addr_gen checks OOB on outer
  dims only.
- feature/lmem-rd 50b830511: 26 planner cases pass, identity holds; with LANE_DISPATCH_GAPLESS=3 loads/stores reach
  0.98 per clock. h200s rtlsim build with RD_ROW died at 211 GB on rover three times; cause being isolated on ICE.
  dxa-rate built an h200s rtlsim model from fd151ee95 (store knobs on) without trouble, so RD_ROW is the suspect.
- Codex resets 06:19 EST: tasks ready in codex-tasks/: noc-review.txt (resume), barrier-gap.txt.

## Update 2026-11-23 (07:15 EST)
Pushed head 2e5c1c09a. Merged today after the 06:30 note: feature/dcache-channels (8f0fcea63; h200s: word 64, 2 channels),
728d50414 (h200s DCACHE_CRSQ_SIZE=16), feature/lmem-rd (2e5c1c09a; LMEM_RD_ROW default off, off in h200s).
h200s SimX baselines at 2e5c1c09a (rover `h8_s_*`, `m2_*`): fetch_squash 10,018; warp_rate 113,382; tmem_roof -n512 -i8
62,072; bwstream -n2048 -u4 -b32 403,996; gemm_block 574,021; gemm_tmem_decode 1,317,208; war_order 321,423;
ldstream lean 181,614; ldstream 315,885; lmem_access 7,917,484.
h200s rtlsim at 64 B / 2 channels without CRSQ (dc-chan): ldstream lean 172,345 (SimX 187,314, -8.0%), war_order 329,894
(SimX 343,220). With CRSQ 16: rover `h8_r_war`, `h8_r_ldl` running (model mh8_h200s).
The "211 GB build" alarm was wrong: rover's mem.log is whole-machine; per-job peaks on ICE are 63-85 GB in every state.
Pending from sm-rate: h200s rtlsim numbers with RD_ROW (ICE), why lmem_access is slower with RD_ROW, before/after table
for enabling RD_ROW and LANE_DISPATCH_GAPLESS=3 in h200s (user decides). Pending from dxa-rate: transparent SimX port
arbiter, single-transfer rate, co-run with plain loads; then merge feature/dxa-rate.

## Update 2026-11-23 (08:15 EST)
Merged feature/dxa-rate (07a68b311): DXA_GMEM_PORTS honoured in RTL, DXA_LMEM_ROW, DXA_LINE_PER_CYCLE (off); h200s: 16
workers, 2 ports, row writes. Checked at the merged head (rover `md_*`, `mdq_*`): h200s SimX fetch_squash 10,018,
warp_rate 113,382, tmem_roof 62,072, bwstream 403,996, war_order 321,423, ldstream lean 181,614, lmem_access 7,917,484
(all as before the merge); gemm_block 574,021 -> 549,551; gemm_tmem_decode 1,317,208 -> 1,151,794; bwstream_dxa
-e4096 -n256 -d4 191,936, -d2 404,284, -e2048 -n512 -d4 483,968 (all pass). Small shape with the DXA, store and load
knobs together (8 warps x 16 threads, IW 4, dxa_rowmix -x8 -r16 -g8): rtlsim 245,108 / SimX 245,592; with RD_ROW
245,109 / 245,465.
Open from dxa-rate: one transfer alone moves 10.5 B/clk (256 lines in 1,145 cycles) against the part's 1.67 lines per
clock; needs a transfer split over workers or two lines per beat. Co-run (dxa_corun, 8 copy CTAs beside 20 load CTAs,
h200s SimX): copy 47,426 cycles alone, 384,963 beside the loads; loads 338,253 -> 340,402. Same with the knobs off. Not
measured on the part. GMEM_PORTS=4: 50.3 / 82.3 / 58.0 B/clk (not adopted). Pre-existing: RTL addr_gen checks OOB on
outer dims only.
