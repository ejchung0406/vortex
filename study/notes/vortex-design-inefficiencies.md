# Vortex design inefficiencies found during the SM-bandwidth study

Things that are not SimX bugs: SimX follows the RTL, and the design itself leaves
performance unused. Each entry says what was measured, where the rule lives, and
what a fix would look like. Dates are 2026-11-19 unless noted. All numbers are
SimX on the `h200q` profile (1.98 GHz clock, 64 warps x 32 threads per SM).

## 1. DXA dispatch sends a request to a busy worker while others are idle

**Rule.** A DXA worker reports ready when its setup stage is idle
(`hw/rtl/dxa/VX_dxa_setup.sv:84`, `req_ready = (setup_state_r == SS_IDLE)`),
whether or not a transfer is running on it. `VX_stream_dispatch` picks the
lowest-numbered ready worker. So a new request is staged behind worker 0's
running transfer while workers 1..N-1 are idle. SimX modelled the same thing
(`sim/simx/dxa/dxa_core.cpp`, setup step of `tick()`).

**Measured.** 16 workers per SM, 64 transfers outstanding (`bwstream_dxa
-e4096 -g64 -d4 -c0`, 4 SMs): 5.3 workers running and 3.1 staged on average,
request queue empty. Delivery 60-67 GB/s per SM.

**SimX change (commit db8fde8f5, fork only).** Two passes: idle workers take a
request first; only the remainder is staged behind running workers. Delivery
90 GB/s per SM steady state (H200 measured about 101). One worker behaves as
before.

**State.** SimX and RTL now differ when there are several workers. At an
issue-bound point (default 1-core config) they still agree within 0.3%
(44,068 vs 44,189 cycles with 1 worker; 43,986 vs 44,029 with 4). An
engine-bound rtlsim comparison is queued (`par2_rtlsim_w4`, `par2_rtlsim_w16`;
SimX gives 49,105 and 41,140 cycles).

**RTL fix plan.** In `VX_dxa_dispatch`, replace the single "first ready"
choice by a two-level one: first ready worker whose transfer is idle, else
first ready worker. Needs each worker's `transfer_active` brought out of
`VX_dxa_worker` to the dispatch. No change to the worker itself. Verify with
the queued engine-bound point: rtlsim should move from the old SimX value to
the new one.

## 2. DXA engine has one memory port; the SM has four

**Rule.** `hw/rtl/VX_socket.sv`: `dxa_gmem_bus_if[1]`. The socket has
`VX_CFG_L1_MEM_PORTS` ports to L2 (4 on the h100 shapes); the DXA shares port 0
with the caches at the lowest priority.

**Measured (after fix 1).** 4 workers: 62 GB/s per SM with one port, 90 with
four. 16 workers reach 90 with one port.

**SimX change (commit 89fd86725).** `VX_CFG_DXA_GMEM_PORTS` (default 1). No RTL
counterpart yet.

## 3. An FPU block keeps at most two operations in flight

**Rule.** `sim/simx/fpu_unit.cpp` admits an operation only while results in
flight plus queued responses are below the response port's capacity,
`VX_CFG_FPU_QUEUE_SIZE = 2 * (SIMD_WIDTH / NUM_FPU_LANES)` = 2 on h200q. The
comment says this mirrors the RTL's response tags. With an FMA latency of 12 a
block completes 2 operations per ~14 cycles although its pipeline is 12 deep.

**Measured.** Register-only multiply-add kernel (`corun -b0 -i4096 -g64`,
1 SM, 64 warps): IPC 0.86 of an issue width of 4. `FMA_LATENCY=4`: 1.98.
`FPU_QUEUE_SIZE=8` or 16: 2.18.

**Fix.** Configuration. h200q now sets `VX_CFG_FPU_QUEUE_SIZE=16` (vortex
commit after 87f598f16, "let h200q's FP pipeline fill"). RTL cost: more
response tags per FPU block; not measured.

## 3b. Issue stops a unit at three operations on their way to it

**Rule.** `sim/simx/core.cpp`: a functional unit is "near full" for a slice at
`VX_CFG_DISPATCH_QUEUE_SIZE - 1` (= 3) operations between issue and the unit.

**Measured.** With fix 3: IPC 2.18 at 4, 2.96 at 8, 16 or 32. h200q now sets 8.

## 3c. Two operands from one register-file bank take two cycles

**Rule.** `sim/simx/opc_unit.cpp`: each of the `VX_CFG_NUM_GPR_BANKS` (4) banks
serves one operand per cycle; bank = register index mod 4. `fmadd fa0,fa0,fa4,fa3`
(f10, f14 both bank 2) and `bltu t0,a7` (x5, x17 both bank 1) each take a
second cycle.

**Measured.** With fixes 3 and 3b: IPC 2.96 with 4 banks, 3.30 with 8 or 16
(operand stalls 24% -> 0%). h200q now sets 8 (decided with the user: the roof
should not depend on which registers the compiler picked). RTL area cost of 8
banks not measured.
After that the SM is short of ready warps (scheduler idle 9%).

Ruled out by measurement: operand collectors (4 -> 16), instruction buffer
(4 -> 8), instruction cache banks, integer lanes (16 -> 32), the FCSR rule of
entry 5.

## 4. A warp fetches one instruction per ~11 cycles

**Rule.** A warp is rescheduled only after its previous instruction has been
fetched and decoded (schedule -> decode is 10 cycles on h200q), so one warp
supplies at most one instruction per 11 cycles; a taken branch adds ~13.

**Measured.** One warp alone: 89 cycles per loop iteration of 6 instructions.
Trace: each instruction is scheduled 11 cycles after the previous one.

**Effect.** Sets the single-warp speed. With 64 warps it allows up to ~4.7
instructions per cycle on this loop, so it is not what holds the SM at 2.18.

## 5. Floating-point operations of one warp run one at a time

**Rule.** `sim/simx/scoreboard.cpp:63`: an instruction waits while its warp has
an FCSR field reserved that it reads or writes. Every FP arithmetic operation
writes the sticky FFLAGS, so a warp's FP operations are serialized: the next
one issues about 21 cycles after the previous (latency 12 + 9 of pipeline).

**Measured.** One warp: 93,864 cycles with the rule, 83,623 without the
write-write part (1024 iterations). 64 warps: no change (IPC 2.17 either way),
so it does not limit the SM.

**Fix idea.** Sticky flags accumulate by OR, so writers need not wait for each
other; only a reader of FFLAGS has to wait for all writers. RTL and SimX both.
Low priority.

## Open

- What holds DXA delivery at 90 GB/s per SM.
- Why decode-GEMM time per SM grows with the SM count (memory latency 395 ->
  545 -> 704 cycles at 1 / 8 / 16 SMs while the memory is at 15-25% of its
  peak).
