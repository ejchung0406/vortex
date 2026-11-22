# tests: add gemm_block, a decode-shaped GEMM that stages its weights into shared memory

Branch `pr/tests-gemm-block`, commit 076f64d3b on upstream/master ccf811e11.

## Summary

A new regression test, `tests/regression/gemm_block`. It computes C = A x B
with few rows in A (GEMM_M, default 16: one decode step's batch). The CTA's
warps are laid along N and the CTA walks K once. Each K chunk of the CTA's
weight columns is staged into shared memory in one of two ways:

- `-s 0`: the CTA's own coalesced loads.
- `-s 1`: the DXA engine, with `-d` chunk buffers (1..4) in flight.

The test prints the kernel's rate: weight bytes per cycle (total and per
core) and multiplies per cycle. It checks every output bit for bit against
the device's own arithmetic.

## Motivation

The tensor GEMM tests lay a CTA's warps along M. At M = 16 they refuse the
shape or run far longer. Measured on this branch (SimX, XLEN 64, fp16, with
`-DVX_CFG_TCU_WGMMA_ENABLE` added because of the build break under
Dependencies):

| shape / config | test | cycles |
|---|---|---|
| M=16 N=256 K=512, 1 core, 8 threads, 4 warps | gemm_block -s0 | 430,708 |
| | gemm_block -s1 (depth 2) | 270,675 |
| | sgemm_tcu_wg | 6,509,013 (24x) |
| | sgemm2_tcu | refused: "M (16) must be a multiple of cta_M=32" |
| | sgemm2_tcu -w1 | 2,708,745, then fails its own fp32 check (small drift) |
| | sgemm_tcu_wg_dxa | refused: cta_M=32 |
| | sgemm_tcu (plain WMMA, no staging) | 181,417 |
| M=16 N=1024 K=1024, 4 cores + L2, 8 threads, 4 warps | gemm_block -s0 | 844,615 (2.48 B/cycle) |
| | gemm_block -s1 -d2 | 533,340 (3.93 B/cycle) |
| | gemm_block -s1 -d4 -c32 | 625,706 |
| | sgemm_tcu | 451,458 |
| | sgemm_tcu_wg | 13,123,689 |
| | sgemm2_tcu, sgemm_tcu_wg_dxa | refused (cta_M=32) |

At these small configurations the plain WMMA test `sgemm_tcu` is faster than
gemm_block. The point of this test is not peak speed. It is the one test
that runs both shared-memory staging paths, coalesced loads and the DXA ring
with several copies outstanding, at the decode shape.

Results: `vortex-exp/results/pr_gb_cmp_*` and `pr_gb_cmp2_*`.

## What changed

- `tests/regression/gemm_block/{Makefile,common.h,kernel.cpp,main.cpp}`
  (new):
  - The Makefile enables TCU and DXA, as `sgemm_tcu_wg_dxa`'s does.
  - The kernel uses `vortex::barrier`, `vx_dxa_issue_2d_wg` and `wmma_context`.
  - Data type: fp16 inputs by default; the TCU's FP16 support is on in
    `VX_config.toml`, so no extra CONFIGS are needed. bf16, fp8, bf8 and tf32
    are selectable with `-DITYPE`. fp32 output only.
  - Check: bit-exact against a reference that applies the device's
    arithmetic uop by uop. On TFR that is TfrFedp, padded to the
    FEDP's fedpK words. On the other datapaths it is fp32 sums of each
    word, then of the words, then of C. This is the method `sgemm_tcu_sp`
    adopted in #473.
  - Why not a tolerance: the `sgemm_tcu` family uses a 1% relative tolerance
    for low-precision inputs. With inputs in [0, 1] and K = 4096, one
    wrong element moves a sum by about 0.02%, so a 1% check passes it. Any
    tolerance loose enough for the accumulation drift at long K misses such
    errors.
- `tests/regression/Makefile`: `gemm_block` added to TESTS.
- `ci/testcases/dxa.yaml`: `gemm_block-load` (`-s0`) and `gemm_block-dxa`
  (`-s1`), simx and rtlsim, default shape, configs
  `-DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_EXT_TCU_ENABLE`. `ci/testcase.py lint`: OK.
- No doc lists the tensor tests one by one (checked `docs/`). Test-only
  commits upstream carry no CHANGELOG entry, so this one has none.

## How verified

Verified at XLEN 64 only.

| what | driver | config | result | results dir |
|---|---|---|---|---|
| gemm_block -s0 | rtlsim | default 4x4, TCU TFR | PASSED bit-exact, 269,498 cycles | pr_gb_rtl_s0 |
| gemm_block -s1 | rtlsim | default 4x4, TCU TFR | PASSED, 192,420 | pr_gb_rtl_s1 |
| gemm_block -s1 | rtlsim | 4x4, `-DVX_CFG_TCU_TYPE=DPI` (non-TFR reference branch) | PASSED, 192,420 | pr_gb_rtl_dpi_s1 |
| gemm_block -s0 / -s1 | rtlsim | 4x4 + WGMMA | PASSED, 269,498 / 192,420 | pr_gb_rtl_wg_s0, _s1 |
| gemm_block -s0 / -s1 | simx | 1 core, 8 threads, +WGMMA, N=256 K=512 | PASSED, 430,708 / 270,675 | pr_gb_cmp_gemm_block_s0, _s1 |
| gemm_block -s0 / -s1 / -s1 -d4 | simx | 4 cores + L2, 8 threads, +WGMMA, N=K=1024 | PASSED | pr_gb_cmp2_gemm_block_* |
| gemm_block GEMM_M=256 -s0 / -s1 | simx | 1 core, 16 threads x 16 warps, N=1024 K=256 | PASSED | pr_gb_repro_a_nt16_s0, _s1 |
| gemm_block GEMM_M=256 -s1 | simx | 1 core, 32 threads x 64 warps, N=1024 K=256 | PASSED | pr_gb_repro_a_nt32_s1 |
| registered CI cases, simx leg, plain configs | simx | default | not run: SimX does not build on upstream/master without WGMMA (see Dependencies) | - |
| simx at the default 4x4 shape (parity with the rtlsim numbers) | simx | default + WGMMA | not run (local run stopped at wind-down) | - |
| bf16, fp8, tf32, DPI on simx, `-d1`, `-s2` reject | simx | default + WGMMA | not run (same) | - |
| `make -C tests/regression` builds everything | - | - | not run | - |
| XLEN 32 | - | - | not run: no riscv32 toolchain in this environment | - |

Commands not run, to run from a configured XLEN 64 build:

- `CONFIGS="-DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE" ./ci/blackbox.sh --driver=simx --app=gemm_block --args="-s0"` (and `-s1`, `-s1 -d1`, `-s1 -d4 -c32`)
- the same with `-DITYPE=bf16`, `-DVX_CFG_TCU_FP8_ENABLE -DITYPE=fp8` and `-DVX_CFG_TCU_TF32_ENABLE -DITYPE=tf32`
- `make -C tests/regression`

No `model_parity` case is registered. Its simx leg cannot build on
upstream/master today, and SimX was not measured at the rtlsim shape.

## Change class

R2 (`path:build-config`: `tests/regression/Makefile` and the new test's
Makefile). The detectors flag nothing; adding a CI case is not a finding.

What the policy still requires that was not done:
- a review of every changed hunk by someone else (no independent review ran);
- maintainer approval.

Beacon, perf baselines and xrt do not apply (no RTL change, no baseline
moves).

## Dependencies on other PR branches

- **Upstream build break, not fixed on any branch I know of.** On
  upstream/master, SimX does not build with `VX_CFG_EXT_TCU_ENABLE` and
  without `VX_CFG_TCU_WGMMA_ENABLE`:
  - Error: `sim/simx/tcu/tcu_unit.cpp:1239: 'class vortex::TcuUnit::Impl' has no member named 'decode_descs'`.
  - Cause: eaea0c79a (#488) calls `decode_descs()` from the unguarded
    `Impl::wgmma()`, but defines it inside the `#ifdef
    VX_CFG_TCU_WGMMA_ENABLE` block (lines 760-1129).
  - Effect: the simx legs of both new cases fail until this is fixed. So do
    upstream's own non-WGMMA SimX TCU cases, such as `sgemm_tcu-1`.
  - Seen in `results/pr_gb_fork_s0`.
- **`runtime-cp-host-tag`.** The reproducer `gemm_block -n5120 -k4096 -s1`
  (one 42 MB upload of B, 1 core, 8 threads) segfaults on this branch, after
  "wait for completion" (`results/pr_gb_repro_b_hosttag`). That is
  consistent with the host-region alias that branch fixes. Not re-run on that
  branch.
- **`simx-dcache-order`.** The bug did not reproduce on this branch at the
  sizes tried. Small versions with GEMM_M=256 (16 tiles, heavy spills), DXA
  staging, at 16x16 and 32x64 threads x warps all passed bit-exact. So
  M = 256 with DXA on large configurations stays a known-risk shape (fork
  measurement: 1-30 of 262,144 outputs wrong on the fork's H200-sized
  profile). It was not confirmed here.

## Open questions for the maintainer

- Should the `decode_descs` guard be fixed in this PR or separately? This PR
  leaves it alone.
- Is a `model_parity` case wanted once SimX builds? rtlsim numbers for it
  are above.

---

## Notes for the sender

- Fork commits squashed into the single commit: 3d21214d2, 8e8d21228,
  9167e8e3e, 69e11596b, b56fc65e4.
- Removed from the fork's research version:
  - the two-role split (`-b`, `-t`, `-o`, `-R`), `-r` rounds and `-P`;
  - the idle-CTA polling;
  - the window, steady and per-core quartile reports;
  - the 1% tolerance.
- The fork's version as-is was not run on upstream: its build hit the SimX
  break above, and the rerun with WGMMA was stopped at wind-down. The
  shipped version builds and runs on upstream.
- Build copy: `vortex-exp/src_pr_tests-gemm-block_wip` (branch content
  plus a `gemm_block_fork` directory that is not in the branch).
  `src_pr_tests-gemm-block` is upstream plus the fork's test as-is.
- Scripts: `vortex-exp/pr/scripts/gemm-block_batch.sbatch`,
  `gemm-block_local.sh`, `gb_catalog_list.py`, lists in `gb_lists/`.
