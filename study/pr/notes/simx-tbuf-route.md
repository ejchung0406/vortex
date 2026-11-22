# simx: route a tile-buffer word by its operand, not by comparing descriptor bases

## Summary

SimX's WGMMA operand gather (`load_lmem_word` in `sim/simx/tcu/tcu_unit.cpp`)
decided whether a word came from the A or the B tile buffer by comparing the
descriptor's base with the current A descriptor's base. When A and B start at
the same shared-memory address, B's reads go to the A buffer. Once B's rows
reach past A's stripe, the A buffer misses and the run aborts with
`Error: TCU buffer miss`. The RTL keeps separate A and B buffers (one
`VX_tcu_abuf` per block and one shared `VX_tcu_bbuf`, each its own LMEM
master), so rtlsim runs the same program correctly. The caller knows which
operand it is reading, so it now passes that in.

## Motivation

One legitimate kernel that makes A and B share a base is C = A * A^T (SYRK,
Gram matrix, self-attention scores with Q == K) where B is read in place from
A's rows: B as K-major rows of a row-major A, with the same stride. Warp 0's A
descriptor and the B descriptor are then the same address. At 8 threads,
xtileM is 8 and xtileN is 16 at NRC 16, so B reads A rows 8..15. Warp 0's A
buffer holds only rows 0..7 of the current k-stripe.

On upstream/master (ccf811e11) with the test added by this PR:

| run | driver | result | cycles |
|---|---|---|---|
| `sgemm_tcu_wg` `WGMMA_SYRK`, NRC 16 | simx | **abort**: `TCU buffer miss at 0x1fffc0080` (LMEM offset 0x80 = A row 8 at a 16 B stride) | - |
| same | rtlsim | PASSED | 4,864 |

With this PR:

| run | driver | result | cycles |
|---|---|---|---|
| `sgemm_tcu_wg` `WGMMA_SYRK`, NRC 16 | simx | PASSED | 4,857 |
| same | rtlsim | PASSED | 4,864 (unchanged, no RTL change) |

SimX and rtlsim agree to 0.14% on cycles and match exactly on instruction
count (1,824).

When the bases are equal but every B row falls inside A's stripe (NRC 8 at 8
threads, where xtileN == xtileM), the old code read B from the A buffer. That
returned the same bytes, so it did not fail. From reading the code (not run): with a
sparse B, the misrouted read also takes the A path's layout flags
(`sparse_b = false`, `sparse_a_layout = true`), so it gathers from the wrong
addresses.

## What changed

- `sim/simx/tcu/tcu_unit.cpp`: `load_lmem_word` takes `is_a`, and its four
  callers in `wgmma()` pass `true` for A and `false` for B. The
  `cur_a_desc_base_` member and the line in `wgmma()` that set it are
  removed. Timing is unchanged: tile-buffer fills and stalls are computed in
  `tbuf_step()` from the descriptors and never used this routing. Only which
  buffer the functional gather reads from changes.
- `tests/regression/sgemm_tcu_wg/{kernel.cpp,main.cpp,common.h}`: new
  `WGMMA_SYRK` variant. B's descriptor points at A's rows `tile_col..` in
  shared memory with A's stride, and B is not staged. The host sets
  B = A^T and checks the usual reference. It needs `WGMMA_RMAJOR_A`
  (`#error` otherwise), M equal to one CTA's rows and N <= M (the host checks
  this), and an element type of 8 bits or more (`static_assert`). `pB` and
  `B_smem` are marked `[[maybe_unused]]` because the variant leaves them unused.
- `ci/testcases/tensor_wg.yaml`: new case `sgemm_tcu_wg-syrk` (simx and
  rtlsim, 8 threads, 8 warps, issue width 4, fp16, NRC 16, SS, row-major A).
- `CHANGELOG.md`: a `Fixed` entry.

## How verified

Build trees: the branch (`src_pr_simx-tbuf-route`), plain upstream/master
(`src_pr_simx-tbuf-route_base`), and upstream/master with only this PR's test
and CI files (`src_pr_simx-tbuf-route_repro`). All are XLEN 64, run through
`ci/blackbox.sh --perf=1`. Result directories are under `vortex-exp/results/`.

Reproducer (CI case configs:
`-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=16 -DWGMMA_SS -DWGMMA_RMAJOR_A -DWGMMA_SYRK -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1`):

| tree | driver | result | cycles | results dir |
|---|---|---|---|---|
| upstream + test | simx | failed as expected (abort, rc=2) | - | `pr_simx-tbuf-route_syrk_repro_simx` |
| branch | simx | PASSED | 4,857 | `pr_simx-tbuf-route_syrk_branch_simx` |
| upstream + test | rtlsim | PASSED | 4,864 | `pr_simx-tbuf-route_syrk_repro_rtlsim` |
| branch | rtlsim | PASSED | 4,864 | `pr_simx-tbuf-route_syrk_branch_rtlsim` |

Default runs against plain upstream/master, all on SimX. Every one has
identical instructions and cycles on both trees:

| app | configs | instrs | cycles (base = branch) | results dirs |
|---|---|---|---|---|
| `sgemm_tcu_wg` | Makefile defaults (SS, block-major) | 6,400 | 16,329 | `pr_simx-tbuf-route_wg_{base,branch}` |
| `sgemm_tcu_wg` | CI `sgemm_tcu_wg-a-offset` (row-major A, offset descriptor) | 4,488 | 6,298 | `pr_simx-tbuf-route_aoff_{base,branch}` |
| `sgemm_tcu_wg` | `WGMMA_RS` (A from registers), NRC 8, 8 threads | 5,712 | 7,367 | `pr_simx-tbuf-route_rs_{base,branch}` |
| `sgemm_tcu_wg_dxa` | defaults | 42,944 | 135,796 | `pr_simx-tbuf-route_wgdxa_{base,branch}` |
| `sgemm_tcu_wg_sp` | defaults (sparse) | 106,176 | 281,541 | `pr_simx-tbuf-route_wgsp_{base,branch}` |
| `sgemm_tcu_wg_sp_dxa` | defaults (sparse + DXA) | 50,208 | 235,174 | `pr_simx-tbuf-route_wgspdxa_{base,branch}` |
| `flash_attn_tcu_wg` | defaults | 609,508 | 1,645,284 | `pr_simx-tbuf-route_flash_{base,branch}` |
| `sgemm_tcu` | `-DVX_CFG_TCU_WGMMA_ENABLE` (WMMA in a WGMMA build) | 48,032 | 83,506 | `pr_simx-tbuf-route_tcuwgb_{base,branch}` |

Verified at XLEN 64 only. The new CI case also lists XLEN 32; that leg is not
run (one attempt, job 15981910, failed after 14 s before writing results and
was not investigated; XLEN 32 is out of scope for this effort). Also not run:
xrt and the full `tensor_wg` category.

State: complete. The bug reproduces on upstream/master; the fix is committed
(94d9d2bec). No independent review was run, so no review findings exist and
the review itself is still open.

## Change class

`python3 ci/change_impact.py explain --base upstream/master --head HEAD`:
**R2**, reasons `path:simx-model` and `evidence:simx-rtl-lockstep`. The
lockstep warning ("SimX model changed with no RTL change") is answered by the
tables above. The change is functional only and corrects SimX toward rtlsim:
default cycles are identical, and on the new case SimX now passes as rtlsim
already did (4,857 vs 4,864).

What R2 still requires that this branch could not provide: a review of every
changed hunk with the `simx-review` skill by someone other than the author
(only the author has checked it), and maintainer approval. No Beacon or
logic-depth check applies (no RTL change), and no perf baseline moves: the
default cycles above are unchanged, and the new case is not a perf-gate case.

## Dependencies

None. The branch is based on upstream/master ccf811e11.

## Open questions for the maintainer

- Is `WGMMA_SYRK` as a variant of `sgemm_tcu_wg` the right home, following
  `A_OFF_K`/`B_OFF_K` from #475, or would you rather have a separate directed
  test?
- `upstream/feature/tmem-dataflow` carries the same base comparison
  (`git show upstream/feature/tmem-dataflow:sim/simx/tcu/tcu_unit.cpp`, lines
  2208/2571/2665). After this lands, that branch needs the same change when it
  is rebased.

---

## Notes for the sender

- Branch `pr/simx-tbuf-route`, one commit (94d9d2bec) on upstream/master ccf811e11.
- Origin: the fork found and fixed this (0c1054236) on top of its merge of
  `feature/tmem-dataflow`, where the trigger was a tensor-memory kernel whose
  unused A lanes past M sat next to the weights. That scenario does not exist
  on master. The routing code it fixed does, unchanged, so this branch
  re-implements the same change on master and adds a master-only reproducer
  (SYRK). The bug is present on `upstream/feature/tmem-dataflow` itself, not
  introduced by fork-only changes.
- Unrelated pre-existing upstream break found on the way: plain
  upstream/master SimX does not compile with `-DVX_CFG_EXT_TCU_ENABLE` and
  WGMMA off (default `sgemm_tcu`). `tcu_unit.cpp:1239` in `wgmma()` calls
  `decode_descs`, which is defined only under `#ifdef
  VX_CFG_TCU_WGMMA_ENABLE`. This came in with eaea0c79a (#488)
  (`results/pr_simx-tbuf-route_tcu_base/run.log`; the branch fails the same
  way). It is not addressed here and is worth its own report or PR.
- Overlaps with sibling branches: `sim/simx/tcu/tcu_unit.cpp` (any TCU or
  tile-buffer branch, for example tcu-bbuf-geometry), `ci/testcases/tensor_wg.yaml`,
  `tests/regression/sgemm_tcu_wg/*` and `CHANGELOG.md` (`Fixed` list). The
  hunks are small and local, so expect textual conflicts at most.
