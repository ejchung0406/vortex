# runtime: tag in-process CP host addresses so they cannot alias device memory

## Summary

The simx, rtlsim, firesim and foldsim runtimes run the command processor
(CP) as a C++ model inside the host process. Its memory hooks treated an
address as host memory when it fell inside a registered host region (command
ring, head and completion slots, DMA staging), and a region's CP address was
the bare host pointer. A device address can equal a host heap pointer. A DMA
chunk meant for that device address is then copied into the host heap, and a
download reads host bytes back. Only the start of an access was checked, so
a 64 KB chunk also overran the region it hit.

This change gives the four backends one shared helper,
`sw/runtime/common/host_regions.h`. A region's CP address is its pointer
with bit 62 set, the hooks route by the tag alone, and a tagged access must
lie wholly inside one region or the process stops with a message.

## Motivation

Device buffers are allocated from `VX_MEM_USER_BASE_ADDR` (0x10000) upward.
In a non-PIE host executable the glibc heap starts just above the
executable, at a few MB. The CP ring (64 KB) and the completion slots are
small allocations, so they live there, among the addresses of an ordinary
device buffer. The Spack GCC 12.3 used here does not build PIE by default,
and the cluster nodes run with ASLR off (`randomize_va_space=0`). Under those
two conditions the layout is fully deterministic.

`tests/runtime/test_host_alias`, added here, is a 64 MB
`vx_copy_to_dev`, a full `vx_copy_from_dev` and a second download from a
32 KB offset, each compared. On plain upstream/master (1 core, default
config, test linked non-PIE), the device buffer is [0x30000, 0x4030000) and
the host heap is at about 0x5c5bb0:

- simx, XLEN 64: segmentation fault in 5 of 5 runs. The backtrace is in the
  `dram_read` lambda of `vx_device::make_cp_hooks`, inside
  `CommandProcessor::tick_engine`, called from `Device::cp_submit_mem_write`
  (the upload).
- simx, XLEN 64, the same binary under gdb (heap 64 bytes higher, at
  0x5c5bf0): no crash, but silently wrong data. Word 1441824 (device address
  0x5B0080) reads back 0x00000002, a completion seqnum the CP wrote into the
  aliased host slot, where the test pattern was expected.
- simx, XLEN 32 (host runtime only): segmentation fault in 3 of 3 runs.
- rtlsim, XLEN 64: segmentation fault (rc 139).
- The same test built as a PIE: heap at 0x555555719bb0, far above device
  memory. It passes on upstream (3 of 3). The bug needs a host binary whose
  heap lies below the top of the device buffers in use. On distributions
  whose compiler builds PIE by default it stays latent, which is probably
  why it has not been seen in CI. A non-PIE application, or a heap in range
  for another reason, hits it.

The research fork first saw it as a crash in `make_cp_hooks` during a
~42 MB upload on one core (fork measurement, `gemm_block -m16 -n5120
-k4096 -b1 -s1`, a fork-only test): the chunk at device address 0x2070000
fell inside the command ring's allocation.

## What changed

- `sw/runtime/common/host_regions.h` (new): `HostRegions` with `alloc` /
  `release` / `lookup` for CP-visible host memory:
  - The CP address is the pointer with `HOST_TAG` (bit 62) set. There is a
    `static_assert(VX_CFG_MEM_ADDR_WIDTH < 62)`.
  - An allocation whose pointer already has bit 62 set fails instead of
    misrouting.
  - `lookup(addr, bytes)` returns nullptr for an untagged address (device
    memory). It returns the host pointer for a tagged access inside one
    region, and aborts with a message for a tagged access outside every
    region.
- `sw/runtime/{simx,rtlsim,firesim,foldsim}/vortex.cpp`: the four identical
  copies of the region map, its mutex, the lookup and the destructor's free
  loop are replaced by the helper. The hooks pass the access size, and
  firesim's now-empty destructor is dropped.
- `tests/runtime/test_host_alias.cpp` (new) and `tests/runtime/Makefile`:
  - The test is added to `TESTS`, so the existing CI case `runtime-1`
    (`ci/testcases/sw/runtime.yaml`, simx, XLEN 32 and 64) runs it. It needs
    no kernel.
  - It is linked `-no-pie` so the heap lies inside the device buffer on any
    distribution.

### Why bit 62 is safe

- **CP addresses are 64-bit at every XLEN.** `host_mem_alloc` and the
  callbacks use `uint64_t`, the CP model holds the ring and completion bases
  in 64-bit fields programmed as LO/HI register pairs, and the command
  arguments are 64-bit. The XLEN 32 host-runtime runs above exercised this (supplementary).
- **No device address reaches bit 62.**
  - Physical device addresses are below 2^`VX_CFG_MEM_ADDR_WIDTH` (48 at
    XLEN 64, 32 at XLEN 32).
  - The runtime's allocator stays below `GLOBAL_MEM_SIZE` (8 GB / 4 GB).
  - With VM on, VAs come from `[ALLOC_BASE_ADDR, GLOBAL_MEM_SIZE)`
    (Sv39/Sv32, lower half only, never sign-extended). The page-table walk
    in the CP model yields PAs of that width.
- **Host pointers do not have bit 62.** User-space pointers are below 2^47
  (x86-64; 2^56 with 5-level paging) or 2^52 (AArch64). Top-byte pointer
  tagging (AArch64 TBI/MTE, HWASan) could set it; the allocation then fails
  with an error rather than misrouting.
- **The tag is never visible to the device.** In these four backends the CP
  is the in-process model, and `lookup` strips the tag before the memcpy.
  The device side (the simx `Processor`, the Verilated RTL, the firesim and
  foldsim transports) only receives untagged device addresses.
  - `vx_mem_alloc(VX_MEM_HOST)` returns the CP address as the buffer
    address, so a caller would now see the tagged value. Nothing in the tree
    uses that flag outside the runtime core, and on these backends a kernel
    could not dereference host memory either way.

### Alternative design

Mainstream stacks give host memory a device-visible bus or IOVA address,
minted by the driver from its own window and mapped back to the host pointer
(e.g. `cudaHostGetDevicePointer`, an IOMMU mapping). The aved backend's
simulation mode does exactly that: `reserveFakePhysAddr` in a DDR window plus
a `sim_regions_` map. The tag here is the minimal form of such a window, with
the base at bit 62 and the offset equal to the pointer, so no window
allocator is needed. If the maintainer prefers a separate window with an
allocator, `HostRegions` is the one place to change.

### Other runtimes

gem5 (a VRAM aperture), opae (FPGA IO addresses), xrt and aved (BO or DMA
addresses; their non-`CPP_API` fallback uses the raw pointer but has no
software CP hook) do not route by region in software. They are not affected
and not changed.

## How verified

Status: complete for the scope below; verified at XLEN 64 only. The XLEN 32
rows are extra host-runtime runs; XLEN 32 is out of scope for this effort.

All runs use 1 core and the default config with `VORTEX_VM_MEGAPAGE_HEAP=1`,
on nodes with ASLR off. "Base" is plain upstream/master (ccf811e11) plus
only the new test. Result directories are under `vortex-exp/results/`.

| what | driver | config | base | branch | results |
|---|---|---|---|---|---|
| test_host_alias, 5 runs | simx | XLEN 64 | segfault 5/5 | PASSED 5/5 | `pr_runtime-cp-host-tag_{base,pr}_simx/host_alias_*.log`, backtrace in `base_simx/host_alias_diag.log` |
| test_host_alias under gdb | simx | XLEN 64 | wrong data (seqnum in buffer) | not run | `base_simx/host_alias_diag.log` |
| test_host_alias built PIE | simx | XLEN 64 | PASSED 3/3 (heap 0x5555...) | not run | `base_simx/host_alias_pie.log` |
| test_host_alias, 3 runs | simx | XLEN 32, host runtime only | segfault 3/3 | PASSED 3/3 | `pr_runtime-cp-host-tag_simx32_rt/summary.txt` |
| test_host_alias | rtlsim | XLEN 64 | segfault | PASSED | `pr_runtime-cp-host-tag_rtlsim/summary.txt` |
| tests/runtime run-simx (6 tests) | simx | XLEN 64 | 5 pass, test_host_alias fails | 6/6 PASSED | `*_simx/runtime_suite.log` |
| demo | simx | XLEN 64 | 6410 cycles, 2468 instrs | 6410, 2468 | `*_simx/bb_demo.log` |
| sgemm -n16 | simx | XLEN 64 | 18364, 7520 | 18364, 7520 | `*_simx/bb_sgemm16.log` |
| vecadd | simx | XLEN 64 | 1631, 512 | 1631, 512 | `*_simx/bb_vecadd.log` |
| multikernel | simx | XLEN 64 | 39335, 13104 | 39335, 13104 | `*_simx/bb_multikernel.log` |
| module_reload | simx | XLEN 64 | PASSED | PASSED | `*_simx/bb_module_reload.log` |
| dxa_copy -d1 (`-DVX_CFG_EXT_DXA_ENABLE`) | simx | XLEN 64 | 6466, 1624 | 6466, 1624 | `pr_runtime-cp-host-tag_base_simx_dxa/`, `pr_simx/bb_dxa_copy.log` |
| demo -n64 | rtlsim | XLEN 64 | 6423, 2468 | 6423, 2468 | `pr_runtime-cp-host-tag_rtlsim/summary.txt` |
| sgemm -n16 | rtlsim | XLEN 64 | 18398, 7520 | 18398, 7520 | same |
| vecadd -n64 | rtlsim | XLEN 64 | 1641, 512 | 1641, 512 | same |
| firesim runtime compiles (`-fsyntax-only`) | — | XLEN 64 | not run | rc 0 | `pr_simx/firesim_syntax.log` |
| foldsim runtime | — | — | not run | not run (no Foldsim installation here) | — |

The rtlsim comparison builds the model once and swaps only
`libvortex-rtlsim.so` (base: md5 4a365012, branch: md5 c0e31569, in place,
because the test binaries carry DT_RPATH to the runtime directory). The
loaded library is checked per case.

`ci/check_sw_sim_boundary.sh`: OK.

Not run here:
- XLEN 32 kernels, blackbox tests and cycle counts (out of scope; also no
  32-bit RISC-V toolchain here). CI's `runtime-1` runs the new test at
  XLEN 32. Command: `./ci/blackbox.sh --driver=simx --app=demo` in a
  `--xlen=32` build.
- foldsim and firesim at run time. foldsim has no Foldsim installation here;
  firesim was only compile-checked. Commands:
  `make -C sw/runtime/foldsim` (needs `FOLDSIM_SHELL`), and
  `VORTEX_DRIVER=firesim make -C tests/runtime run`.
- xrt `run-xrt` of the new test. The CP there is RTL on a separate host
  port, and the test's 64 MB transfer may be slow in xrtsim.
- ASLR on. It could not be enabled on these nodes. With ASLR, a non-PIE heap
  is placed up to 32 MB above the executable, still inside the 64 MB buffer.

## Change class

`python3 ci/change_impact.py explain --base upstream/master --head HEAD`:
**R2**, reasons `path:sw-stack`, `path:build-config`. No R3 detector hit.
Evidence note: `configure-copied` for `tests/runtime/Makefile`; the tests
ran from freshly configured build trees.

The policy still requires:
- a review of every changed hunk against the rules by someone other than the
  author (not done; no reviewer was spawned, so no review findings exist,
  open or closed);
- maintainer approval.

No perf baseline moves: the change is runtime-only and cycles are identical
above. No Beacon is needed (no RTL).

## Dependencies

None. The branch is off upstream/master ccf811e11.

## Open questions for the maintainer

- Is a tagged window at bit 62 acceptable, or would you rather have an
  allocated IOVA-style window, as aved's simulation mode uses?
- `lookup` aborts on a tagged access outside every region. Would a soft
  failure through the CP be preferred? The hooks return `void` today.
- The `VX_MEM_HOST` comment in `vortex2.h` says simx, rtlsim and gem5
  ignore the flag. On upstream they do not: simx and rtlsim return a host
  region, and with this change its address is tagged. Should the comment or
  the behaviour change?

---

## Notes for the sender

- The commit message holds the same evidence in short form.
- Possible textual overlap: branches `dram-hbm3e`, `dram-interleave` and
  `dram-sched-window` edit `sw/runtime/foldsim/vortex.cpp` near line 474;
  this branch edits lines 346, 420, 707 and 808. A clean merge is expected.
- Fork measurements quoted: only the original gemm_block crash (0x2070000,
  ~42 MB upload).
