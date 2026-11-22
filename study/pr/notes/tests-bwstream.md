# tests: bwstream and bwstream_dxa, streaming-read bandwidth probes

## Summary

Two regression tests that read a buffer the host never writes, so every line
they load is a memory read that no cache holds:

- `bwstream`: each thread sums `-n` floats with plain loads, 1 to 8 loads
  outstanding (`-u`). `-p 0` coalesced (a warp reads adjacent floats per
  load), `-p 1` one stream per thread. `-b W` makes W consecutive one-warp
  CTAs share one slice as the warps of a W-warp thread block do; `-b 1` gives
  each warp its own contiguous stream.
- `bwstream_dxa`: each one-warp CTA has the DXA copy its tiles into local
  memory, keeping `-d` (1 to 8, at most the barriers per CTA) transfers
  outstanding through a ring of tile buffers with one barrier each, and sums
  them (`-c 0`: reads one word per tile, so the DXA's delivery is the only
  work per tile).

Both check every thread's sum on the host and exit non-zero on a mismatch.
The bandwidth is read from `--perf=7` (memory reads over cycles).

## Motivation

There is no test that measures streaming-read bandwidth without the cache
serving part of it: an upload leaves its lines in the last-level cache (see
`tests/regression/chase`, which uses a never-written buffer for its DRAM
latency for the same reason). `vecadd` streams uploaded data.

The access order matters for what is measured. In a CUDA kernel the warps of
a block read neighbouring 128 B pieces at the same moment and a warp's next
load is a block-width further on; a probe that gives each warp its own
contiguous stream puts a different address stream on the memory. `-b W`
reproduces the block order; `-b 1` keeps the own-stream order. On the research
fork these two orders ranked DRAM address mappings oppositely (fork
measurement, not repeated here).

Method note, carried in the test's header and usage text: differencing two
run lengths (`-n`) to remove launch and drain assumes the memory sees the same
address streams at both lengths, and it does not, because slices start
`W * steps * threads` floats apart. With `-b 1` every warp is a stream of its
own, so a different `-n` moves all of them and the difference is the
bandwidth of neither run; on the fork that produced wrong figures. With
`-b W` there are W times fewer streams; on the fork whole-run and differenced
values agreed there (fork measurements), but this is not guaranteed by
construction, so the header says to check.

## What changed

- `tests/regression/bwstream/{Makefile,common.h,kernel.cpp,main.cpp}` (new)
- `tests/regression/bwstream_dxa/{Makefile,common.h,kernel.cpp,main.cpp}` (new)
- `tests/regression/Makefile`: both added to `TESTS` (`bwstream` after `chase`,
  `bwstream_dxa` after `dxa_kmajor_check`), so `all`, `run-<driver>` and
  `clean` cover them.
- `tests/regression/Makefile`: both also in `EXCLUDE_opae` and `EXCLUDE_xrt`
  (only the simulators fill unwritten memory with one value).
- `ci/testcases/regression.yaml`: `bwstream-own` (`-n32 -u4`), `bwstream-rows`
  (`-n32 -p1 -u2`), `bwstream-block` (`-n32 -u4 -b4`, 2 cores + L2); simx and
  rtlsim, XLEN 32 and 64, smoke tier.
- `ci/testcases/dxa.yaml`: `bwstream_dxa-1` (`-e64 -n8 -d4`), `bwstream_dxa-2`
  (`-e64 -n8 -d8 -c0`, 2 cores + L2) on simx and rtlsim; `bwstream_dxa-3`
  (`-e64 -n3 -d4`) on simx.

No existing source, test or baseline is touched.

## How verified

Verified at XLEN 64 only. Build copy of the branch at c6b1d85c7
(`src_pr_tests-bwstream`, identical to the commits), `VORTEX_VM_MEGAPAGE_HEAP`
unset. CI cases were run with the exact commands `ci/testcase.py` produces for
them (`Spec.run_command`). Results under `vortex-exp/results/`.

| What | Config | SimX | rtlsim |
|---|---|---|---|
| `make -C tests/regression all` | default | builds (rc 0) | |
| `make run-<drv>-bwstream` (`-n64 -r1`) | default 1 core, 4w x 4t | PASSED, 4,722 | PASSED, 4,735 |
| `make run-<drv>-bwstream_dxa` (`-e64 -n4 -r1`) | default + DXA | PASSED, 9,988 | PASSED, 9,982 |
| `bwstream-own` `-n32 -u4` | default | PASSED, 2,600 | PASSED, 2,613 |
| `bwstream-rows` `-n32 -p1 -u2` | default | PASSED, 2,469 | PASSED, 2,482 |
| `bwstream-block` `-n32 -u4 -b4` | 2 cores + L2 | PASSED, 2,754 | PASSED, 2,753 |
| `bwstream_dxa-1` `-e64 -n8 -d4` | default + DXA | PASSED, 18,828 | PASSED, 18,837 |
| `bwstream_dxa-2` `-e64 -n8 -d8 -c0` | 2 cores + L2 + DXA | PASSED, 6,235 | PASSED, 6,238 |
| `bwstream_dxa-3` `-e64 -n3 -d4` | default + DXA | PASSED, 8,096 | (simx-only case) |
| `bwstream -n64 -r1 -p0 -u4 -b32` | 1 core, 64w x 32t, `-DVX_CFG_DCACHE_NUM_BANKS=4` | PASSED, 16,942 | PASSED, 16,377 |

Result directories: `pr_tests-bwstream_v2_x64_simx` (SimX rows),
`pr_tests-bwstream_v2_x64_rtlsim` (rtlsim rows and the rtlsim parity run),
`pr_tests-bwstream_v2_par_x64_simx` (SimX parity run). Cycles are the
runtime's `PERF: instrs=..., cycles=...` line; retired instructions are equal
between the drivers in every row. The last row is the 64-warp shape: SimX and
rtlsim agree within 3.4% on the stock configuration.

Not run: XLEN 32 (out of scope; every new CI case also runs at 32 in CI),
opae and xrt (both tests are excluded there), and a check of the differencing
statement in the header (it now states the mechanism only).

Existing tests are not touched; `make -C tests/regression all` builds the
whole list with the two new entries.

## Change class

`python3 ci/change_impact.py explain --base upstream/master --head HEAD`:
**R2**, reason `path:build-config` (the three Makefiles; configure copies them
into the build tree). Every other file is R1 (`tests`).

R2 asks for a review of every changed hunk and maintainer approval. Not
needed here: Beacon / `logic_gate` (no RTL), perf-baseline re-record (no
cycles move), `model_parity` (no model change).

## Dependencies

None. Sibling branches `simx-dcache-fwd-hold` and `dxa-dispatch-idle` use these
tests for their evidence and should land after this one (or carry it).

## Open questions for the maintainer

- The host check counts loads (every unwritten word reads as the same value),
  it cannot catch a wrong address. Writing the buffer would put it in the
  cache and defeat the probe. Acceptable, or is there a preferred way to seed
  device memory without the cache?
- Should a `model_parity` case on `bwstream` go in? On the stock configuration
  the 64-warp shape agrees within 3.4% at XLEN 64 (above).

## Notes for the sender

- State: complete at XLEN 64; XLEN 32 not run (out of scope for this effort).
- Branch `pr/tests-bwstream`: 6161fba5f (bwstream), c6b1d85c7 (bwstream_dxa).
  Earlier hashes f5c9c46eb/939e502aa and 7550e77d1/14619c182 are superseded;
  `pr/tests-bwstream-prev-msgs-backup` points at 14619c182 (kept, nothing
  deleted). Siblings stacking on this branch should use c6b1d85c7.
- Two commits because the tests share no code and the second needs the DXA
  extension and lives in the DXA CI category.
- Taken from the fork head (214405599, 601a1d482, 87f598f16, no later edits),
  without `profiles/rtxspark.json` and `gemm_decode`, then cleaned after an
  independent review: braces everywhere; `-b` rejected with `-p 1`; `-r 0`
  rejected; the fill value read from its own 4-byte buffer so no source line
  enters a cache; `bwstream_dxa` depth capped at min(8, VX_CFG_NUM_BARRIERS);
  a why-comment on the slot-reuse barrier; both tests excluded on opae/xrt;
  CI comments corrected; `bwstream_dxa-3` added (ring deeper than the tiles).
- The differencing text was corrected after review: with `-b W` the slices
  also start `W * steps * threads` floats apart, so the layout moves with `-n`
  in both shapes. The header now states that and says to check a differenced
  value against the whole run. The fork observed whole-run and differenced
  values agreeing in the block shape (fork measurement, not repeated here).
- Review findings still open (not done): stronger address checking (one
  thread per CTA reporting its last address), interleaving slices across the
  grid so the layout is independent of `-n`. The RTL barrier's LSU drain was
  not reviewed (only SimX).
- Parity: the fork's gap on this shape (rtlsim 20,570 vs SimX 25,824, fork
  measurement on `h200s`) does not appear on upstream's stock configuration
  (16,377 vs 16,942). `simx-dcache-fwd-hold` needs a configuration that fills
  the dcache response queue to show its effect. No `model_parity` case is
  registered here: the gate runs at XLEN 32, which was not measured.
- Expected conflicts with sibling branches: the `TESTS` and `EXCLUDE_*` lines
  of `tests/regression/Makefile` and appended blocks in
  `ci/testcases/regression.yaml` / `dxa.yaml`.
- Still to do by hand: XLEN 32 runs, maintainer approval, push and open the PR.
