# Issue text (item 8): DXA workers per socket scale with the total core count

Report only; no branch. Checked on `upstream/master` at ccf811e11 by reading
the files named below (static; nothing was run for this report).

---

**Title:** config: `VX_CFG_NUM_DXA_CORES` sizes each socket's DXA engine from the total core count

**Summary.** `VX_CFG_NUM_DXA_CORES` is still derived as a cluster-level pool
size, but since the DXA core moved into the socket it is the number of workers
inside *every* socket's `VX_dxa_core`. A socket's asynchronous-copy capacity
therefore depends on how many cores the whole configuration has.

**Where.**

- `VX_config.toml`, `[dxa]`:
  ```
  # Cluster-level DXA engine count, decoupled from cores/socket size.
  VX_CFG_NUM_DXA_CORES = "expr: max(1, up($VX_CFG_NUM_CORES / 8))"
  VX_CFG_DXA_MEM_PORTS = "expr: min($VX_CFG_NUM_DXA_CORES, up($VX_CFG_NUM_CORES / $VX_CFG_SOCKET_SIZE) * $VX_CFG_L1_MEM_PORTS)"
  ```
- `hw/rtl/VX_socket.sv:800` instantiates one `VX_dxa_core` per socket
  (`NUM_REQS = VX_CFG_SOCKET_SIZE`, `GMEM_OUT_PORTS = 1`), and
  `hw/rtl/dxa/VX_dxa_core.sv` builds `VX_CFG_NUM_DXA_CORES` workers in it
  (`g_workers`). SimX does the same (`sim/simx/dxa/dxa_core.cpp`,
  `workers_(VX_CFG_NUM_DXA_CORES)`).
- The move was 7d6950de5 (2026-07-11, "gfx: relocate TEX/RTU/DXA into the
  socket"); the formula was not changed with it.

**Effect.** `/` is integer division and `up(x)` is "x, or 1 if x is 0"
(`ci/gen_config.py`, `UpHelper`), so workers per socket are 1 for 1-15 cores,
2 for 16-23, 3 for 24-31, 16 for 132. With `SOCKET_SIZE = 1` (one SM per
socket) a 132-SM configuration has 132 x 16 workers while an 8-SM one has
8 x 1, so the same kernel on one SM
sees a different copy engine depending on how many other SMs are simulated.
Any experiment that scales the SM count changes per-SM DXA capacity with it.

**Also stale.**

- The comment above the formula ("Cluster-level DXA engine count").
- `docs/designs/dxa_async_copy_multicast.md`: "DXA is **cluster-shared**: one
  `VX_dxa_core` per cluster (`VX_cluster.sv:241`)", "Cluster engine top", and
  the follow-up list item that still describes DXA as cluster-scoped in RTL
  and SimX. It also names the count `NUM_DXA_UNITS`.
- `VX_CFG_DXA_MEM_PORTS` has no reader under `hw/`, `sim/`, `sw/`, `ci/` or
  `profiles/` (the socket passes `GMEM_OUT_PORTS = 1`): a knob that is derived
  and ignored.

**Proposed.** Make the worker count a per-socket value that does not depend on
`VX_CFG_NUM_CORES` (a constant, or a function of `VX_CFG_SOCKET_SIZE`), fix the
comment and the design document, and remove or wire `VX_CFG_DXA_MEM_PORTS`.
The right per-socket number is a design choice for the maintainer; for scale,
on our fork (measurements there, not on `master`) one SM reached about 65 GB/s
of copy with 3 workers, 84 with 4 and 101 with 8 through the single memory
port, and `VX_CFG_DXA_MAX_INFLIGHT = 8` (the default beside it) limited a
worker far more than the worker count did (8 lines in flight: 6.7 GB/s; 256:
90 GB/s).

**Not a silent change.** Moving the default is an R3 `config-knobs` change:
multi-core DXA parity and perf cases with 16 or more cores would move.

---

## Notes for the sender

- This is the report the candidates list calls item 8. The related fixes are
  branches `pr/dxa-mem-tag-width` (a larger `DXA_MAX_INFLIGHT` x worker count
  did not elaborate) and `pr/dxa-dispatch-idle` (which worker a request goes
  to); mention them once they are sent.
- The throughput figures are fork measurements on the private `h200s` profile
  with the fork's `bwstream_dxa`; drop them if you would rather keep the issue
  to what `master` shows.
