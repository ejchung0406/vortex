# SimX gains less than the RTL from the 64 B dcache word with two channels

Task: vortex-exp/codex-tasks/dcache-ch2-gap.txt. Branch fix/dcache-ch2-simx
from c0cf5eec1 (feature/dxa-borrowing), worktree vortex-exp/wt_dcg. rover
source copies and points use the prefix dcg_.

## 2026-11-23 setup

- rover source dcg_base = c0cf5eec1 (= ccd8d3974 + study files).
- Differences between the agreeing parity case and h200s: ISSUE_WIDTH 4
  (8 dcache ports, 4 banks: registered request crossbar instead of the
  2-port combinational one), 32 threads on 8 LSU lanes (4 packets per
  instruction), 64 warps, FWD_HOLD 1, CRSQ 16, MSHR/pending 512, dcache
  256 KB, HBM3E memory, VM.
- First sweep (ldstream -b8 -u8 -m2 -a -l, XLEN 64, word 64, 2 ch, 8 lanes,
  4 banks, FWD_HOLD 1, CRSQ 16, MSHR/pending 512, 256 KB, IW 4, 16 warps):
  c1 = 8 threads (-t128), c2 = 32 threads (-t512).

## 2026-11-23 smallest configuration with the gap

ldstream -b8 -u8 -a -l, XLEN 64, no profile, all with word 64 / 2 ch / 8 LSU
lanes / 4 banks / FWD_HOLD 1 / CRSQ 16 / MSHR and LSU pending 512 / 256 KB /
ISSUE_WIDTH 4 / 32 threads. "mem" = L2 (2 MB, 16 banks, link 94, line 128) +
the h200s HBM3E platform; "vm" = VM_ENABLE, DTLB 256, L2_TLB_MEGA 256;
"core" = the h200s core knobs (LANE_DISPATCH_GAPLESS 3, FETCH_INFLIGHT 4,
BRANCH_REG/FETCH_REQ_BUF/FLUSH_REQ_BUF/OPC_MERGE_BUF 0, GPR banks 8, dispatch
queue 8, ALU lanes 16, fetch width 4, ...). rtlsim / SimX cycles:

| point | shape | rtlsim | SimX | SimX vs rtlsim |
|---|---|---|---|---|
| dcg_c1 | 16 warps x 8 thr, default memory, -t128 -m2 | 83,283 | 82,913 | -0.4% |
| dcg_c2 | 16 warps x 32 thr, default memory, -t512 -m2 | 52,583 | 52,772 | +0.4% |
| dcg_c3 | c2 + mem, -m4 | 83,038 | 82,443 | -0.7% |
| dcg_c4 | 64 warps + mem, -t2048 -m4 | 54,861 | 56,234 | +2.5% |
| dcg_c5 | c4 at -m16 (manual run in the c4 trees) | 177,830 | 180,495 | +1.5% |
| dcg_c6 | c5 + vm | 174,955 | 181,571 | +3.8% |
| dcg_c7 | c5 + core | 173,751 | 174,886 | +0.6% |
| dcg_c8 | c5 + vm + core | 168,660 | 178,126 | +5.6% |
| dcg_c9 | 16 warps + mem + vm, -t512 -m4 | 85,223 | 85,348 | +0.1% |
| h200s (task) | profile, CRSQ 16 | 168,615 | 182,466 | +8.2% |

Instructions equal everywhere. c8 is the reproducer (rtlsim build 11 min,
run 4 min). At c6 the dcache counters agree (reqs 320,261 / 321,818, bank
stalls 129,601 / 128,919): the same number of merged bank accesses. Load
latency (core class) rtlsim 670, SimX 753; memory latency rtlsim 156, SimX
99: SimX holds requests longer before memory.

## 2026-11-23 where the time goes (probe, c8)

Temporary probe (source copy dcg_probe on rover; not committed): per
interface the count of read-request fires and response fires and the sum of
their cycles, so (sum rsp - sum req) / n is the mean residency. RTL: $display
in final blocks under DCG_PROBE (VX_socket, VX_cache_bank); SimX: channel
tx/pop callbacks. Cycles per read, c8:

| segment | rtlsim | SimX |
|---|---|---|
| LSU request -> dcache accept | 3.6 | 3.8 |
| dcache accept -> dcache response | 558 | 751 |
| dcache miss request -> fill accepted by the bank | 511 | 686 (317 to arrive + 370 queued at the bank) |
| bank response queue (CRSQ) in -> out | 40 | 57 |

The same counts in both (538,116 port reads, 266,245 misses, 307-310 k bank
reads). The extra SimX time is fills waiting for the bank, and the bank
holds fills because its response queue is full (FWD_HOLD): the response
path out of the dcache drains slower in SimX.

## 2026-11-23 21:40-23:40 rover unreachable

ssh to rover closes the connection after the key exchange (host answers
ping) from about 21:40. Runs dcg_c8r_* (multicast and LSU-side probes) were
in flight; results to be read when it is back. SimX c8r (multicast, per
bank lane): lane head present ~165 k cycles for ~77 k responses per lane
(2.1 cycles each), a merged response taken by one port and not the other
~17 k cycles per lane; per port ~84 k cycles with a response owed, ~17 k of
them blocked by a full downstream (port 1: 120 k owed, 52 k blocked). RTL
(c8p) response stalls at the dcache ports: port 1 61 k, port 0 22 k, others
~9 k cycles.

## 2026-11-24 01:20 paused: rover still unreachable

The probe (not for commit) is saved as notes/dcache-ch2-gap.probe.patch and
is applied, uncommitted, in wt_dcg; rover source dcg_probe has it too (the
LSU-side part, VX_lsu_mem_path / mem_coalescer, is not yet on rover).
Next: read dcg_c8r_{simx,rtlsim} (rover results) and compare per bank lane
head time and per port blocked cycles between the models; then the LSU-side
probe. Config strings: scratchpad dcg_cfg.sh contents are COMMON + MEM +
NUM_WARPS=64 + VM + CORE as in the table above.

c8 XCONF (rover points dcg_c8_*, ldstream -t2048 -b8 -u8 -m16 -a -l, VORTEX_VM_MEGAPAGE_HEAP=1):

    -DVX_DBG_STALL_TIMEOUT=20000000 -DVX_CFG_DCACHE_WORD_SIZE=64 -DVX_CFG_DCACHE_CHANNELS=2 
-DVX_CFG_NUM_LSU_LANES=8 -DVX_CFG_DCACHE_NUM_BANKS=4 -DVX_CFG_DCACHE_FWD_HOLD=1 
-DVX_CFG_DCACHE_CRSQ_SIZE=16 -DVX_CFG_DCACHE_MSHR_SIZE=512 -DVX_CFG_LSU_PENDING_SIZE=512 
-DVX_CFG_LSU_PENDING_LIMIT=512 -DVX_CFG_DCACHE_SIZE=262144 -DVX_CFG_ISSUE_WIDTH=4 
-DVX_CFG_NUM_THREADS=32 -DVX_CFG_L2_ENABLE -DVX_CFG_L2_LINK_LATENCY=94 -DVX_CFG_L2_LINE_SIZE=128 
-DVX_CFG_L2_NUM_BANKS=16 -DVX_CFG_L2_MSHR_SIZE=256 -DVX_CFG_L2_SIZE=2097152 
-DVX_CFG_L2_REPL_POLICY=2 -DVX_CFG_PLATFORM_MEMORY_TYPE=HBM3E -DVX_CFG_PLATFORM_MEMORY_NUM_BANKS=16 
-DVX_CFG_PLATFORM_MEMORY_DATA_RATE=6250 -DVX_CFG_PLATFORM_MEMORY_PEAK_BW=800000 
-DVX_CFG_PLATFORM_MEMORY_BANK_GROUPS=8 -DVX_CFG_PLATFORM_MEMORY_SCHED_WINDOW=256 
-DVX_CFG_PLATFORM_MEMORY_INTERLEAVE_SIZE=1024 -DVX_CFG_PLATFORM_CLOCK_RATE=1980 
-DVX_CFG_NUM_WARPS=64 -DVX_CFG_VM_ENABLE -DVX_CFG_DTLB_SIZE=256 -DVX_CFG_L2_TLB_MEGA_SIZE=256 
-DVX_CFG_LANE_DISPATCH_GAPLESS=3 -DVX_CFG_FETCH_INFLIGHT=4 -DVX_CFG_FFLAGS_ACCUM=1 
-DVX_CFG_BRANCH_REG=0 -DVX_CFG_FETCH_REQ_BUF=0 -DVX_CFG_FLUSH_REQ_BUF=0 -DVX_CFG_OPC_MERGE_BUF=0 
-DVX_CFG_NUM_GPR_BANKS=8 -DVX_CFG_DISPATCH_QUEUE_SIZE=8 -DVX_CFG_FPU_QUEUE_SIZE=16 
-DVX_CFG_BAR_DRAIN_SCOPE=1 -DVX_CFG_NUM_ALU_LANES=16 -DVX_CFG_FETCH_WIDTH=4

## 2026-11-24 01:40 cause found (SimX runs on the Phoenix session node)

rover still down, ICE scratch full (99.5% of its file quota), so SimX runs
now go to the local session node (/tmp/echung67, vortex-tools). Local SimX c8
reproduces rover: 178,126.

The dMMU response lane. RTL: VX_mmu's response path has no stage for the
dMMU (RSP_OUT_BUF=0; hw/rtl/vm/VX_mmu.sv:117-128 VX_mem_bus_slice passes
rsp through, :570-576 mux only for injected kill/fault responses), so the
dcache's per-port output buffer drives the LSU adapter's pack directly.
SimX: sim/simx/mem/mmu.cpp:292-321 moves each response from RspIn (a
2-entry queue, mmu.cpp:31) to RspOut one tick at a time, even with
rsp_reg=false (dcache and icache MMUs, core.cpp:310-320): a queue and a hop
the RTL does not have, so the coalescer's two channel inputs are fed through
two independent extra queues and the dcache's response ports see the MMU's
queue instead of the coalescer's. Experiment: bind the dcache's response port
straight to the coalescer (bypass the hop): c8 SimX 178,126 -> 170,659
(rtlsim 168,660; +5.6% -> +1.2%).

Fix: with rsp_reg=false the MMU binds RspIn to RspOut (a wire) and sends
only its kill/fault responses itself.

## 2026-11-24 02:30 fix committed (9d7b5d3fb on fix/dcache-ch2-simx)

SimX, h200s, local session node, before (c0cf5eec1) -> after (9d7b5d3fb);
rtlsim references from the task (ccd8d3974; not re-run, rover down):

| case | SimX before | SimX after | rtlsim | before | after |
|---|---|---|---|---|---|
| ldstream -l, profile shape (64 B, 2 ch, CRSQ 16) | 182,466 | 173,926 | 168,615 | +8.2% | +3.2% |
| ldstream -l, old shape (32 B, 2 ch, CRSQ 0) | 245,673 | 252,912 | 252,467 | -2.7% | +0.2% |
| war_order | 320,655 | 320,317 | 321,222 | -0.2% | -0.3% |
| bwstream -n2048 -u4 -b32 | 405,591 | 394,442 | ~395,000 | +2.7% | -0.1% |
| gemm_block -m16 -n1024 -k4096 -s1 | 515,695 | 518,282 | 555,376 | -7.1% | -6.7% |

Instructions equal before and after. c8 (no profile): 178,126 -> 170,659
(rtlsim 168,660). VM parity apps (SimX, xlen 64) move <= 1.3%.
The RTL gains 33% from the 64 B word, SimX now 31% (was 26%).
Open: ldstream +3.2% and gemm_block -6.7% (the L2 replacement defect on
fix/smem-phase took ldstream 8.2% -> 6.9% alone; not combined yet).

## 2026-11-24 02:40 parity case pending RTL

Candidates (SimX before / after the fix, rtlsim not yet run; rover down):
- 16 warps x 32 thr, 8 lanes, IW 4, word 64 / 2 ch, VM, LANE_DISPATCH_GAPLESS 3,
  ldstream -t512 -b8 -u8 -m1 -l: 24,616 / 25,806 (+4.8%)
- same with 16 thr: 28,830 / 28,103; with 8 thr (-t128): 47,412 / 47,286
- 8 thr, 16 warps, IW 4, VM, L2-mode hits (-w0.0078125 -p16): 15,993 / 16,142
The case will need rtlsim on the first candidate (and a tolerance tighter
than 5%, e.g. 0.02, if the gap before is between 2% and 5%).
Local trees: vortex-exp/src_dcg_base (c0cf5eec1), src_dcg_fix (9d7b5d3fb),
src_dcg_probe (probe; breaks 2-port configs, its bank pop callback replaces
the arbiter's); helper scripts in the session scratchpad.

## 2026-11-24 parity case and small-shape check (rtlsim on ICE, tree src_dcg_fix)

| shape | rtlsim | SimX before | SimX after |
|---|---|---|---|
| c8 at -m4 (= parity-dcache-ch2-ldstream-vm) | 52,418 | 55,434 (+5.8%) | 53,609 (+2.3%) |
| K3: 16w x 32t, 8 lanes, IW4, VM, gapless 3, -t512 -m1 | 25,061 | 24,616 | 25,806 |
| K3 without VM | 27,514 | 24,109 | (same: no MMU) |
| K5: 16 thr | 29,859 | 28,830 | 28,103 |
| K4: 8 thr, -t128 | 47,184 | 47,412 | 47,286 |

Committed d23b1e6e0 (the case). Why K3/K5 move the wrong way: after the fix
SimX's response path is the same with and without VM, as in the RTL (wire).
But at this shape the RTL is 9% faster WITH VM than without, and SimX is
12% too fast without VM: a separate difference (request side of the dMMU,
skid + 2-entry out_pipe per lane in RTL, or the non-VM LSU->dcache path),
which the old 2-slot response queue happened to offset. Open.
Remaining h200s ldstream gap after the fix: 173,926 vs 168,615 = +3.1%.
c8 at -m8: rtlsim 90,521; SimX 96,538 (+6.6%) -> 92,654 (+2.4%).
ICE results fetched to vortex-exp/results/dcg_{k3,k4,k5,k3nv,c8m4,c8m8}_rtl;
the ICE source tree was removed.
