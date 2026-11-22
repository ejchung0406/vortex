# tcu: B tile buffer rows follow the block geometry, not the LMEM bank count

## Summary

`VX_tcu_bbuf` assumes that one LMEM bank row of 32-bit words equals one row of
the block-major B layout: the `B_SUB_BLOCKS` dense blocks packed together,
`TCU_BLOCK_CAP` = `NUM_THREADS` words. It asserts
`B_BLOCK_WORDS * B_SUB_BLOCKS == NUM_BANKS` (`VX_tcu_bbuf.sv:100`), and sizes a
slot, the in-row select and the fetch address shift by `NUM_BANKS` and
`XLEN / 32`. That holds only when the bank count equals the warp size.

The `h100` and `h200` profiles set 32 threads over `VX_CFG_LMEM_NUM_BANKS=16`
banks of 8 B (the part's 128 B per clock; XLEN 64). Their bank row carries
exactly one 32-word B row, but the buffer expects 16 words per row. So every
rtlsim run with WGMMA on that shape stops at time 0. SimX has no such check:
it already keys B rows by the block geometry, so it runs the shape.

This change sizes the buffer by the layout instead:

- a slot stores `LROW_WORDS = B_BLOCK_WORDS * B_SUB_BLOCKS` words (one logical
  row);
- a physical bank row (`NUM_BANKS * XLEN / 32` words) holds
  `LROWS_PER_ROW` logical rows, and the dense row select, the sparse two-row
  split, the sparse two-fetch decision and the fetch-address shift use
  `LROWS_PER_ROW` instead of `XLEN / 32`;
- the assertion now checks what the buffer needs: a bank row holds a
  power-of-two number of whole logical rows. A bank row narrower than a
  logical row (for example XLEN 32 with 16 banks under 32 threads) is still
  rejected, with a message that says so.

In every configuration that elaborated before, `LROW_WORDS == NUM_BANKS` and
`LROWS_PER_ROW == XLEN / 32`, so every derived parameter, and the logic, is
the same as on master.

SimX needs no change. It already keys a block-major B row by the block
geometry (`kBSubBlocks` from `NUM_THREADS`), and its tile-buffer port issues
one LMEM access per bank row (`TcuTbuf::issue`, `LocalMem::DMA_PORTS`), so a
sparse block that spans two bank rows already takes two reads there. At 16
threads over 8 banks the RTL and SimX report the same `lmem_reads` (912) and
`tbuf_stalls` (2,964). SimX's `kBSparseTwoFetch` still names the old
condition (`2 * LMEM_NUM_BANKS` words, XLEN 32); it has no effect on these
shapes because the port splits the read anyway.

## Motivation

Reproducer on upstream/master (ccf811e11), XLEN 64, rtlsim:

```
CONFIGS="-DVX_CFG_NUM_THREADS=32 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4
  -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DVX_CFG_LMEM_NUM_BANKS=16
  -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=8 -DWGMMA_SS
  -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1"
./ci/blackbox.sh --driver=rtlsim --app=sgemm_tcu_wg

[0] %Error: VX_tcu_bbuf.sv:101: Assertion failed in TOP.rtlsim_shim.vortex.g_clusters[0].cluster.g_sockets[0].socket.g_cores[0].core.tcu_shared.wgmma.tbuf.bbuf: VX_tcu_bbuf assumes one bank-row per B_SUB_BLOCKS blocks
%Error: .../hw/rtl/tcu/VX_tcu_bbuf.sv:101: Verilog $stop
```

The same configuration passes on SimX (4,170 cycles). The sparse WGMMA test
(`sgemm_tcu_wg_sp`) fails the same way. At 16 threads over 8 banks (XLEN 64)
the sparse test stops one assertion earlier, at `VX_tcu_bbuf.sv:94` ("flat
sparse-B supports 1 or 2 logical bank-rows per block").

This is the minimal macro set of the profile's shared memory and tensor unit.
A full `h100` profile on rtlsim (64 warps, 4 issue slices, 132 cores) is too
large to build here and was not run; it carries the same 32 threads, 16 banks
and WGMMA, so it reaches the same assertion.

`STATIC_ASSERT` is empty in synthesis. A synthesized build of this shape (the
opt-in `gpu_h100` logic-gate build in `ci/testcases/logic_gate.yaml`) would
not stop: it would build 16-word slots and pick half rows, which is the wrong
data.

## What changed

- `hw/rtl/tcu/VX_tcu_bbuf.sv`: `LROW_WORDS`, `LROWS_PER_ROW` and
  `LG_LROWS_PER_ROW` replace `NUM_BANKS` as the slot size and `XLEN_RATIO` as
  the logical rows per bank row (in the dense select, the sparse logical-row
  select and fetch address, `SPARSE_TWO_FETCH`, and the response offsets). The
  assertion is the geometry check above. `XLEN_RATIO` remains only where the
  physical row width is meant (K-major lane arithmetic, response bounds).
  Comments updated.
- `ci/testcases/tensor_wg.yaml`: `sgemm_tcu_wg-lmem16` and
  `sgemm_tcu_wg_sp-lmem16`, simx and rtlsim, XLEN 64 only, 32 threads over 16
  banks.
- `docs/designs/tensor_core_wgmma_engine.md`: a bank row is
  `LMEM_NUM_BANKS x XLEN/8` bytes (the doc said `NUM_THREADS x XLEN/8`, true
  only at the default banking); 6.3 says what a B row is and that a bank row
  carries one or more.

Storage (two slots of `B_BUF_WORDS` 32-bit words, LUTRAM, `VX_dp_ram`
`SIZE=1`), computed from the parameters (not a synthesis run):

| shape | master | this branch |
|---|---|---|
| banks == threads (every configuration that elaborated) | 2 x NT x 32 bits (512 / 1,024 / 2,048 at NT 8 / 16 / 32) | same |
| 32 threads, 16 banks, XLEN 64 | does not elaborate in simulation; in synthesis 2 x 16 x 32 = 1,024 bits, wrong data | 2 x 32 x 32 = 2,048 bits; the 2:1 half-row select is gone (one row per bank row) |
| 16 threads, 8 banks, XLEN 64 | does not elaborate in simulation | 2 x 16 x 32 = 1,024 bits |

## How verified

All runs at XLEN 64: this host has no RV32 toolchain, so no XLEN-32 run was
made (`not run`). Builds from a clean copy of the branch and of
upstream/master; `results/<name>` is
`/storage/project/r-hkim358-0/echung67/vortex-exp/results/pr_tcu-bbuf-geometry_<name>`.

**The failing shape passes, and SimX tracks rtlsim** (cycles, rtlsim / SimX,
instructions equal on both drivers):

| test | config | master rtlsim | branch rtlsim | branch SimX | gap | results |
|---|---|---|---|---|---|---|
| `sgemm_tcu_wg` fp16 SS NRC8 (CI case shape) | 32T, 16 banks | assertion at 0 | 4,177 PASS | 4,170 PASS | -0.2% | `base_ss_b16_x64_rtlsim`, `br_ss_b16_x64_*` |
| `sgemm_tcu_wg_sp` NRC8 (CI case shape) | 32T, 16 banks | assertion at 0 | 13,283 PASS | 13,212 PASS | -0.5% | `base_sp_b16_x64_rtlsim`, `br_sp_b16_x64_*` |
| `sgemm_tcu_wg` fp16 SS NRC32, -m128 -n128 -k128 | 32T, 16 banks, 8 LSU lanes, DXA | assertion at 0 | 92,712 PASS | 92,470 PASS | -0.3% | `base_ss_b16_rtlsim`, `br_ss_b16_*` |
| `sgemm_tcu_wg_sp` NRC32, -m128 | same | assertion at 0 | 66,916 PASS | 66,492 PASS | -0.6% | `base_sp_b16_rtlsim`, `br_sp_b16_*` |
| `sgemm_tcu_wg` FEDP2K RS NRC8, -m128 | same | FEDP2K_BASE | FEDP2K_BR | 201,187 PASS | FEDP2K_GAP | `*_f2k_b16_*` |
| `sgemm_tcu_wg_sp` NRC8 | 16T, 8 banks | assertion (line 94) at 0 | 31,801 PASS | 31,817 PASS | +0.1% | `base_sp_t16b8_rtlsim`, `br_sp_t16b8_*` |
| `sgemm_tcu_wg_sp` NRC8, -m128 | 16T, 8 banks | not run | 241,454 PASS | 238,865 PASS | -1.1% | `br_sp_t16b8_m128_*` |
| `sgemm_tcu_wg` fp16 SS NRC8 | 16T, 8 banks | not run | 4,626 PASS | 4,621 PASS | -0.1% | `br_ss_t16b8_*` |

The 32-thread -m128 rows use the fork's measurement shape; the research fork
this came from measured 92,712 / 92,470, 66,916 / 66,492 and 199,345 /
201,187 (fork measurements, on a build that also has the unmerged
`feature/tmem-dataflow` branch). This branch reproduces the dense and sparse
numbers exactly.

SimX columns come from the final branch, whose SimX sources are
upstream/master's (`v2_*_simx`; equal to `base_*_simx` where both ran). TCU
counters (`--perf=11`) at 16 threads / 8 banks sparse: rtlsim
`tbuf_stalls=2964, tbuf_cache_hits=450, lmem_reads=912`; SimX
`2964, 451, 912` (`br_sp_t16b8_p11_rtlsim`, `v2_sp_t16b8_p11_simx`).

**Nothing moves where banks == threads** (the shapes of the existing
`tensor_wg` parity and rtlsim cases, run at XLEN 64): cycles identical to
upstream/master on both drivers, instructions equal, all PASS.

| case (tensor_wg shape) | rtlsim master = branch | SimX master = branch |
|---|---|---|
| `model_parity-wgmma-int8` (8T, int8, NRC32) | 11,164 | 11,157 |
| `model_parity-wgmma-fedp2k-rs` (8T, FEDP2K, DPI, -m64) | 138,458 | 138,219 |
| `model_parity-wgmma-ldm` (8T, LDST_MATRIX, DPI, -m64) | 127,935 | 127,651 |
| `sgemm_tcu_wg_sp-4` (8T sparse) | 89,052 | 89,463 |
| `sgemm_tcu_wg_sp-6` (32T sparse) | 12,885 | 12,793 |
| `sgemm_tcu_wg-7` (32T tf32, NRC16) | 5,371 | 5,363 |
| `sgemm_tcu_wg-b-offset` (8T, K-major B with offset) | 7,304 | 7,276 |
| `sgemm_tcu` (default) | 83,383 | does not build on master (below) |
| `vecadd` (default) | 1,641 | 1,631 |

Results: `{base,br}_eq_{pint8,pf2k,pldm,sp4,sp6,wg7,boff}_{rtlsim,simx}`,
`{base,br}_def_{sgemm_tcu,vecadd}_{rtlsim,simx}`.

Not run:
- XLEN 32 anything (no RV32 toolchain on this host). The `model_parity` gate
  runs at XLEN 32 only (`ci/testcase.py` pins check cases to 32), where the
  32-thread / 16-bank shape is still rejected by design (a 16-word bank row is
  narrower than a 32-word B row). So this branch adds functional simx + rtlsim
  cases at XLEN 64 instead of a `model_parity` case; the parity numbers above
  were measured by hand.
- Beacon (`ci/logic_gate.py -b tcu --against-rev $(git merge-base HEAD
  upstream/master)`): CIRCT and foldsim are not installed here. The `tcu`
  logic-gate build is 16 threads over the default 16 banks, where the logic is
  unchanged; the opt-in `gpu_h100` build changes (it now builds 2,048-bit
  slots), and it has no recorded baseline.
- xrt.

## Change class

`change_impact.py explain`: **R2** (`path:rtl`, `evidence:simx-rtl-lockstep`
warn: "RTL logic changed with no SimX change"); no R3 detector (no knob,
interface or existing test case changed; two cases added). The lockstep
evidence check sees RTL without SimX: the parity table above is the answer
(no cycle moves where the RTL elaborated before; SimX already models the new
shapes).
Still required by the policy: a review of every hunk with `rtl-review` by
someone other than the author (an agent review was started and returned no
report; not done), Beacon on `tcu`, and maintainer approval. No baseline moves (`ci/baselines/**` untouched).

## Dependencies

None. Independent of `feature/tmem-dataflow`; the fork's version of this fix
was made on top of that branch, and this one is re-implemented on master's
`VX_tcu_bbuf.sv`.

## Open questions for the maintainer

1. XLEN 32 with fewer banks than threads (32 threads, 16 banks of 4 B) is
   still unsupported: a B row spans two bank rows and would take two dense
   reads. No profile sets it (every profile with WGMMA is XLEN 64). Should the
   configuration system reject it before elaboration (there is no constraint
   mechanism in `VX_config.toml` or `ci/gen_config.py` today), and should
   SimX `static_assert` the same rule so it stops running a shape the RTL
   rejects? `ci/profile_check.py` is not the place: it compares a profile
   with its measured reference numbers, not with RTL constraints.
2. `model_parity` cases are XLEN 32 only, and every profile is XLEN 64. A
   profile-shaped parity case cannot exist under that rule. Is an XLEN-64
   exception for profile shapes wanted?
3. Found on the way, not fixed here: on upstream/master SimX does not build
   with `VX_CFG_EXT_TCU_ENABLE` and WGMMA off. `sim/simx/tcu/tcu_unit.cpp:1240`
   (`wgmma()`) calls `decode_descs`, which is defined only under
   `VX_CFG_TCU_WGMMA_ENABLE` (from #488). `blackbox.sh --driver=simx
   --app=sgemm_tcu` fails with "'class vortex::TcuUnit::Impl' has no member
   named 'decode_descs'" (`results/pr_tcu-bbuf-geometry_base_def_sgemm_tcu_simx`).

---

## Notes for the sender

- Branch `pr/tcu-bbuf-geometry`, one commit on upstream/master ccf811e11.
- Build copies: `vortex-exp/src_pr_tcu-bbuf-geometry` (branch) and
  `src_pr_tcu-bbuf-geometry_base` (upstream/master), each with a `.pr_rev`
  file naming the source revision. Run script:
  `vortex-exp/pr/scripts/bbuf_point.sbatch` (run_point.sbatch plus an XLEN
  choice); table: `vortex-exp/pr/scripts/bbuf_collect.sh`.
- The FEDP2K rtlsim build at 32 threads needs more than 64 GB to verilate
  (first attempts killed by the OOM killer: `*_f2k_b16_rtlsim_oom64g`); the
  rerun used `--mem=180G`.
- Commit trailers follow the effort brief (Claude Fable 5.2), not the session
  default.
- Before sending: run the two new CI cases and the XLEN-32 `tensor_wg` cells on
  a machine with the RV32 toolchain, and Beacon on `tcu`.
- The side finding in open question 3 deserves its own small PR.
