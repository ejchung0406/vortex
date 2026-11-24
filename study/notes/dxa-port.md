# dxa-port: why one DXA memory port passes ~0.56 line per clock (running log)

Agent `dxa-port`, started 2026-11-23. Branch `fix/dxa-port` (worktree `vortex-exp/wt_dxaport`, base c0cf5eec1).
Question (lead): on h200s (DXA_GMEM_PORTS=2, LMEM_ROW=1, 16 workers) each port passes ~0.56 line/clk while ~5
workers wait at it. Name the limiting stage in both models; fix if defect/setting, else report the structure.

## Path of port i (SimX, from the instantiations)
worker -> DxaCore gmem-arb i (MemArbiter RR, req delay 1) -> gmem-out i (RegSlice, under VM) -> unit MMU lane
DXA+i (ReqIn 3 deep, registered ready) -> port 0: socket mem_arb0 (priority icache, dcache, DXA) + breq0 RegSlice;
port 1: breq1 RegMerge (dcache first) -> l2link-req i (DelayBuffer, L2_LINK_LATENCY 94) -> L2 CachePortArb input i
-> L2 bank. Responses: bank -> core_xbar -> l2link-rsp i -> brsp (RegSlice / RegSplit) -> unit MMU RspOut ->
gmem-arb i RspIn -> worker RspOut i (HOL: one response FIFO per port).

## Log
- Helpers: slurm/dxp/mksrc.sh (frozen copy src_dxp_<name>), slurm/dxp/stats_patch{,2..8}.py (temporary DXP_STATS
  counters, not for commit), slurm/dxp/lbuild.sh + lrun.sh (local h200s SimX in the holder job: rover's build lock
  was held by others' rtlsim builds for 45+ min). rover src dxp_base = c0cf5eec1, dxp_st1 = counters.
- Base (c0cf5eec1) bwstream_dxa -e4096 -n256 -d2 -c0 = 399,917 cycles (lead's 407,209 was 636d15961).
- 11-23 counters, 7xD2, base (393,698 active cycles; per port 0/1):
    lines sent                     220,928 / 237,824  (0.56 / 0.60 per active clk)
    cycles a worker offers         284,217 / 302,593  (72% / 77%) -> 0.78 line per offered clk
    offered but not sent           ~63K / ~65K: unit MMU lane's ReqOut full (67,793 / 72,259) = socket boundary
                                   stage out of its 130 credits (67,861 / 72,266 cycles): the L2 link request queue
                                   holds 114 avg (residency 159 clk vs latency 94) because the L2 port takes slowly
    L2 bank pipe stalls            632,839 bank-clk, ALL with a Replay at the head whose core_rsp_out is full
                                   (crsq 2 slots; when any bank has a response for port 0, 3.6 do: one output/port)
    no worker offers               ~110K / ~91K cycles (28% / 23%)
    transfers                      span 2,359 = issue phase 1,533 + tail 826; 10.74 on workers of 14, 3.26 in the
                                   CTA's turnaround (~716 clk each: barrier wake, consume, 2nd barrier, issue)
  Response side not binding by itself: brsp credits 2 -> 4 (exp x1) 406,305 (no gain).
- L2_CRSQ_SIZE=32 (CONFIGS): 7xD2 382,048 (-4.5%), bank stalls 3; port sends 229K in 232K offered clk (0.98)
  but offered only 68%. 3x4 (-d4) 188,827 -> 177,765 (-5.9%).
- Priority (not RR) worker arbiter per port (stagger transfers): 7xD2 497,986 (+25%), 3x4 228,354 (+21%): a
  single stream's responses belong to one worker, whose capture (1 line / 2 clk) HOL-blocks the port's response
  FIFO (gmem-arb rsp blocked 38% of rsp cycles), which backs up into L2 bank stalls (2.78M) and request blocking.
- Variants (7xD2 / 3x4 cycles): LPC 404,980 / 234,931; LPC+CRSQ32 379,849 / 194,216; prio+LPC+CRSQ32 422,067.
- Issuing workers on port 0 per active cycle, 7xD2: 0: 26%, 1-2: 7%, 3-6: 43%, 7+: 24% (bimodal: RR-shared
  transfers end their issue phase together and then wait out their tails together).
- Where the CTAs are (bwstream_dxa: each CTA is ONE warp; occupancy 3.0 on 3x4): at each new DXA request, the
  CTA's older transfers still unfinished: 3x4 base avg 1.83 (0:8 1:238 2:397 3:125); 3x4 LPC 0.59 (0:321 1:438
  2:9); 7xD2 base 0.66 (0:603 1:1189). Per-CTA issue interval 3x4: 720 clk base, 904 LPC; finish of the CTA's
  latest transfer -> its next request 423 / 475 clk. So with LPC the DXA finishes transfers sooner (span 1,744 ->
  1,122; issue phase 768 -> 313) but the one-warp CTA loop becomes the bound and itself slows: core load_lat
  53 -> 93 (PERF class 1), scrb stalls 52% -> 61%. Probable cause: shared-memory reads wait behind the DXA's
  denser LMEM DMA writes (a DMA access takes the banks, no core pass that cycle: local_mem.cpp ~774-823,
  VX_lmem_unit.sv:856 pass_en = ~dma_valid). Not confirmed by an LMEM counter (my LocalMem counter saw no core
  pass pending; the consume loads may take another path) - OPEN.
- Named stage of the request-side blocking, both models (structure, not a defect): the L2 bank stalls its whole
  pipeline, input included, while its core-response queue is full. SimX cache.cpp:2173 (Replay: core_rsp_out
  full -> return), :1178/:1191 (pipe_stall gates processInputs); queue = 2 + L2_CRSQ_SIZE slots (:3547). RTL
  VX_cache_bank.sv:1439 crsp_queue_stall, :348 pipe_stall, :444 core_req_ready &= ~pipe_stall; CRSQ_QUEUE_SIZE
  :114. L2_CRSQ_SIZE = 0 (VX_config.toml:440), h200s does not set it (DCACHE_CRSQ_SIZE 16 is set).
- Clean base c0cf5eec1 h200s SimX (local build /tmp/echung67/dxp/base), cycles and B/clk, then with
  -DVX_CFG_L2_CRSQ_SIZE=8: dx7d2 (-e4096 -n256 -d2) 399,917 73.4 -> 383,306 76.6; dx3 (-d4) 188,827 66.6 ->
  181,877 69.2; e2k (-e2048 -n512 -d4) 412,429 71.2 -> 431,480 68.0 (worse); bwstream -n2048 -u4 -b32 405,591 ->
  405,591. Mixed, and the part's L2 response buffering is unknown: NOT applied (value departing from the part
  needs the user). Reported as an option.
- No rtlsim point: the named stage is the same structure in both models by reading; an rtlsim run gives no
  per-stage counters and the build costs 45 min / 80 GB.
- CONCLUSION (11-23): no stage of the port path passes one line per two cycles, and there is no defect to fix.
  The time-average ~0.56 = 72% of cycles with a line offered x 0.78. Of the offered cycles, 22% are lost to L2 bank
  pipe stalls on a full 2-slot core-response queue, which back up the port's head and the link. In the other 28%
  of cycles no worker of the port is in its issue phase (synchronized RR issue phases, ~830-clk tails, one-warp
  CTAs with depth 2-4 and ~420 clk from a transfer's end to the CTA's next request). LINE_PER_CYCLE: the DXA side
  gets faster, the CTA loop becomes the bound and slows (core load latency 53 -> 93).
- 11-23 (lead asked: does the L2 replacement defect explain part of it?): base dx7d2, PERF class 5: L2 reqs 460,546,
  read misses 458,849 (= the 458,752 DXA lines + a few: each line misses exactly once, compulsory: a 29.4 MB source
  read once through an 8.65 MB L2), write misses 858, evictions 584 in the whole run (0.0013 per DXA line), no
  dirty writebacks of DXA data. No refetches: the replacement policy cannot move this shape; the 0.56 is not from it.
