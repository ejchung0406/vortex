# Tensor Memory (TMEM) and the `tc.*` Engine

**Scope:** the per-core tensor memory, the single-thread asynchronous MMA
engine that writes it (`tc.mma`, `tc.cp`, `tc.commit`), the warp transfers
(`tmem.ld` / `tmem.st`), allocation, and the launch support (TMEM budget,
block-aligned warp placement). RTL:
[`VX_tcu_tmem.sv`](../../hw/rtl/tcu/VX_tcu_tmem.sv),
[`VX_cta_dispatch.sv`](../../hw/rtl/core/VX_cta_dispatch.sv),
[`VX_kmu.sv`](../../hw/rtl/VX_kmu.sv). SimX:
[`tcu_unit.cpp`](../../sim/simx/tcu/tcu_unit.cpp),
[`cta_dispatcher.cpp`](../../sim/simx/cta_dispatcher.cpp). Software:
[`vx_tmem.h`](../../sw/kernel/include/vx_tmem.h).

The capability set follows Blackwell's `tcgen05`: accumulators in TMEM
instead of registers, a single thread issuing asynchronous MMAs, completion
signalled to a barrier, sparse metadata in TMEM, A from TMEM, and
shared-memory-to-TMEM copies. The hardware is built from the TCU's existing
pieces (the WGMMA tile buffers, the blocks' FEDP arrays, the barrier unit);
what is TMEM-specific is the storage, the shape logic on the transfer path,
a small op queue and the allocator.

---

## 1. Geometry and storage

TMEM is `NUM_TCU_BLOCKS * xtileM` lanes by `VX_CFG_TCU_TMEM_COLS` 32-bit
columns. A lane is an M row of an MMA and a column an N element. TCU block
`b` computes, and stores, lanes `[b*xtileM, (b+1)*xtileM)`.

Storage is **fragment-major**. A storage row holds one C/D fragment: `tcM`
lanes by `tcN` columns, word `i*tcN + j` = lane `m*tcM + i`, column
`g*tcN + j`. Column group `g`'s two M steps are row `g` of two RAMs per
block (`VX_dp_ram`, `NT*32` bits wide, `COLS/tcN` deep). An MMA step reads
C from, and writes D to, one row of one RAM with identity wiring; the
transfer shapes (§3) are the only place data is rearranged. D, metadata and
A-from-TMEM columns, and transfer column addresses, are multiples of `tcN`.

At XLEN=64, `tmem.ld` NaN-boxes the 32-bit cells into the FP registers.

## 2. ISA

| Instruction | Operands | Semantics |
|---|---|---|
| `tc.mma` (custom-0, funct7 6, funct3 4) | rs1 idesc, rs2 A, rs3 (rd field) B | One K step: D (+)= A·B, asynchronous, from thread 0. |
| `tc.cp` (funct3 2) | rs1 `{cols, col}`, rs2 source | Copies pre-packed fragment rows from shared memory, in order with `tc.mma`. |
| `tc.commit` (funct3 1) | rs1 barrier, rs2 arrival count | Arrives once the core's earlier `tc.mma` / `tc.cp` have completed. |
| `tmem.ld` / `tmem.st` (funct7 5, funct3 0/1, +4 = fragment shape) | rs1 taddr, rd FP base, rs2 count-1 | Warp transfer to/from the issuing block's lanes. |
| `tmem.alloc` (funct3 6) | rs1 columns | First column of a power-of-two run, owned by the CTA; waits until free. |
| `tmem.dealloc` (funct3 7) | rs1 column, rs2 columns | Frees the CTA's run. |

**Instruction descriptor** (`tc.mma` rs1, `tmem::make_idesc<IType>`): `fmt_s` [4:0],
sparse [5], A in TMEM [6], `acc_init` [7] (D is overwritten, Blackwell's
`enable_input_d` = 0), `n_frag - 1` [14:8], D's first fragment group [22:15],
metadata column [31:23]. The output format is fp32 (int32 for integer
inputs). A and B are the WGMMA shared-memory descriptors
(`tensor::smem_matrix_desc`, offset and leading bytes; B's leading bytes 0
select block-major B); with A in TMEM (a `tmem::tmem_col` operand, which sets
bit 6), rs2 is A's TMEM column. There is no descriptor state: every `tc.mma`
carries its whole description, and software loops over K, one instruction
per `fedpK` words of K, as on Blackwell.

**`tc.cp` source** is TMEM's storage order: column group by group, block by
block, M step by M step, NT words each. The copy moves whole LMEM bank rows
(the source and size are bank-row multiples), each response writing whole
fragment rows with fixed wiring.

**Sparse metadata** is stored pre-packed: for each K step a lane holds, in
one fragment group from the metadata column, the `VX_tcu_sp_mux` rows of the
step's lower then upper `TC_K` words. It is read once per instruction.

**MX scale factors** (`fmt_s` an MX format) use the same side column. A step
takes `SF = TCU_MX_MAX_SF` scales per A row and per B column
(`tensor::mx_max_fedp_sf(fedpK)`); scale `s` covers the step's K elements
from `s * step_elems / SF`. From the side column:

- A: lane `l` (an M row), columns `side + s/4`, byte `s % 4`: one fragment
  group, read by the prologue with the metadata's read and latched.
- B: a dense byte stream, byte `q = col * SF + s`, in rows of `NT*4` bytes;
  row `r` is M step 1 of block `r % NUM_TCU_BLOCKS`, at group
  `side/TC_N + 1 + r / NUM_TCU_BLOCKS`. A step `(m = 0, n)` whose `n` starts a
  row reads it on the RAM its C read leaves free; every block takes it, as
  they all take B.

So the scales cost no MMA cycles and no per-step latch beyond one row. A
kernel copies them in with `tc.cp` like the metadata
(`tests/regression/sgemm_tcu_tmem_mx`).

## 3. Transfers (`tmem.ld` / `tmem.st`)

A warp's transfers reach only its own block's lanes: warp `w` issues to
block `w % ISSUE_WIDTH`, and `taddr`'s lane is that block's first. Each
block has its own transfer port; nothing crosses blocks. One uop moves one
register:

- **Lane per thread** (`LANE_PER_THREAD`): thread `t` holds lane `t % xtileM`;
  register `r`, column `r*TPL + t/xtileM`. One read or write of both RAMs at
  one row; a per-thread `tcN/TPL`:1 select. FlashAttention's softmax uses it
  (a thread owns a row, so max and sum need no shuffles).
- **Fragment** (`FRAGMENT`): register `r` is the fragment of M step `r % 2`,
  columns `(r/2)*tcN`, in the WGMMA accumulator register layout. One read or
  write of one RAM, identity data. Epilogues use it.

A step's D write-back, then a `tc.cp` row, have a block's write port before
a store; a transfer read has the read port before the engine (which loses
the cycle).

## 4. The engine

`tc.mma`, `tc.cp`, `tc.commit` and the allocation ops reach a scalar-only
port (thread 0's operands; a block arbiter). `tc.mma` and `tc.cp` enter a
4-deep in-order queue. A `tc.commit` rides on the youngest op that has not
left the engine (up to two per op); with none it queues as an entry of its
own.

The engine runs the queue head:

- **`tc.mma`:** a cycle to set the descriptors, a prologue that reads the
  metadata row and/or A's `fedpK/tcN` fragment groups (all blocks in
  parallel), then the (m, n) walk, one step per cycle on all blocks in
  lockstep over the tile buffers (A stripes per block, B shared, with the
  dense K-major B prefetch). A step waits while its C fragment's write-back
  is in flight (the consecutive-MMA row hazard, the only hazard checked in
  hardware).
- **`tc.cp`:** waits for the steps in flight to be written back, then reads
  one bank row at a time through the tile buffers' LMEM port.

**Operand fetch knobs.** A dense K-major B block serves 2 steps and its
fetch takes 4 cycles from request to first use, so with the one-block-ahead
prefetch above the walk runs 2 steps in every 4 cycles.
`VX_CFG_TCU_TMEM_B_AHEAD` (default 1) is the number of blocks fetched ahead of
the one in use: above 1 the B buffer has that many slots plus one (block `n`
in slot `n mod slots`), requests run ahead of the responses, each side with
its own block and row counters, and the walk is back to back from 2 on.
`VX_CFG_TCU_TMEM_A_BANKROW` (default 0) at 1 lets one shared-memory read
serve every remaining row-major A row in the bank row it reads, instead of a
read per row. Sparse and block-major B keep the one-block fetch.

An op leaving the engine posts its commits with the count of steps issued
so far; a commit arrives on its barrier once that many steps have been
written back, one arrival per cycle through `VX_tcu_bar_if`.

## 5. Ordering contract

As on Blackwell, ordering between transfers and MMAs is the software's:
`tmem.ld` of columns an MMA writes needs that MMA's `tc.commit` and a
barrier wait; an MMA reading columns a warp stored needs `tmem::wait_st`
(`vx_wsync`) and a barrier first. The hardware has no range interlock. SimX
checks the contract: a transfer or dealloc touching columns of an
unsettled op of its CTA aborts with the op it raced.

**Undefined behavior.** As in PTX, misuse of tensor memory is undefined:
the hardware does not check it, and the result is unspecified (it may
overwrite other columns, another CTA's included, or wait forever). RTL
simulation asserts on each case below and SimX checks the ordering
contract; an FPGA reports nothing, as for the rest of the core (a
misaligned access or an illegal instruction is also checked only in
simulation). Undefined:

- a `tc.mma` whose D, A or metadata columns are unaligned or out of range,
  or whose shared-memory descriptors are invalid;
- a `tc.cp` whose columns, size or source are unaligned or out of range;
- a `tmem.alloc` or `tmem.dealloc` size that is not a power-of-two number
  of 32-column chunks within TMEM, a dealloc of columns its CTA does not
  own, and allocations beyond the launch's TMEM budget (§6);
- a transfer of another block's lanes or at a column that is not a
  multiple of `tcN`, and one issued with any thread inactive (a transfer is
  warp-wide, as `tcgen05.ld/st` are `.sync.aligned`);
- a `tc.*` or allocation op issued without thread 0 active;
- breaking the ordering contract above;
- a WGMMA instruction in a TMEM build (§7).

## 6. Allocation and launch

`tmem.alloc` waits off the scalar port until a run of its size is free, so
a waiting allocation never blocks other ops. Runs are tagged with the owner
CTA, and a CTA's runs are freed when its last warp retires; a dealloc of
another CTA's run is undefined (§5).

A kernel declares its CTAs' TMEM in `vx_launch_info_t::tmem_cols` (the KMU
DCR `VX_DCR_KMU_TMEM_COLS`). The budget, rounded up to a power-of-two run,
counts toward occupancy like LMEM, so co-resident CTAs' allocations always
fit and an allocation within budget never waits on a CTA that is not
running. A launch with a budget also places its warps block-aligned: rank
`r` runs on launch lane, and TCU block, `r % ISSUE_WIDTH` (TMEM builds have
one launch lane per block). The runtime writes the DCR only when a budget
is set or was set, so other launches are unchanged.

## 7. TMEM builds and WGMMA

A TMEM build drives the tile buffers from the engine, so warps issue no
WGMMA: the per-warp WGMMA expansion and its front-end logic are left out
(`TCU_WARP_WGMMA`), and a WGMMA instruction is undefined (§5): simulation
rejects it at decode. WMMA is unchanged.

## 8. Results

NT=8, 4 TCU blocks (TMEM 32 lanes by 512 columns, 64 KB), FEDP2K, fp16 →
fp32. Area: Beacon on the `tcuw` DUT (`VX_tcu_unit` + `VX_tcu_shared` at
full issue width, TFR FEDP; `make tcuw-beacon` in `hw/syn/xilinx/dut`).
Cycles: rtlsim, SimX in parentheses, with the configs of
`ci/testcases/tensor_wg.yaml` (16 warps).

**Area against the same build without TMEM:**

| | WGMMA only | TMEM | Difference |
|---|---|---|---|
| LUT | 100,302 | 110,959 | +10,657 (+10.6%) |
| FF | 60,960 | 63,355 | +2,395 |
| Block RAM | 0 | 32 | the TMEM storage |

| Where the LUTs go | LUT |
|---|---|
| `VX_tcu_tmem` | 7,516 |
| A buffers: the A-from-TMEM fill (4 blocks) | 1,960 |
| B buffer: dense K-major prefetch, row merging, wide N step | 628 |
| Tile-buffer LMEM port: the `tc.cp` master | 119 |
| Blocks: engine steps beside warp ops | 988 |
| Warp WGMMA logic left out (§7) | −553 |

**Inside `VX_tcu_tmem`**, each part measured by removing it alone:

| Part | LUT |
|---|---|
| `tmem.st`: per-block data, lane-per-thread scatter, write port | 2,358 |
| `tmem.ld` and the transfer unit's shared logic | 911 |
| `tc.cp` | 1,674 |
| Commit tracking | 411 |
| Allocator | 341 |
| Row-hazard check | 176 |
| Op queue | ~100 |
| Engine walk, step register, write-back queues, port control | ~1,550 |

**Builds without TMEM** match the base: every non-TMEM perf gate keeps its
cycle count and flip-flops are identical. Beacon LUTs are equal for WMMA
builds and within 2 for WGMMA builds (34 on the `tensor` DUT), where the B
buffer's TMEM-gated code maps slightly differently.

**Cycles:**

| Kernel | Cycles |
|---|---|
| GEMM 128×128×128 | 46,724 (48,132) |
| GEMM 128×128×512 | 183,529 (185,767) |
| Sparse GEMM 64×64×64, metadata by `tc.cp` | 8,191 (8,631) |
| FlashAttention-4, 4 tiles (`-l64 -n32 -d64 -b4`) | 59,649 (59,205) |

## 9. Not covered

- WGMMA and TMEM in one build.
- `.ws`, `cta_group::2`, `shift`, multicast commit.
