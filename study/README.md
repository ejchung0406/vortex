# Study material: calibrating SimX against a real H200

Working notes, scripts and raw results behind the `h200s` profile and the
fixes on this branch. Not part of Vortex proper; kept here so it can be read
on GitHub.

| Path | What |
|---|---|
| `notes/upstream-pr-candidates.md` | Every finding (bugs, calibration, tests) with evidence and status |
| `notes/upstream-issue-drafts.md` | Issue texts for the three SimX/runtime bugs |
| `notes/open-threads.md` | In-flight state, newest section last |
| `notes/corun-workloads.md` | The co-run scenario (Qwen3.5-9B decode beside Z-Image-Turbo) and its real-H200 measurements |
| `notes/vortex-design-inefficiencies.md` | Core and DXA findings |
| `notes/*.patch` | Patches as produced by the investigations (most are already commits on this branch) |
| `scripts/run_point.sbatch` | One simulator data point: configure, build and run in a node-local tree |
| `scripts/*.py`, `scripts/wait_results.sh` | Result collection |
| `slurm/*_names.txt` | Run names of each experiment batch |
| `results/<name>/` | Raw output of each run: `meta.txt` (source, config, arguments), `run.log`, `build.log`, DRAM stats |
| `gpu/` | Real-GPU measurement sources and scripts (CUDA, cuBLAS, CUTLASS, PyTorch) |
| `results_gpu/` | Real-GPU raw output |

Paths inside the scripts are those of the cluster the study ran on.
