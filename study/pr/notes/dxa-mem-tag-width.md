# rtl: size the socket memory tag for the DXA's in-flight slots

> Status (wind-down, 2026-11-21): complete for its scope, verified at XLEN 64
> only. One commit `5d48b6c1a` on `pr/dxa-mem-tag-width`, nothing
> uncommitted. Not done: Beacon (`logic_gate`), xrt, independent review (none
> was spawned, so there are no open review findings), and the XLEN-32 runs of
> the new CI case (out of scope; one XLEN-32 rtlsim build check was made and is
> listed below as an extra).

## Summary

The socket memory arbiter's tag width (`SOCKET_MEM_TAG_WIDTH` in
`hw/rtl/VX_gpu_pkg.sv`) is the widest of the L1, texture and ray-cache memory
tags. The DXA engine shares that port but was not in the maximum: each worker
puts its in-flight slot id in the tag, the worker arbiter adds the worker
select, and the UUID rides along, and all of that had to fit in whatever the
L1 left. With a few more DXA slots or workers than the defaults it does not,
and the RTL fails to elaborate. This change puts the DXA's tag in the maximum
and guards the spare-bits lint sink in `VX_dxa_gmem_req.sv` for the case where
the tag fits exactly. Where the DXA fitted before, every width is unchanged.

## Motivation

At the default cache sizes the L1 memory tag is 8 bits at XLEN 64 (9 at XLEN
32) in a release build, 1 of them UUID, so the DXA has 7 bits (8) for
`CLOG2(VX_CFG_DXA_MAX_INFLIGHT) + CLOG2(VX_CFG_NUM_DXA_CORES)`. Debug builds
have the same budget (UUID 44, L1 tag 51 / 52). On upstream/master, rtlsim
`dxa_copy -d1`, XLEN 64, `-DVX_CFG_EXT_DXA_ENABLE` plus:

| workers x slots | DXA tag needs (with UUID) | socket tag | upstream/master |
|---|---|---|---|
| 8 x 8   | 7 | 8 | builds, PASSED, 6,470 cycles |
| 1 x 128 | 8 | 8 | fails to elaborate |
| 2 x 64  | 8 | 8 | fails to elaborate |
| 8 x 16  | 8 | 8 | fails to elaborate |
| 1 x 256 | 9 | 8 | fails to elaborate |
| 8 x 32  | 9 | 8 | fails to elaborate |
| 8 x 256 | 12 | 8 | fails to elaborate |

The smallest failing configurations are those whose slots x workers reach 128
(XLEN 64) or 256 (XLEN 32), e.g. 8 workers of 16 slots. The error, from
`VX_dxa_gmem_req.sv:308` (`UNUSED_VAR` of `tag.value[GMEM_TAG_VALUEW-1:TAG_W]`),
for 8 x 16:

```
%Warning-SELRANGE: hw/rtl/dxa/VX_dxa_gmem_req.sv:308:70: [3:4] Slice range has ascending bit ordering, perhaps you wanted [4:3]
    : ... note: In instance 'rtlsim_shim.vortex.g_clusters[0].cluster.g_sockets[0].socket.dxa_core.g_workers[7].worker.gmem_req'
%Warning-SELRANGE: hw/rtl/dxa/VX_dxa_gmem_req.sv:308:70: Selection index out of range: 5:4 outside 3:0
%Error: Exiting due to 4 warning(s)
```

When the slot id fits exactly there are no spare bits and the slice is empty;
when it does not fit, the same slice is reversed. The lint failure is the only
thing that stops the build: `VX_dxa_gmem_req`'s `STATIC_ASSERT` on the width
is an `initial` block. A build with only the lint guard and no width change
(8 x 32) builds and then stops at time 0:

```
[0] %Error: VX_dxa_gmem_req.sv:99: Assertion failed in TOP.rtlsim_shim.vortex.g_clusters[0].cluster.g_sockets[0].socket.dxa_core.g_workers[7].worker.gmem_req: gmem tag too narrow for slot encoding
```

(`results/pr_dxa-mem-tag-width_guardonly_rtl_copy_w8_i32`), so both halves are
needed.

Raising the in-flight count is the natural knob for DXA bandwidth (a
transfer is bounded by slots x latency), so this is reachable without exotic
settings.

## What changed

- `hw/rtl/VX_gpu_pkg.sv`: `DXA_MEM_TAG_WIDTH = UUID_WIDTH +
  CLOG2(VX_CFG_DXA_MAX_INFLIGHT) + CLOG2(VX_CFG_NUM_DXA_CORES)` joins the
  maximum that gives `SOCKET_MEM_TAG_WIDTH` (under `VX_CFG_EXT_DXA_ENABLE`).
  `CLOG2(workers)` equals `VX_dxa_core`'s `ARB_SEL_BITS(workers, 1)` for the
  socket's single DXA memory port.
- `hw/rtl/dxa/VX_dxa_gmem_req.sv`: the spare-tag-bits `UNUSED_VAR` is generated
  only when there are spare bits.
- `ci/testcases/dxa.yaml`: `dxa_copy-tagw`, `dxa_copy -d1` on SimX and rtlsim
  with 8 workers of 32 slots, at both XLENs (the file default). That needs 9
  bits with the UUID, against a socket tag of 8 at XLEN 64 (the width fix)
  and 9 at XLEN 32 (an exact fit: the lint guard), so the case covers both
  halves. At XLEN 64 it fails on upstream/master and passes on the branch;
  at XLEN 32 the rtlsim build fails on upstream/master (SELRANGE, `[4:5]`:
  the exact fit) and builds on the branch. The XLEN-32 run itself was not
  done (no RISC-V 32-bit toolchain on this machine). It is also the only case in
  the catalog with more than one DXA worker: the default worker count is
  `max(1, up(NUM_CORES / 8))` (integer division), 1 at 1-15 cores, 2 at
  16-23, 16 at 132, and no DXA case runs more than 6 cores.
- `CHANGELOG.md`: a Fixed entry.

## How verified

Build copies of the branch and of upstream/master (ccf811e11), XLEN 64 unless
noted, `--perf=7`, results under `vortex-exp/results/pr_dxa-mem-tag-width_*`.

Widths. A Verilator elaboration probe of `VX_gpu_pkg` printed
`SOCKET_MEM_TAG_WIDTH`, `SOCKET_MEM_ARB_TAG_WIDTH`, `L2/L3_MEM_TAG_WIDTH` and
`VX_MEM_TAG_WIDTH` before and after at XLEN 32 and 64, release and debug, for
the default configuration, every DXA shape `ci/testcases/dxa.yaml` uses
(1/2 cores, 2 clusters x 2 cores, L2, sockets of 2 and 3, 8 warps / 8 threads,
16 threads, TCU, WGMMA, 128 KB LMEM, wide loads), two non-DXA configurations,
and the synthesis/Beacon DUT configurations that include the DXA (`dxa`,
`gfx`, `gfxvm`, `tensor`, `gpu32_full`, with `-DSYNTHESIS -DVIVADO -DNDEBUG`).
All identical. Without `VX_CFG_EXT_DXA_ENABLE` the expression is textually the
old one.

Reproducer and fix (rtlsim unless noted):

| test | config | upstream/master | branch |
|---|---|---|---|
| dxa_copy -d1 | 8 x 8 | PASSED, 6,470 | (not run, widths identical) |
| dxa_copy -d1 | 1 x 128 | elaboration fails | PASSED, 6,470 |
| dxa_copy -d1 | 8 x 16 | elaboration fails | PASSED, 6,470 |
| dxa_copy -d1 | 8 x 32 | elaboration fails | PASSED, 6,470 |
| dxa_copy -d1 | 8 x 256 | elaboration fails | PASSED, 6,470 |
| dxa_copy -d1, SimX | 8 x 32 / 8 x 256 | PASSED, 6,466 / 6,466 | PASSED, 6,466 / 6,466 |
| sgemm2_dxa | 8 x 32 | (rtlsim does not build) | rtlsim 36,086, SimX 36,098 |
| sgemm2_dxa | 8 x 256 | (rtlsim does not build) | rtlsim 36,086, SimX 36,098 |
| rtlsim driver build only, XLEN 32 | 8 x 32 | elaboration fails (SELRANGE) | builds |

SimX has no tag width, so it runs these configurations on upstream/master
too; rtlsim and SimX agree to 0.06% (dxa_copy) and 0.03% (sgemm2_dxa) at the
larger settings. `dxa_copy -d1` is a single transfer on one worker, so its
cycle count does not depend on the worker or slot count; `sgemm2_dxa` spreads
its tiles over the workers (36,242 -> 36,086 cycles with 8 workers).

Defaults, rtlsim, cycle-identical to upstream/master:

| test | upstream/master | branch |
|---|---|---|
| dxa_copy -d1 (the `model_parity-copy` / `perf_gate-copy` case) | 1,624 instrs, 6,470 cycles | 1,624, 6,470 |
| dxa_copy -d2 | 15,968, 53,994 | 15,968, 53,994 |
| sgemm2_dxa | 12,656, 36,242 | 12,656, 36,242 |

Unit test `hw/unittest/dxa_core` (`make -C hw/unittest/dxa_core run`): default
PASSED on both. 1 worker x 256 slots, XLEN 64: upstream fails to build
(SELRANGE), the branch builds and PASSES (`..._base_ut_w1_i256`,
`..._br_ut_w1_i256`). A Verilator lint of the unit's top with the Makefile's
flags also gives: 1 x 256 fails on upstream at XLEN 32 as well; 1 x 128 fails
on upstream at XLEN 64 (exact fit) and is clean on the branch, and is clean on
both at XLEN 32, where it still has a spare bit. Script:
`vortex-exp/pr/scripts/dxa-mem-tag-width_probe/ut_lint.sh`.

`python3 ci/change_impact.py explain --base upstream/master --head HEAD`
(base revision's copy of the classifier, policy section 5): see Change class.

## Change class

R2 (`path:rtl`, `evidence:simx-rtl-lockstep`). The `gpu-pkg-types` detector
does not fire: the change is a localparam, not a typedef. The lockstep
evidence warning asks for confirmation that no cycles move: SimX models no
tag width, and the default widths are unchanged, so `model_parity-copy`
(dxa_copy -d1) keeps its numbers (6,470 cycles above, before and after).

Not done here:
- Beacon (`logic_gate`): CIRCT and foldsim are not installed on the machine
  this was checked on. The widths at every Beacon DUT configuration that
  contains the DXA are identical, and the only other edit wraps a lint sink in
  a generate block, so no register depth or area should move. Command for the
  maintainer: `ci/logic_gate.py -b dxa -b gfx -b gfxvm -b tensor --against-rev $(git merge-base HEAD origin/master)`.
- No perf or synthesis baseline should move; none was re-recorded.
- xrt was not run (the AFU wrappers take `VX_MEM_TAG_WIDTH` from the package
  and do not assume a value).
- Independent review.

## Other consumers of the socket tag

Every user takes it from the package: `VX_socket.sv` (socket arbiter, unit
MMU, `ASSIGN_VX_MEM_BUS_IF_EX` from each cache), `VX_dxa_core.sv` /
`VX_dxa_worker.sv` / `VX_dxa_gmem_req.sv`, the cluster (`L2_TAG_WIDTH =
SOCKET_MEM_ARB_TAG_WIDTH`), the PTW port (`L2_TAG_WIDTH` / `L3_TAG_WIDTH`),
`Vortex.sv` / `Vortex_axi.sv` / the AFU wrappers / `rtlsim_shim.sv`
(`VX_MEM_TAG_WIDTH`, adapted to the platform's tag through
`VX_mem_bank_adapter`), and the unit-test and synthesis wrappers
`hw/unittest/{dxa_core,gfx,tensor}`. None hard-codes a width. SimX has no
mirror of these widths.

## Dependencies

None. Independent of the worker-count report (the formula of
`VX_CFG_NUM_DXA_CORES` is untouched) and of the dispatch change; a branch that
edits `ci/testcases/dxa.yaml` or the `CHANGELOG.md` Fixed list will need a
trivial rebase.

## Open questions for the maintainer

- `hw/unittest/dxa_core` does not build with more than one DXA worker, on
  upstream/master and on this branch, for a reason unrelated to this change:
  `VX_mem_bus_arb.sv:97/126` WIDTHEXPAND in `VX_dxa_core_top.dxa_core.lmem_arb`
  (2 workers: "expects 3 bits ... SEL generates 2 bits"). The wrapper's LMEM
  bus looks sized for one worker. Left for a separate change.
- `sgemm2_dxa-5` .. `-10` in `ci/testcases/dxa.yaml` pass
  `-DVX_CFG_NUM_DXA_UNITS=4`, a knob that exists nowhere in the tree, so they
  run with one DXA worker like their neighbours.

---

## Notes for the sender

- Branch `pr/dxa-mem-tag-width`, one commit on upstream/master ccf811e11.
- The fork's commit was checked on the fork's h200s profile; nothing of that
  profile is in this branch, and every number above is from runs on this
  branch or on upstream/master.
- Probe and lint scripts and their width dumps (before/after):
  `vortex-exp/pr/scripts/dxa-mem-tag-width_probe/`. `probe.sh <tree> <xlen> -D...`
  elaborates `VX_gpu_pkg` alone and prints the widths (under a second).
- Not run: the XLEN-32 runs of `dxa_copy-tagw` on rtlsim and SimX (no
  riscv32 toolchain here; only the XLEN-32 rtlsim build was checked, job
  15982020, `pr/scripts/dxa-mem-tag-width_batch.sbatch`). From a configured
  XLEN-32 build tree:
  `CONFIGS="-DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_NUM_DXA_CORES=8 -DVX_CFG_DXA_MAX_INFLIGHT=32" ./ci/blackbox.sh --driver=rtlsim --app=dxa_copy --args=-d1`
  (and `--driver=simx`).
