# sim: size the DRAM scheduler window (VX_CFG_PLATFORM_MEMORY_SCHED_WINDOW)

Status (wind-down, 2026-11-21): complete; committed as 8d3e9211a, worktree clean,
nothing pushed. Verified at XLEN 64 only (XLEN 32 out of scope; the AXI
bank-model runs used the foldsim.yaml configuration at XLEN 64). Review
findings: none open; SCHED_WINDOW=0 is refused since 8d3e9211a (unit test).

## Summary

Ramulator's generic DRAM controller chooses its next command among at most 32
read and 32 write bursts per channel (`ReqBuffer::max_size = 32`, a constant in
`base/request.h`). A channel fed by many independent streams fills that window
with one burst of each stream, so FR-FCFS finds few row hits and few other
banks' work. This adds a simulation-only knob, `VX_CFG_PLATFORM_MEMORY_SCHED_WINDOW`
(default 32), for the bursts a channel's scheduler chooses among. At 32 nothing
changes: the simulators still instantiate Ramulator's own `Generic` controller.
Any other value instantiates `Windowed`, the same controller source that
`third_party/Makefile` copies at build time with its two buffer sizes made
parameters.

## Motivation

On the fork (fork measurements on the fork-only `h200s` profile with the
fork-only `bwstream` test, 22 SMs on 16 HBM3E channels, 800 GB/s peak): 382
GB/s at Ramulator's 32, 588 at 128, 621 at 256, 636 at 512; at 32, 74% of the
controller's enqueue attempts were refused (a fork-side counter, not in this
branch). Little's law at the part's per-channel rate over the time a burst
stays in the controller gives about 100 entries; the fork's profile uses 256,
a calibrated value. Accel-Sim's H100/H200 configurations use 128 per (narrower)
channel.

Measured on this branch: SimX, `--profile=h100 --cores=4` with the memory
overridden to two HBM3 channels at 6,400 MT/s (204.8 GB/s), `bwstream -n256
-r1 -p0 -u4 -b32` (8 MB read; copied into the build copy only, from sibling
branch `pr/tests-bwstream`):

| window | cycles | GB/s | avg read queue per channel | read row hits (ch 0) |
|---|---|---|---|---|
| 32 (Ramulator's) | 342,346 | 48.5 | 42.5 | 74,707 |
| 256 | 234,050 | 71.0 | 122.3 | 78,561 |

(results/pr_dram-sched-window_v2_simx, cases bw_w32 and bw_w256, with
`ramulator_bw_w*.stats.log`; Ramulator's queue average includes the reads
waiting out their latency, so it can exceed 33 at a window of 32.) Unit test:
64 streams 1 MB apart on one H100-shaped channel stream 2.0x faster at 256
than at 32.

## What changed

- `third_party/Makefile`: a rule that generates
  `ramulator_ext/windowed_controller.cpp` from Ramulator's
  `src/dram_controller/impl/generic_dram_controller.cpp` with `sed` (class
  renamed, registered as `"Windowed"`, `read_buffer_size` / `write_buffer_size`
  parameters with default 32 set in `init()`), compiles it into the existing
  `libramulator-vortex.so` next to the GDDR6 model, and stops the build if the
  rewritten lines are no longer found (`grep -q` guard). `make clean` removes
  the generated file.
- `.gitignore`: the generated `third_party/ramulator_ext/windowed_controller.cpp`.
- `VX_config.toml`: `VX_CFG_PLATFORM_MEMORY_SCHED_WINDOW = 32`.
- `sim/common/dram_sim.{h,cpp}`: `Config::sched_window`; `Generic` at 32,
  `Windowed` with both buffer sizes otherwise. The bank-model backend
  (`VORTEX_DRAM_MODEL=bank`) throws for any other value: its controller buffers
  are Foldsim's endpoint's, 32 bursts each.
- `sim/rtlsim/processor.cpp` (AXI bank-model path) and
  `sw/runtime/foldsim/vortex.cpp`: refuse a window other than 32 for the same
  reason (the Foldsim image's queue registers are 8 bits and its bank model is
  fixed at 32).
- `docs/simulation.md` (knob table), `docs/codebase.md` (`ramulator_ext`
  contents), `CHANGELOG.md`.
- `ci/testcases/core.yaml`: `parity-vecadd-sched-window` (model_parity, window
  256, HBM3 x4 at 6400 MT/s, 2 cores, L2).
- `tests/unittest/dram_sim/main.cpp`: 64 streams 1 MB apart on one H100-shaped
  channel stream faster at 256 than at 32; the bank model refuses 256. Runs in
  the existing `unittest/sw` CI lane (`make -C tests/unittest run`).

### Why a build-time copy, and the alternatives

Ramulator is a submodule pinned at e62c84a6 of CMU-SAFARI/ramulator2; Vortex
already builds extra Ramulator sources into `third_party/ramulator_ext`
(`libramulator-vortex.so`, today only the GDDR6 device). Considered:

1. Patch the submodule (or point it at a fork): invasive, and every Ramulator
   update has to carry the patch.
2. Upstream a `read_buffer_size` parameter to Ramulator and bump the pin: the
   right long-term fix, but it depends on Ramulator's maintainers; this PR can
   be reduced to the `dram_sim` lines once it lands.
3. A controller plugin (`IControllerPlugin`) that resizes the buffers: plugins
   only see `IDRAMController*`; the buffers are private members of a class
   defined in a `.cpp`, so there is no access without the class.
4. Subclass or `#include` the controller source: the class is `final` and
   registers the name `"Generic"`, so a second copy in the same process
   collides unless the registration line is rewritten anyway.
5. A hand-written copy committed under `ramulator_ext`: drifts silently from
   Ramulator's source on an update.
6. The chosen one: generate the copy with `sed` at build time from the pinned
   source. Three substitutions, no Ramulator code committed, the copy follows
   Ramulator updates automatically, and the `grep -q` guard fails the build
   loudly if the rewritten lines move. `Generic` stays Ramulator's own and is
   what every default configuration still uses.

Note: Ramulator's `ReqBuffer::enqueue` accepts while `size <= max_size`, so a
window of N holds N+1 bursts (33 for Ramulator's default); the knob keeps
Ramulator's meaning.

## How verified

Build copy `src_pr_dram-sched-window` = branch tree at 857d99c26, whose
code is the final commit's (the final commit adds the CHANGELOG entry and the
parity case's configs/comment, which is what ran as `pb_on`); its
`third_party/Makefile` rebuilt `libramulator-vortex.so` with the Windowed
controller. The generation rule was also exercised by hand: its output is
byte-identical to the fork's generated file, a re-run is up to date, and with
a source that lacks the rewritten lines it fails with a message and leaves no
file. Base `src_pr_dram_base` = upstream/master ccf811e11.

| what | driver | config | branch | upstream | result dir |
|---|---|---|---|---|---|
| unit dram_sim / dram_bank_model (incl. window 0 and bank-model refusals) | host | - | PASS / PASS | - | pr_dram-sched-window_v2_simx |
| sgemm MEM_BLOCK_SIZE=8 | simx | default | 817,602 | 817,602 | pr_dram-sched-window_v2_simx, pr_dram_base_v2_simx |
| sgemm -n32 | simx | default | 113,990 | 113,990 | same |
| vecadd -n16384 | simx | default | 381,938 | 381,938 | same |
| sgemm -n32 --l2cache --cores=2 | simx | default | 57,246 | 57,246 | same |
| vecadd HBM3 x4 6545 --l2cache --cores=2 | simx | HBM3 | 188,562 | 188,562 | same |
| AXI bank model demo / sgemm -n16 | rtlsim | foldsim.yaml shape | 17,150 / 200,012 | 17,150 / 200,012 | pr_dram-sched-window_v2_rtlsim, pr_dram_base_axi |
| parity-vecadd-sched-window (`pb_on`) | simx / rtlsim | HBM3 x2, window 256 | 188,623 / 188,113 (0.27%) | - | v2 dirs |
| same at 32 (`pb_off`) | simx / rtlsim | | 188,623 / 188,113 | - | v2 dirs |
| bwstream 32 / 256 | simx | above | 342,346 / 234,050 | - | pr_dram-sched-window_v2_simx |

First round, on commit 214ff1649 (before the review fixes, which added the
window-0 refusal and the Makefile guards): identical to upstream on SimX and
rtlsim for sgemm -n32, sgemv, vecadd, dxa_copy -d2, the two-core L2 sgemm,
HBM3 and GDDR6 shapes; SimX also the p100, gtx1080 and h100 profiles
(results/pr_dram-sched-window_simx, pr_dram-sched-window_rtlsim vs the
pr_dram_base_* dirs).

The parity case does not fill a 32-burst window (cycles identical at 32 and
256); it shows that the Windowed controller loads and times the same on both
drivers. The window's effect is in the unit test and bwstream.

Not run: the Foldsim runtime (refusal only; needs the Foldsim install), xrt,
opaesim, xrtsim.

## Change class

R3 (`change_impact.py explain --base upstream/master --head HEAD`): detector
`config-knobs` (added `VX_CFG_PLATFORM_MEMORY_SCHED_WINDOW`); evidence check
`configure-copied` for `third_party/Makefile` (tests ran from freshly
configured build trees). Paths: `sim/common` R2, `third_party/Makefile` R2
(build-config), `sw/runtime/foldsim` R2, `sim/rtlsim/processor.cpp` R1,
tests/CI R1, docs and `.gitignore` R0.

Still required and not done: maintainer approval; xrt; Foldsim runtime build.
No perf baseline moves (no profile or existing case changes). Existing build
trees pick up the Windowed controller on their next `make` (the top-level
`make` builds `third_party`); a tree whose `third_party` is not rebuilt and a
non-default window should fail at start with Ramulator's
unknown-implementation error (not tried).

## Dependencies

Independent of `pr/dram-hbm3e` and `pr/dram-interleave` (based on
upstream/master). It touches lines next to theirs in `VX_config.toml`,
`sim/common/dram_sim.{h,cpp}`, `docs/simulation.md`, `ci/testcases/core.yaml`,
`tests/unittest/dram_sim/main.cpp`, `sw/runtime/foldsim/vortex.cpp`,
`sim/rtlsim/processor.cpp` and `CHANGELOG.md`: whichever lands second needs a
rebase. `git merge-tree` against `pr/dram-interleave` (which includes
`pr/dram-hbm3e`) conflicts in 8 files (`VX_config.toml`,
`ci/testcases/core.yaml`, `docs/simulation.md`, `sim/common/dram_sim.{h,cpp}`,
`sim/rtlsim/processor.cpp`, `sw/runtime/foldsim/vortex.cpp`,
`tests/unittest/dram_sim/main.cpp`) plus `CHANGELOG.md`; all are insertions at
the same anchors, kept both ways, except the `BankDram` constructor, where
(a)/(c) extend the initializer and this branch adds the body: keep both. Send
order: `dram-hbm3e`, `dram-interleave`, then this branch, rebased by the
sender onto what has landed.

## Open questions for the maintainer

1. Should Ramulator get a `read_buffer_size` parameter upstream, so this
   copy can go? Then only the `dram_sim` lines remain.
2. No profile opts in. The fork's H200 study uses 256 (calibrated, see
   above); `h100`/`h200` opting in changes their cycles and perf baselines.
3. The bank model (and Foldsim's image) keeps 32-burst buffers; matching a
   larger window there would need the image's queue registers (8 bits) to
   grow.

## Notes for the sender

- Review findings (independent reviewer, via the lead), all fixed in
  8d3e9211a: the generated file now lists the Makefile as a prerequisite; the
  guard also checks `class WindowedDRAMController`; a failed generation
  removes its `.tmp`; a window of 0 is refused when the Ramulator backend is
  built (unit test).
- The bwstream measurement uses a test from sibling branch
  `pr/tests-bwstream`, copied into the build copy only.
