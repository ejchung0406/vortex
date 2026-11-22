# sim: size the DRAM scheduler window (VX_CFG_PLATFORM_MEMORY_SCHED_WINDOW)

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

@@MOTIVATION@@

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

@@VERIFIED@@

## Change class

@@CLASS@@

## Dependencies

Independent of `pr/dram-hbm3e` and `pr/dram-interleave` (based on
upstream/master). It touches lines next to theirs in `VX_config.toml`,
`sim/common/dram_sim.{h,cpp}`, `docs/simulation.md`, `ci/testcases/core.yaml`,
`tests/unittest/dram_sim/main.cpp`, `sw/runtime/foldsim/vortex.cpp`,
`sim/rtlsim/processor.cpp` and `CHANGELOG.md`: whichever lands second needs a
mechanical rebase (adjacent insertions, no semantic overlap). Suggested order:
`dram-hbm3e`, then `dram-sched-window`, then `dram-interleave`.

## Open questions for the maintainer

@@OPEN@@
