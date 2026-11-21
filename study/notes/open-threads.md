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
DiT 1.7% of memory bandwidth, proportional to SMs; decode matmul kernels 65-68%, 72% of speed on 20% of SMs, ~95-125 GB/s per SM with few SMs. Decode wall-clock in HF transformers is host-bound.

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
