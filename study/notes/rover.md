# rover as a third place to run Vortex simulations (set up 2026-11-22)

rover (`ssh rover` from Phoenix) is a
shared lab server, not a cluster: 2 x Xeon E5-2696 v4 (88 threads, 2.2 GHz),
251 GB RAM, Ubuntu 24.04, gcc 13.3, no Slurm and no time limit. Its use is the
long rtlsim reference runs that do not fit Phoenix's or ICE's limits.
Everything of ours is under one directory, `~/vortex-rover/` (NFS home), plus
build trees and a compiler cache under `/tmp/echung67/vortex-rover/` (local
NVMe). Nothing else in the home directory was touched.

## House rules

- Reach it only with `ssh rover`. Do not edit `~/.ssh/config`.
- It is shared, and the user has their own processes there. Never kill, renice
  or otherwise touch a process we did not start. Ours are the sessions listed
  in `~/vortex-rover/run/*.pid`.
- Everything runs under `nice -n 10` (rover_start.sh does this).
- At most 40 cores in total: build threads plus one per running point. Fewer
  if `uptime` shows load from others.
- One simulator build at a time, and only when memory is free. rover_point.sh
  enforces both: one lock for all builds, and a build waits until 130 GB
  (rtlsim) or 16 GB (SimX) is available.
- Keep everything under `~/vortex-rover/`.
- Never push git from there, never copy `JESD238B.01.pdf` (rover_push_src.sh
  sends no PDF), no compiled binaries into the repo's `study/` folder.
- `/tmp` on rover deletes files unused for 30 days (`tmpfiles.d`), and a reboot
  may clear it. A lost build tree only costs a rebuild; results and logs are on
  the NFS home.

## Layout on rover

```
~/vortex-rover/
  tools/                 copy of Phoenix vortex-tools (riscv gcc 12.2, llvm-vortex 20.1.8,
                         verilator 5.050, libc64, libcrt64); env.sh is rover's own
  src/<name>/            source copies from rover_push_src.sh (+ .rover_src_info)
                           vd1e5 = d1e537603, v602 = 602af5c84
  rover_point.sh         pushed from Phoenix vortex-exp/rover/ before every start
  rover_status_remote.sh
  results/<NAME>/        meta.txt run.log build.log configure.log model_key.txt mem.log DONE
  logs/<NAME>.out        stdout/stderr of the point's script (empty when all is well)
  run/<NAME>.pid         session id (= process group) of the point
  run/<NAME>.phase       what it is doing and since when
/tmp/echung67/vortex-rover/
  build/<MODEL>/         kept build trees (shared simulators); build/_<NAME>_<pid>/ private ones
  ccache/                compiler cache (limit 60 GB)
  build.lock
```

## Scripts (Phoenix, `vortex-exp/rover/`)

- `rover_push_src.sh --rev <git-rev> <name>` puts the exact tree of a commit on
  rover as `src/<name>`: `git archive` plus the five third_party submodules
  from a prepared tree (`vortex-exp/src_q_unroll`; it checks that the commit
  pins the submodule commits the repo has checked out). `rover_push_src.sh
  <dir> [<name>]` copies an existing tree instead. Either way it sends only
  what a build needs, writes `.rover_src_info`, and builds third_party on
  rover once (`ramulator_ext` is compiled there with gcc 13; the other
  third_party binaries are the Phoenix ones and load as they are).
- `rover_start.sh` takes the variables of `run_point.sbatch` (NAME APP ARGS
  PROFILE CORES XCONF PERF DRIVER) and `SRC` (a name under `src/`), plus:
  - `THREADS` build parallelism (default 32);
  - `MODEL` a build tree to keep and share (see below);
  - `AFTER` names of points whose builds must be over before this one's starts;
  - `MINFREE_GB` memory that must be free before the build.

  It starts the point detached (`setsid nohup nice -n 10`), so it survives the
  ssh session, and prints the session id. It refuses a name that is running or
  already has a result with rc=0 (`FORCE=1` reruns).

      NAME=x APP=bwstream ARGS="-n2048 -u4 -b32" PROFILE=h200s CORES=1 \
        XCONF="-DVX_DBG_STALL_TIMEOUT=20000000" DRIVER=rtlsim SRC=v602 \
        MODEL=m602_h200s_c1 vortex-exp/rover/rover_start.sh

- `rover_status.sh` prints one line per point: running / done / FAILED / DEAD
  (no process and no DONE marker), the phase, build time and peak memory, run
  time and the final cycle count.
- `rover_fetch.sh NAME...` copies `results/<NAME>/` into
  `vortex-exp/results/<NAME>/` and appends `ran_on=rover (fetched ...)` to
  meta.txt. It skips a point without an `rc=` line (`FORCE=1` fetches anyway)
  and never overwrites a Phoenix result of the same name.
- `rover_point.sh` is run_point.sbatch for rover. Differences: the build is
  timed apart from the run (`build_s`, `build_peak_gb`; `wall_s` is the
  blackbox call, which then finds the simulator built), meta.txt has `cpu=`,
  `cluster=rover`, `model=`, `threads=`, and the session's memory is sampled
  every 20 s into `mem.log`.
- `rover_env.sh` is installed as `tools/env.sh`.

To stop a point (ours only): `ssh rover 'kill -TERM -- -$(cat ~/vortex-rover/run/<NAME>.pid)'`.

## Sharing one built rtlsim model

The Verilated model is `sw/runtime/librtlsim.so` of a build tree. Its Makefile
rebuilds it when Verilator's flags or the C++ flags change, and both contain
the CONFIGS string the app's Makefile resolves (profile folded in, then the
flags the app adds for itself). So two points can use one built model exactly
when that string is the same, whatever the app and its arguments.

`MODEL=<name>` names a kept build tree. The first point builds it and records
its key (source, driver, profile, THREADS, resolved CONFIGS) in
`<tree>/.model_key`; a later point with the same key finds the model built
(seconds), and a point with another key is refused, so a tree is never rebuilt
under a running simulation. Each point also writes its key to
`results/<NAME>/model_key.txt`.

For the study profile this means: `h200s` already turns on TCU, DXA, WGMMA and
TMEM, so `bwstream`, `warp_rate`, `tmem_roof` and `war_order` (and `bar_vis`,
`fetch_squash`) with the same XCONF resolve to one string and share one model.
`gemm_block` with `-DMTILES=1 -DPROFILE_ENABLE` and `gemm_tmem_decode` with
`-DKSTEPS=2 -DPIPE=6 -DXFER=8192` have other strings, so each has its own
tree. Those macros are only read by the apps (none appears under hw, sim,
sw/runtime, sw/kernel or sw/common at 602af5c84), so the hardware is the same
in all three; the build system still sees three configurations. The compiler
cache did not shorten the second model build (41 min against 37; the cache
holds 239 MB after two models, so the model's objects are not in it).

## What matches Phoenix and what differs

- **Tools:** the copied vortex-tools run unchanged. Only `riscv64-unknown-elf-gdb`
  (libpython2.7, libncurses5), `clang-repl` (libedit) and `llvm-spirv` have
  missing libraries, and no build uses them. Verilator is our 5.050, not
  rover's `/usr/bin/verilator` 5.020.
- **Host compiler:** gcc 13.3 (Phoenix and ICE: 12.3). Boost 1.83 from `/usr`.
- **ccache:** rover has it and the simulator Makefiles use it. `tools/env.sh`
  points it at `/tmp/echung67/vortex-rover/ccache` so the user's `~/.ccache`
  is not touched. Phoenix and ICE build without. It makes a repeated SimX
  build a matter of seconds; it does not help the rtlsim model.
- **CPU:** E5-2696 v4, 2.2 GHz (2016), slower per core than the Gold 6226.

## Validation (2026-11-22)

SimX, profile h200s, 1 core, each number against the Phoenix value given for
the same source:

| Source | Case | XCONF | Phoenix | rover | Result name |
|---|---|---|---|---|---|
| d1e537603 | `bar_vis` | `-DVX_DBG_STALL_TIMEOUT=20000000` | 154,182 | 154,182 | rv_d1_barv |
| d1e537603 | `bar_vis` | none | 154,182 | 154,182 | rv_d1_barv_nost |
| d1e537603 | `gemm_block -m16 -n1024 -k4096 -s1` | `-DMTILES=1 -DPROFILE_ENABLE` | 665,514 | 665,514 | rv_d1_gb |
| 602af5c84 | `bar_vis` | `-DVX_DBG_STALL_TIMEOUT=20000000` | 146,121 | 146,121 | rv_602_barv |
| 602af5c84 | `fetch_squash` | `-DVX_DBG_STALL_TIMEOUT=20000000` | 10,016 | 10,016 | rv_602_fsq |

rtlsim, default configuration (no profile), 602af5c84, `fetch_squash -w2 -i8`,
run on both machines for this check:

| | cycles | instrs | `PERF:` lines | build + run |
|---|---|---|---|---|
| Phoenix, run_point.sbatch job 16012541 (`rv_phx_fsq_rtl`, source `vortex-exp/src_rv602`) | 6,453 | 964 | | 55 s |
| rover (`rv_rov_fsq_rtl`) | 6,453 | 964 | all 4 identical to Phoenix | 49 s + 16 s |

All validation results are fetched into `vortex-exp/results/rv_*`.

## Measured cost on rover (h200s, 1 core, rtlsim, 602af5c84)

| | |
|---|---|
| Model build, 32 threads | 37 min (2,218 s) and 41 min (2,468 s, with four simulations running beside it). About 25 min of it is Verilator itself on one thread; the C++ compile is short |
| Peak memory of a build | 77-78 GB (Verilator alone holds about 35 GB) |
| Build tree | 4.0 GB each on `/tmp` |
| A point that finds its model built | 1-2 s |
| Simulation speed | 67-151 simulated cycles per second (107 `tmem_roof`, 116 `warp_rate`, 106 `war_order`, 77 `gemm_block`, 67 `gemm_tmem_decode`; `bwstream` 151, 123 and 85 at n=128, 512 and 2048; measured with a model build or other simulations running beside). The simulation is one thread; `bwstream` and `gemm_block` keep a second thread busy in the host program, so count two cores for them |
| Memory of a running simulation | 0.25 GB |
| SimX point | build 3 min the first time, seconds after (ccache); run 20-40 s |

For comparison, the same kind of point took 3.2-3.6 h on Phoenix with 8 cpus
(`h2rt_*_rtl`, build included), almost all of it the build.

## Long references started 2026-11-22 (rtlsim, h200s, 1 core, 602af5c84)

XCONF is `-DVX_DBG_STALL_TIMEOUT=20000000` plus what the table says; `--perf=7`.
SimX values are Phoenix's for the same source, except `war_order`, taken on
rover (`rvr_war_simx`).

| Name | App and args | XCONF adds | MODEL | SimX cycles | rtlsim cycles | diff | run time |
|---|---|---|---|---|---|---|---|
| rvr_roof_rtl | `tmem_roof -n512 -i8` | | m602_h200s_c1 | 62,279 | 61,749 | -0.85% | 577 s |
| rvr_wrate_rtl | `warp_rate` | | m602_h200s_c1 | 113,382 | 113,406 | +0.02% | 981 s |
| rvr_war_rtl | `war_order` | | m602_h200s_c1 | 344,196 | 329,817 | -4.18% | 3,114 s |
| rvr_bw_rtl | `bwstream -n2048 -u4 -b32` | | m602_h200s_c1 | 419,474 | 548,386 | +30.7% | 6,418 s |
| rvr_gb_rtl | `gemm_block -m16 -n1024 -k4096 -s1` | `-DMTILES=1 -DPROFILE_ENABLE` | m602_h200s_c1_gb | 574,100 | 608,849 | +6.05% | 7,950 s |
| rvr_gtd_rtl | `gemm_tmem_decode -m16 -n8192 -k4096` | `-DKSTEPS=2 -DPIPE=6 -DXFER=8192` | m602_h200s_c1_gtd | 1,319,938 | 1,554,320 | +17.76% | 23,160 s |

`war_order` retires the same 341,824 instructions on both models.

**All six finished and passed on 2026-11-22** (the last, `gemm_tmem_decode`,
at 23:09 after 6.4 h of simulation). A running rtlsim prints nothing until it
ends and rover does not allow attaching to it (`ptrace_scope=1`), so progress
cannot be read while it runs; plan with the speeds above and expect the RTL
to need up to about 30% more cycles than SimX on the memory-heavy cases.

**`bwstream`: rtlsim and SimX move apart as the stream grows.** Two smaller
sizes were run by hand on the same model (`results/rvc_bw_calib_rtl`; SimX in
`rvc_bw_n*_simx`, on rover):

| `bwstream -u4 -b32` | SimX | rtlsim | diff | rtlsim time | cycles/s |
|---|---|---|---|---|---|
| `-n128` | 38,539 | 38,956 | +1.1% | 258 s | 151 |
| `-n512` | 126,128 | 140,758 | +11.6% | 1,146 s | 123 |
| `-n2048` | 419,474 (Phoenix) | 548,386 | +30.7% | 6,418 s | 85 |

Instructions match at n=128 and n=512 (34,816 and 120,832); rtlsim retires
464,896 at n=2048. From n=128 to n=512 each unit of n costs 228 cycles on
SimX and 265 on rtlsim, and the rtlsim count at n=2048 is what that slope
gives (548 thousand), while SimX's is below its own slope (476 thousand
expected, 419,474 reported). The cause is not looked into here.

The driver prints a one-time "CP has not retired a command in 10s" warning in
these logs. It is the wall-clock watchdog of a slow simulation, not an error;
the passing runs have it.

## Later points (2026-11-23)

**`bwstream -u4 -b32` at four sizes**, rtlsim on model m602_h200s_c1 against
SimX from two sources, all on rover (`rvr_bw*_rtl`, `rvc_bw_calib_rtl`,
`rvc_bw_n*_simx`, `rvc_bw_n*_simx_f55f`; source copy `vf55f` = f55faaa2d):

| n | rtlsim 602af5c84 | SimX 602af5c84 | diff | SimX f55faaa2d | diff |
|---|---|---|---|---|---|
| 128 | 38,956 | 38,539 | -1.1% | 40,189 | +3.2% |
| 512 | 140,758 | 126,128 | -10.4% | 151,757 | +7.8% |
| 1024 | 211,928 | 208,994 | -1.4% | 206,544 | -2.5% |
| 2048 | 548,386 | 419,474 (Phoenix) | -23.5% | 449,189 | -18.1% |

(diff = SimX against rtlsim.) Instructions are the same on both models at
every size (34,816 / 120,832 / 235,520 / 464,896). The gap is not monotonic
in n: small at 128 and 1024, large at 512 and 2048.

**`smprobe`** (`--perf=4`, XCONF adds `-DPATH_STATS`, model m602_h200s_c1_ps;
SimX values are Phoenix's, before and after the lane-parity fixes):

| Args | rtlsim cycles | instrs | SimX before | SimX after |
|---|---|---|---|---|
| `-k0 -n2 -i20 -d5000` | 50,477 | 33,445 | 77,636 | 89,793 |
| `-k0 -n2 -i40 -d5000` | 90,995 | 59,533 | 140,178 | 160,587 |
| `-k1 -n2 -i80 -d5000` | 147,024 | 99,197 | 236,063 | 265,536 |
| `-k0 -n2 -i40 -d2000` | 85,145 | 55,117 | 135,508 | 154,184 |

rtlsim is 35-45% below SimX at all four. SimX's instruction counts were not
at hand, so whether both models do the same work here is not checked.
`PATH_STATS` appears nowhere under hw, sim, sw or tests at 602af5c84.

Script additions since the first version: `PARALLEL_APP=1` lets points of one
app run at the same time in one MODEL tree (used for the four smprobe
points); a killed point now ends as FAILED instead of `done rc=0`. Other
agents also start points with these scripts, so rover_status.sh lists theirs
too.

Check and collect:

    vortex-exp/rover/rover_status.sh
    vortex-exp/rover/rover_fetch.sh rvr_roof_rtl rvr_wrate_rtl rvr_war_rtl rvr_war_simx rvr_bw_rtl rvr_gb_rtl rvr_gtd_rtl

Housekeeping afterwards: the three model trees take 12 GB under
`/tmp/echung67/vortex-rover/build/`; delete them when no more points of those
configurations are wanted (`ssh rover 'rm -rf /tmp/echung67/vortex-rover/build/<MODEL>'`,
only when rover_status.sh shows nothing running in it).

## 2026-11-24: outage and new limits
rover refused ssh from 2026-11-23 20:56 (closed right after "Server accepts key") until someone rebooted it at
2026-11-24 09:30. Cause not confirmed (no access to the previous boot's kernel log); several h200s rtlsim runs, an
rtlsim build and a full-GPU Beacon run were active. /tmp was cleared: every build tree and model is gone; ~/vortex-rover
(src, results, scripts) survived. From now on: one rtlsim build at a time for all users of the scripts (the script no
longer takes BUILD_SLOTS for rtlsim), 150 GB must be free before a build starts, and no lint or Beacon of a full GPU
on rover.
Memory cap (2026-11-24): `~/.config/systemd/user/vortex.slice` on rover (MemoryHigh 185G, MemoryMax 200G, no swap,
TasksMax 4000). `rover_point.sh` re-executes itself inside it (`systemd-run --user --scope --slice=vortex.slice`), so
every point started by rover_start.sh is under the cap: when our work outgrows it the kernel kills inside the slice
and the server stays up. Anything run by hand must be wrapped: `~/vortex-rover/vxrun <command>`. Processes started
directly over ssh are NOT in the slice (they live in the login session's scope, which a user cannot limit); a cap
on the whole account or systemd-oomd / earlyoom needs root.
