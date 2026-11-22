# sim: DRAM interleave unit with the channel and bank mixed

Status (wind-down, 2026-11-21): complete; committed as 109181e08, worktree clean,
nothing pushed. Verified at XLEN 64 only (XLEN 32 out of scope; the AXI
bank-model runs used the foldsim.yaml configuration at XLEN 64). Review
findings: none open; F2 (default changed SimX mapping when MEM_BLOCK_SIZE != DATA_SIZE) fixed in 109181e08 with default 0, shown cycle-identical on SimX and rtlsim.

## Summary

Adds a simulation-only knob, `VX_CFG_PLATFORM_MEMORY_INTERLEAVE_SIZE`
(default 0 = today's mapping), for the bytes of consecutive addresses kept on
one DRAM channel. A nonzero unit (a power of two from one block up to a row:
HBM 1 KB, GDDR6 2 KB) stays whole in one row of one bank, and the unit's
channel and bank are chosen together: the unit index XORed with a
multiplicative hash of the address bits above. The mapping lives in one
function, `DramSim::Config::locate()`, used by SimX's memory crossbar and by
rtlsim's native memory bus, so both drivers map addresses the same way.

## Motivation

With the plain map (block i on channel i mod channels, 128 B per bank visit)
streams that start a power of two apart land on the same channels and banks.
On the fork (fork measurements, fork-only `h200s` profile and `bwstream`
test, 22 SMs, window 256, 32 banks, peak 800 GB/s): a CUDA-block-shaped
stream reached 493 GB/s with the plain 64 B map, 706 with 256 B units mixed,
708 with 1 KB mixed; mixing the channel and the bank separately used only
half the banks (the joint mix is what uses all of them); without any mixing a
1 KB unit sent all streams to one channel at a time (11 SMs: 425 -> 200 GB/s).
The fork also saw the opposite for one access shape (one stream per warp:
660 plain, 643 at 256 B, 408 at 1 KB), so the unit is a calibration choice,
not a universal win.

Measured on this branch (SimX, `--profile=h100 --cores=4`, four HBM3E
channels at 6,400 MT/s with 8 bank groups = 204.8 GB/s, `bwstream -n256 -r1
-p0 -u4 -b32` copied into the build copy from sibling branch
`pr/tests-bwstream`; 8 MB read):

| `INTERLEAVE_SIZE` | cycles | GB/s |
|---|---|---|
| 0 (plain) | 198,122 | 83.8 |
| 256 | 196,446 | 84.5 |
| 1024 | 191,614 | 86.7 |

(results/pr_dram-interleave_v2_simx, cases bw_i0, bw_i256, bw_i1k.) Four SMs
are latency-bound here (41% of peak), so the gain is small at this shape; the
fork's numbers above are at 22 SMs.

## What changed

- `sim/common/dram_sim.{h,cpp}`: `Config::interleave`, `mixed()`,
  `visit_bytes()`, `locate()` (plain: `block % channels`, `block / channels`;
  mixed: the joint hash), and `validate()`, called by the `DramSim`
  constructor: a nonzero unit must be a power of two, at least one block, at
  most a row, with a power-of-two channel count.
- `sw/common/dram_timing.h`: `geometry()` takes the row-visit size (default
  128 B) and checks it lies between a burst and a row; the channel-size and
  burst constants move above it.
- `sim/common/dram_bank_model.h`: the visit size reaches the bank model
  (`VORTEX_DRAM_MODEL=bank`); a comment that said "four bursts" now says "by
  default".
- `sim/simx/mem/memory.cpp`: the crossbar's channel choice and the address
  within the channel come from `locate()`. For a power-of-two channel count
  the plain branch is the old `& (N-1)` / `/ N` exactly.
- `sim/rtlsim/processor.cpp`: the native memory bus keeps the port's own
  channel and `bank_addr` unless the interleave is mixed; then the request's
  byte address goes through `locate()`. The AXI bank-model path (Foldsim's
  mirror) refuses a nonzero unit.
- `sim/opaesim/opae_sim.cpp`, `sim/xrtsim/xrt_sim.cpp`,
  `sw/runtime/foldsim/vortex.cpp`: refuse a nonzero unit (each memory port
  drives its own channel there; xrtsim's platform map binds channels to
  address ranges).
- `VX_config.toml`: `VX_CFG_PLATFORM_MEMORY_INTERLEAVE_SIZE = 0`.
- `docs/simulation.md`: the key in the DRAM table and a paragraph on the
  mapping, including which simulators refuse it and that rtlsim's
  fixed-latency AXI pipe models no DRAM at all (it ignores every key of the
  table, as it already does on upstream).
- `ci/testcases/core.yaml`: `parity-vecadd-interleave` (model_parity, 1 KB
  mixed, HBM3 x2, 2 cores, L2).
- `tests/unittest/dram_sim/main.cpp`: see the bijection check below; refusal
  of 3 channels and of a unit below one block.
- `CHANGELOG.md`.

## The mix is one-to-one (point 6)

Argument: for a fixed `up` (the bits above channels x banks units) the
channel-and-bank field is the unit's low bits XORed with a value that depends
on `up` only, and the bank is then XORed again with a fold of `up`; both are
permutations, and the channel address keeps `up`, the bank and the offset, so
(channel, channel address) determines the address. The channel's own map
(`RamulatorDram::dram_addr()`, and the bank model's `decode()`, the same map)
is a bijection within the channel.

Check: `tests/unittest/dram_sim` walks every block of several runs of
channels x banks units, low in memory and at 37 GB, for HBM2 32 ch / 1 KB,
HBM3 4 ch / 256 B, HBM3E 16 ch / 8 groups / 1 KB, HBM3E 1 ch / 1 KB,
HBM3E 32 ch / 16 groups / 512 B and GDDR6 8 ch / 2 KB: 423,936 blocks, each
on its own (channel, channel address), each keeping its offset within the
unit, and each run putting exactly one unit on every (channel, bank), the
bank as the channel decodes it (`DramBankModel::decode()`, which implements
`dram_addr()`'s map). PASS (results/pr_dram-interleave_v2_simx/run_unit.log).
A non-power-of-two channel count is refused with a mixed unit (the code does
not support it; the plain map still takes any count, as before).

## How verified

Build copy `src_pr_dram-interleave` (branch tree at 60b564e7d, whose code is
identical to the final commit; the final commit differs only in
`ci/testcases/core.yaml` comments/configs, `CHANGELOG.md` and one unit-test
message string, which was not re-run). Base: `src_pr_dram_base`
(upstream/master ccf811e11).

| what | driver | config | branch | upstream | result dir |
|---|---|---|---|---|---|
| unit dram_sim / dram_bank_model | host | - | PASS / PASS | - | pr_dram-interleave_v2_simx |
| sgemm (MEM_BLOCK_SIZE=8, cache.yaml sgemm-11 shape) | simx | default | 817,602 | 817,602 | pr_dram-interleave_v2_simx, pr_dram_base_v2_simx |
| same | rtlsim | default | 830,105 | 830,105 | pr_dram-interleave_v2_rtlsim, pr_dram_base_v2_rtlsim |
| sgemm -n32 | simx / rtlsim | default | 113,990 / 114,005 | 113,990 / 114,005 | v2 dirs, pr_dram_base_rtlsim |
| vecadd -n16384 | simx / rtlsim | default | 381,938 / 380,940 | same | same |
| sgemm -n32 --l2cache --cores=2 | simx / rtlsim | default | 57,246 / 57,326 | same | same |
| vecadd HBM3 x4 6545 --l2cache --cores=2 | simx / rtlsim | default | 188,562 / 188,051 | same | same |
| AXI bank model demo, sgemm -n16 (AXI_BUS=1 AXI_MEM_MODEL=bank) | rtlsim | foldsim.yaml shape | 17,150 / 200,012 | 17,150 / 200,012 | pr_dram-interleave_v2_rtlsim, pr_dram_base_axi |
| parity-vecadd-interleave (1 KB) | simx / rtlsim | 2 cores, L2, HBM3 x2 | 188,595 / 188,085 (0.27%) | - | v2 dirs |
| same, knob off | simx / rtlsim | | 188,623 / 188,113 | - | v2 dirs |
| bwstream 0 / 256 / 1024 | simx | above | 198,122 / 196,446 / 191,614 | - | pr_dram-interleave_v2_simx |

Earlier round (commit 8735f3fc4, before the review fixes, default then
derived from DATA_SIZE): also identical to upstream on SimX for sgemv, dxa_copy,
GDDR6 sgemv and the p100, gtx1080 and h100 profiles
(results/pr_dram-interleave_simx vs pr_dram_base_simx); those runs predate
the default fix but exercise the same plain path.

Not run: opaesim, xrtsim (refusal only; no build here), the Foldsim runtime
(needs the Foldsim install), xrt.

The parity case is too light to be bound by the memory (the knob moves it by
0.01%); it shows that SimX and rtlsim map the same way with the knob on. The
mapping itself is covered by the unit test, the effect by bwstream.

## Change class

R3 (`change_impact.py explain --base upstream/master --head HEAD` reports the
stack; against `pr/dram-hbm3e` the only knob is the new one): detector
`config-knobs` (added `VX_CFG_PLATFORM_MEMORY_INTERLEAVE_SIZE`). Evidence
check `simx-rtl-lockstep` warns that SimX changed without RTL: there is no RTL
behaviour to match (the DRAM keys are simulation-only), SimX's default
mapping is unchanged, and SimX and rtlsim both map through `locate()` when
the knob is on (parity case above).

Still required and not done: maintainer approval; xrt; Foldsim runtime build.
No perf baseline moves.

## Dependencies

Stacked on `pr/dram-hbm3e` (needs `geometry()`'s bank groups so the mix
covers every bank of a non-default organisation, and edits the same lines of
`dram_timing.h`/`dram_sim.*`). Send after it. `pr/dram-sched-window`
conflicts with this branch at the same anchors (also
`sim/rtlsim/processor.cpp`'s `axi_bank_config()`); send order: `dram-hbm3e`,
`dram-interleave`, then `dram-sched-window` rebased onto them by the sender.

## Open questions for the maintainer

1. The 1 KB unit is a calibrated value (reported from the GPU's driver, not a
   document; Accel-Sim uses 256 B with a hashed channel index). No profile
   opts in here; should `h100`/`h200` (see `pr/dram-hbm3e`)?
2. A mixed unit needs a power-of-two channel count, while H100/H200 have 80
   and 96 channels; a modulo-based mix would lift that but is not
   implemented.
3. opaesim and xrtsim refuse the knob; opaesim could map like rtlsim (it
   would need the byte address in its request record) if wanted.

## Notes for the sender

- Review findings (independent reviewer, via the lead) and their state:
  F2 SimX default changed with MEM_BLOCK_SIZE != DATA_SIZE: fixed (default 0,
  `mixed()` = nonzero), shown cycle-identical on both drivers (table).
  F1 below-one-block unit silently plain / non-power-of-two channels thrown
  late: fixed (`validate()` in the DramSim constructor; unit test). F1 docs
  claimed every other path refuses while the AXI latency pipe ignores the
  keys: docs fixed (that pipe ignores every DRAM key on upstream too). F1
  brace-less `if`: fixed. F1 stale "four bursts" and "block i on channel i mod
  channels" text: fixed. Test through the channel's decode: added.
- The bwstream measurement uses a test from sibling branch
  `pr/tests-bwstream`, copied into the build copy only.
- Commit 109181e08 on `pr/dram-hbm3e` 243b3d81e.
