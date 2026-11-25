# Study material: calibrating SimX against a real H200

Working notes, scripts and raw results behind the `h200s` profile and the
fixes on this branch. Not part of Vortex proper; kept here so it can be read
on GitHub.

| Path | What |
|---|---|
| `notes/upstream-pr-candidates.md` | Every finding (bugs, calibration, tests) with evidence and status |
| `notes/upstream-pr-candidates.ko.md` | The same in Korean, brought up to the current state |
| `notes/h200-model-status.md`, `notes/h200-model-status.ko.md` | One page for someone about to run experiments: profiles, how to run a point, model against the part and SimX against rtlsim by kernel class, what not to trust, open items (English and Korean) |
| `notes/upstream-issue-drafts.md` | Issue texts for the three SimX/runtime bugs |
| `notes/open-threads.md` | In-flight state, newest section last |
| `notes/corun-workloads.md` | The co-run scenario (Qwen3.5-9B decode beside Z-Image-Turbo) and its real-H200 measurements |
| `notes/vortex-design-inefficiencies.md` | Core and DXA findings |
| `notes/tpc-placement-and-borrowing.md` | Real H200: does borrowing other SMs' TMA help, and what placement over TPCs and GPCs does to a memory-bound job |
| `notes/h200-l2-structure.md` | Real H200: L2 partitions and hash, latencies, the per-GPC and cross-partition limits, the bystander effect, the per-SM ingress |
| `notes/h200-go-nogo.md` | Real H200: go/no-go experiments A, B, C: a lane-sized kernel never co-resides with a tensor-core GEMM (A), the bypass test is undecided (B), cuBLAS 13.4 computes NaN under MPS percentages and the DiT's real DRAM traffic (C) |
| `notes/h200-bystander.md` | Real H200: why a saturated GPC slows SMs elsewhere (a backlog of unique-line requests at the L2 slices serving it), a model, and the in-flight cap that avoids it |
| `notes/gpu-noc-sources.md` | What the primary sources (NVIDIA documents, patents, papers, Accel-Sim) say about the SM-to-memory network, with quotes |
| `notes/related-work-placement.md` | Related work on GPC/partition placement, per-SM bandwidth and SM sharing, against our measurements |
| `notes/noc-and-design-space.md` | How Vortex connects SMs to memory today, what real GPUs do, and the design space for a memory-bound job on few SMs (corrected in place) |
| `notes/noc-and-design-space-figures.md` | Figure specifications for the note above |
| `notes/noc-model-design.md` | Design of a network and L2/memory model for Vortex, and the `h200g2` / `h200g4` / `h200g4x8` shapes (in progress) |
| `notes/noc-design-verification.md` | Independent verification of that design: verdict and blocking items (in progress) |
| `notes/gpc-limit-design.md` | Is there a hardware proposal in the GPC limit? (no, not on its own) |
| `notes/smem-bypass-design.md` | Is feeding the tensor core without shared memory worth it for decode? (not a proposal on its own) |
| `notes/sm-sharing-design-space.md` | Can an SM be shared between a memory-bound and a compute-bound job without starving the memory job? |
| `notes/sm-sharing-figures.md` | Figure specifications for the note above |
| `notes/framing-critique.md` | Reviewer-style critique of the research framing (verdict: revise) |
| `notes/ice-cluster.md` | How the ICE cluster is set up as a second, free cluster for SimX runs (in progress) |
| `notes/simx-perf.md` | SimX wall-clock optimisation with bit-identical cycle counts (in progress) |
| `notes/sm-rate.md` | Why one simulated SM moves memory at half the H200 rate: shared-memory stores, the DXA engine's port and issue loop, plain loads like for like (running log, interim) |
| `notes/h200-wgmma-width.md` | Real H200: tensor-core throughput against wgmma tile width and number of issuers; the roof is 4,096 FLOPs per actual SM clock |
| `notes/*.patch` | Patches as produced by the investigations (most are already commits on this branch; `dxa_mc_parity.patch`, `warp_rate.patch`, `noc_model.patch`, `ld_calib.patch`, `wrr_fetch_ahead_rtl_wip.patch` are not; the per-warp rate work is on branch `feature/warp-rate`) |
| `notes/noc_collect.py` | Result collection for the network-model runs |
| `pr/notes/` | Notes of the upstream-PR effort (wound down 2026-11-21); `INDEX.md` is its final state. Some notes are drafts (in progress) |
| `pr/scripts/` | Run scripts of that effort |
| `ice/` | Scripts for running SimX points on the ICE cluster |
| `scripts/run_point.sbatch` | One simulator data point: configure, build and run in a node-local tree |
| `scripts/*.py`, `scripts/wait_results.sh` | Result collection |
| `slurm/*_names.txt` | Run names of each experiment batch |
| `results/wl_pf2_table.txt` | `h200s` before/after table for the lane-dispatch and front-end knobs (277 points, SimX, one core) |
| `results/<name>/` | Raw output of each run: `meta.txt` (source, config, arguments), `run.log`, `build.log`, DRAM stats. Files over 5 MB (two instruction traces) and the compiled build stages under `perf_*/stage_*` are left out |
| `gpu/` | Real-GPU measurement sources and scripts (CUDA, cuBLAS, CUTLASS, PyTorch); compiled binaries, address lists and latency samples are left out |
| `results_gpu/` | Real-GPU raw output (`h200_gng_*` go/no-go, `h200_bys_*` bystander, `h200_warprate_nottaken.txt` branch cost); per-slot `*.util` samples are left out |

Paths inside the scripts are those of the cluster the study ran on.
