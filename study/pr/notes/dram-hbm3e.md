# sim: HBM3E DRAM type and HBM bank groups per pseudo-channel

Status (wind-down, 2026-11-21): complete; committed as 243b3d81e, worktree clean,
nothing pushed. Verified at XLEN 64 only (XLEN 32 out of scope; the AXI
bank-model runs used the foldsim.yaml configuration at XLEN 64). Review
findings: none open.

## Summary

Adds a DRAM type `HBM3E` that models one JESD238 (HBM3) channel as the
standard defines it, and a knob for the bank groups of an HBM pseudo-channel.
Both are simulation-only and off by default; no existing profile changes.

- `VX_CFG_PLATFORM_MEMORY_TYPE=HBM3E`: a 64-bit channel of two 32-bit
  pseudo-channels. A 32 B burst is BL8, four Ramulator clocks (two of the
  part's CK, which runs at a quarter of the data rate), and the column-to-column
  spacing within a bank group is tCCDL = max(4 nCK, 2.5 ns). The existing
  `HBM3` type is unchanged: it keeps modelling a pair of HBM3 channels as one
  128-bit channel with BL4 bursts (HBM2's shape at HBM3's rates).
- `VX_CFG_PLATFORM_MEMORY_BANK_GROUPS` (default 4): bank groups per HBM
  pseudo-channel, four banks each, a power of two up to 16; the channel keeps
  its 8 Gb, so more groups have fewer rows. GDDR6 refuses any value but 4.

## Motivation

The fork this comes from studies an H200-class memory system. Its stock
`HBM3` channel (two HBM3 channels merged into one 128-bit channel at the
combined rate) gives half the channels, half the banks and twice the per-burst
rate of the part; with many SMs streaming, the model delivered 16-22% of the
nominal bandwidth (fork measurement, fork-only `h200s` profile, 33 SMs). With
the channel modelled as it is and 8 bank groups (32 banks per pseudo-channel,
what JESD238 gives a stack with two stack IDs), a 22-SM stream went from 382 to
522 GB/s at Ramulator's 32-entry scheduler window (fork measurement on
`h200s` with the fork-only `bwstream` test; 64 banks gave no further gain).
tCCDL itself moved results by under 2% there.

Measured on this branch (upstream tests and a build-copy-only `bwstream`):

SimX, `--profile=h100 --cores=4` with the memory overridden to 204.8 GB/s
in both cases (2 HBM3 channels at 6,400 MT/s, or 4 HBM3E channels at 6,400
MT/s), `bwstream -n256 -r1 -p0 -u4 -b32` (8 MB read; the test is copied into
the build copy only, from sibling branch `pr/tests-bwstream`):

| memory | cycles | GB/s |
|---|---|---|
| HBM3, 2 channels (stock merged shape) | 342,346 | 48.5 |
| HBM3E, 4 channels, 4 bank groups | 205,431 | 80.9 |
| HBM3E, 4 channels, 8 bank groups | 198,122 | 83.8 |

(results/pr_dram-hbm3e_v2_simx, cases bw_hbm3, bw_hbm3e, bw_hbm3e_bg8.)
Unit test on one channel: an HBM3E channel at 6,400 MT/s streams 24.8 B per
core cycle, 96% of its computed peak; 64 streams 1 MB apart go 1.3x faster
with 8 bank groups than with 4.

## What changed

- `sw/common/dram_timing.h`: `Standard::HBM3E`, `hbm3e_bus_timing` (nBL 4,
  nCCDS 4, nCCDL 8 = 4 nCK), `hbm_bus()`, `hbm3e_ccdl()` (raises nCCDL to
  2.5 ns where that is longer, e.g. 12 clocks at 9,600 MT/s), and
  `geometry(standard, bank_groups)`.
- `sim/common/dram_sim.{h,cpp}`: `Config::bank_groups`; the HBM3E device is
  Ramulator's HBM3 implementation with the HBM3E bus timings (the 128-bit
  `channel_width` stays as address granularity only); a non-default bank-group
  count overrides Ramulator's `org.bankgroup` and `org.row`, which keeps its
  8 Gb density check passing.
- `sim/common/dram_bank_model.h`: `Config::make()` and `DramBankSim::Config`
  take the bank groups (the `VORTEX_DRAM_MODEL=bank` backend honours them).
- `sim/rtlsim/processor.cpp` (AXI bank-model path, Foldsim's mirror) and
  `sw/runtime/foldsim/vortex.cpp` (the host writer of the Foldsim image's bank
  model registers): refuse a bank-group count other than 4, so the host writer
  and the model reader never disagree silently. `dram_timing.h` is not one of
  the headers `change_impact.py` treats as ABI (`sw-common-abi`); its only
  includers are `sim/common/dram_{sim,bank_model}` and this Foldsim runtime.
- `VX_config.toml`: `VX_CFG_PLATFORM_MEMORY_BANK_GROUPS = 4`; `HBM3E` added to
  the `VX_CFG_PLATFORM_MEMORY_TYPE` enum.
- `docs/simulation.md`: the type and the knob in the DRAM table, the HBM3E
  paragraph, the per-transfer widths.
- `ci/testcases/core.yaml`: `parity-vecadd-hbm3e` (model_parity: HBM3E x2 at
  6,400 MT/s, 8 bank groups, 2 cores, L2).
- `tests/unittest/dram_sim/main.cpp`: HBM3E streams 75-100% of its computed
  peak; tCCDL is 8 clocks at 6,400 MT/s and 12 at 9,600; 8 bank groups stream
  64 strided streams over 1.15x faster than 4. Runs in the existing
  `unittest/sw` CI lane.
- `CHANGELOG.md`.

## Provenance of the values

- From JESD238B.01 (HBM3), as read on the fork (the standard is licensed;
  nothing from it is committed, and no table is quoted here): 64 DQ per
  channel, two pseudo-channels, 256-bit prefetch per pseudo-channel so a 32 B
  burst is BL8; banks per pseudo-channel = 16 per stack ID (so 8 groups of 4
  for a part with two stack IDs); tCCDL = max(4 nCK, 2.5 ns). The reviewer of
  this branch did not check these against the document; the maintainer may
  want to.
- Not from the standard (vendor values, left blank there): the row timings.
  HBM3E reuses the existing HBM2 array timing table (tRCD/tRP 14, tRAS 33,
  tRC 47, tRRD 4, tFAW 15 ns, GPGPU-Sim's SM7_QV100 JEDEC line), as `HBM3`
  already does. Their sensitivity is not measured.
- Not in this branch: 6,250 MT/s is the fork's H200 data rate (4.8 TB/s over
  6 stacks x 1,024 pins); the type takes any `DATA_RATE`. The calibrated
  values (window 256, 1 KB interleave) belong to `pr/dram-sched-window` and
  `pr/dram-interleave`.

## How verified

Build copy `src_pr_dram-hbm3e` = branch tree at 8230e48a6, whose code is
the final commit's (the final commit adds only the CHANGELOG entry and the
parity case's configs/comment, and those configs are what was run as
`pa_on`). Base `src_pr_dram_base` = upstream/master ccf811e11. The
third_party extension library was rebuilt in each copy by its own
`third_party/Makefile`.

| what | driver | config | branch | upstream | result dir |
|---|---|---|---|---|---|
| unit dram_sim / dram_bank_model | host | - | PASS / PASS | - | pr_dram-hbm3e_v2_simx |
| sgemm MEM_BLOCK_SIZE=8 (cache.yaml sgemm-11 shape) | simx | default | 817,602 | 817,602 | pr_dram-hbm3e_v2_simx, pr_dram_base_v2_simx |
| sgemm -n32 | simx | default | 113,990 | 113,990 | same |
| vecadd -n16384 | simx | default | 381,938 | 381,938 | same |
| sgemm -n32 --l2cache --cores=2 | simx | default | 57,246 | 57,246 | same |
| vecadd HBM3 x4 6545, --l2cache --cores=2 | simx | HBM3 | 188,562 | 188,562 | same |
| AXI bank model demo / sgemm -n16 (AXI_BUS=1 AXI_MEM_MODEL=bank) | rtlsim | foldsim.yaml shape | 17,150 / 200,012 | 17,150 / 200,012 | pr_dram-hbm3e_v2_rtlsim, pr_dram_base_axi |
| parity-vecadd-hbm3e (`pa_on`) | simx / rtlsim | HBM3E x2, 8 groups | 188,574 / 188,064 (0.27%) | - | v2 dirs |
| same with 4 groups (`pa_off`) | simx / rtlsim | | 188,627 / 188,117 | - | v2 dirs |
| bwstream (above) | simx | h100 x4 | see Motivation | - | pr_dram-hbm3e_v2_simx |

First round, on commit eac8c869e (before the review fixes, which touched
only the AXI refusal, `hbm3e_ccdl()`'s floor and a test threshold):
identical to upstream on SimX and rtlsim for sgemm -n32, sgemv -m128 -n128,
vecadd -n16384, dxa_copy -d2, the two-core L2 sgemm, the HBM3 shape and a
GDDR6 x8 sgemv; on SimX also the p100 (HBM2), gtx1080 (GDDR6) and h100
(HBM3) profiles at 4 cores (results/pr_dram-hbm3e_simx,
pr_dram-hbm3e_rtlsim vs pr_dram_base_simx, pr_dram_base_rtlsim).

Not run: Foldsim runtime build (needs the Foldsim install), xrt, opaesim,
xrtsim; the a100 profile, because upstream/master's SimX does not build with
it (`sim/simx/tcu/tcu_unit.cpp:1239` calls `decode_descs()`, defined only
under `VX_CFG_TCU_WGMMA_ENABLE`; a100 has the TCU without wgmma; not this
branch's doing).

The parity case is too light to be bound by the memory (8 bank groups move it
by 0.03%); it shows that SimX and rtlsim agree with the type and the knob on.

## Change class

R3 (`change_impact.py explain --base upstream/master --head HEAD`): detector
`config-knobs` (added `VX_CFG_PLATFORM_MEMORY_BANK_GROUPS`; the
`VX_CFG_PLATFORM_MEMORY_TYPE` enum gains `HBM3E`). Paths: `sim/common` R2
(simx-model), `sw/common/dram_timing.h` and `sw/runtime/foldsim` R2
(sw-stack), `sim/rtlsim/processor.cpp` R1, tests/CI R1, docs R0. No RTL change
(the keys are simulation-only).

Still required by the policy and not done here: the maintainer's approval;
the Foldsim runtime change is not built or run (needs the Foldsim install);
xrt not run. No perf baseline moves (no profile or existing CI case changes).
An independent review agent did review the branch (findings and their fixes
are listed in the sender notes).

## Dependencies

None: based on upstream/master. `pr/dram-interleave` is stacked on this
branch (it needs `geometry()`'s bank groups to size the channel/bank mix).
`pr/dram-sched-window` is independent but edits the same files at the same
anchors. Send order: this one, then `pr/dram-interleave`, then
`pr/dram-sched-window` (rebased).

## Open questions for the maintainer

1. Should `h100`/`h200` move to `HBM3E`? Not proposed in this PR: doing so
   changes those profiles' cycles and their perf baselines (a human
   re-record), and the parts' real channel counts are not powers of two (H100
   SXM5: 5 stacks x 16 = 80 channels; H200: 6 x 16 = 96), while the profiles
   today use 32 merged channel pairs precisely to keep a power-of-two count.
   A sixth or a fifth of the part (one stack, 16 channels) fits.
2. The row timings of HBM3E parts are vendor data; the model reuses HBM2's.
   Not measured: how much they matter.
3. Ramulator issues one command per clock per channel; JESD238 has separate
   row and column command buses. Not measured.
4. Does the Foldsim image support more than 4 bank groups? Its geometry
   register has a 2-bit field for them; this branch refuses anything but 4
   rather than guess.

## Notes for the sender

- Review findings (independent reviewer, via the lead): rtlsim's AXI bank
  model honoured bank groups while the Foldsim runtime refused them: fixed
  (both refuse anything but 4). The bank-group unit check had no margin:
  fixed (now over 1.15x; measured 1.3x). nCCDL 8 in `hbm3e_bus_timing` was
  dead: fixed (`hbm3e_ccdl()` now takes its 4 nCK floor from that entry).
  JESD238 values not verified by the reviewer: provenance listed above.
- Commit 243b3d81e. `pr/dram-interleave` (109181e08) sits on it.
