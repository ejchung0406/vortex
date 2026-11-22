# PACE ICE as a second cluster (set up 2026-11-21)

ICE (login-ice.pace.gatech.edu, `ssh ice` from Phoenix, user echung67) is
Georgia Tech's instructional cluster. Using it costs nothing. Everything of
ours lives under one directory there, `~/scratch/vortex-ice/`
(= `/storage/ice1/4/7/echung67/vortex-ice`). Nothing else in the ICE home was
touched.

## Limits and policies (and where each is stated)

| What | Value | Source |
|---|---|---|
| Slurm association | account `coc`, QOS `coc-ice` (default) and `coc-grade`. There is no charge account to pass. | `sacctmgr show assoc user=echung67` |
| `coc-ice` QOS | priority 7, **MaxSubmitPU 50** (running + pending) | `sacctmgr show qos` |
| `coc-grade` QOS | priority 12, MaxSubmitPU 10, MaxWall 1 day. **Do not use it:** its 10 slots carry the user's own GPU jobs (pcie-introspection `qos_switch.sh`). | `sacctmgr show qos`; pcie-introspection eng13_1117/NOTES.md |
| ice-cpu | default partition, MaxTime 18 h, DefaultTime 1 h, DefMemPerCPU 4 GB, 52 nodes / 1576 cpus: 39 x Gold 6226 24c/191 GB (`cpu-small`), 7 x Gold 6226 24c/772 GB, 1 x 6226 SAS, 4 x EPYC 7513 64c, 1 x Granite Rapids 192c | `scontrol show partition ice-cpu`, `sinfo -o "%c %m %f"` |
| ice-gpu | MaxTime 16 h, 6 H200 nodes x 8 GPUs (atl1-1-03-017-{2,9,16,23,30}-0, 018-2-0; Platinum 8562Y+ 64c, 2 TB). `coc-gpu` has no H200 (a `-p coc-gpu,ice-gpu` request gets BadConstraints). | `scontrol show partition ice-gpu`; pcie-introspection k35_1117/NOTES.md |
| Preemption | `preempt/qos` with mode REQUEUE, but no QOS lists another to preempt, so nothing is preempted in practice | `scontrol show config`, `sacctmgr show qos format=name,preempt` |
| Node sharing | partitions are `Exclusive=NO OverSubscribe=NO`: jobs share a node but never a cpu or GPU. `--exclusive` is accepted (`sbatch --test-only`). | `scontrol show partition` |
| Home quota | 30 GB (16.6 used) | `pace-quota` on ICE |
| Scratch quota | **300 GB and 1 M files**. On 2026-11-21, 262 GB (87%) and 651 k files were used before this setup; we added about 2.3 GB and 18 k files. | `pace-quota` |
| Scratch purge | No rule found. Phoenix purges files older than 60 days; on ICE, files from 2024 are still in the user's scratch, so there is no such age purge. PACE announcements mention a semester-end "scratch deleter" on ICE. Treat scratch as temporary and fetch results promptly. | `ls -la ~/scratch`; pace.gatech.edu notices |
| Maintenance | next window starts 2027-01-11 06:00 | `pace-check-queue ice-cpu` |
| Fair share | the user's FairShare is 0.0011 (heavy past use); the effect is small while ice-cpu has idle cores | `sshare -U -l` |

Our self-imposed caps (in `ice_submit.sh`): at most 20 of our ice-cpu jobs at
once and at most 40 of the user's ICE jobs in total (QOS limit 50); 1 GPU job.

## Layout on ICE

```
~/scratch/vortex-ice/
  tools/                   copy of Phoenix vortex-tools (riscv gcc 12.2, llvm-vortex 20.1.8, verilator 5.050, libc64, libcrt64)
  src/<name>/              source copies pushed by ice_push_src.sh (+ .ice_src_info)
  run_point_ice.sbatch     pushed from Phoenix vortex-exp/ice/ before every submit
  results/<NAME>/          meta.txt run.log build.log configure.log (as on Phoenix)
  slurm/point_<jobid>.out
  gpu/                     l2s_lab, tpc_lab, tpc_preload.so, tpc_bwsweep, libsmctrl (built with cuda/12.9.1, sm_90a)
  results_gpu/gpucheck_<jobid>/
```

## Helper scripts (Phoenix, `vortex-exp/ice/`)

- `ice_submit.sh` takes the same variables as `run_point.sbatch` (NAME, APP,
  ARGS, PROFILE, CORES, XCONF, PERF, DRIVER, SRC), so switching a submit is one command:

      NAME=x APP=bwstream ARGS="-n512 -r1 -p0 -u4 -b32" PROFILE=h200s CORES=4 \
        SRC=/storage/project/r-hkim358-0/echung67/vortex-exp/src_all vortex-exp/ice/ice_submit.sh

  It prints the ICE job id. Before each submit it rsyncs SRC to ICE as
  `src/<basename of SRC>`. The rsync is incremental: about 15 s when nothing
  changed, about 60 s for a first copy. `ICE_PUSH=0` skips it for a burst of
  submits from one tree. It refuses when the ICE copy with that basename came
  from a different Phoenix tree. Slurm knobs: `QOS` (default coc-ice), `CPUS`
  (8), `MEM` (32G), `TIME` (12:00:00, max 18 h), `EXCLUSIVE=1` (whole node),
  `CONSTRAINT=gold6226` (Phoenix's CPU model). Caps: `ICE_MAX_OURS` (20),
  `ICE_MAX_ALL` (40). Over a cap it exits 3; `ICE_WAIT=1` makes it wait instead.
- `ice_fetch.sh NAME...` copies `results/<NAME>/` back into
  `vortex-exp/results/<NAME>/` and appends `ran_on=ice (fetched ...)` to meta.txt.
  It skips a run that has no `rc=` line yet (`FORCE=1` fetches anyway). It never
  overwrites a Phoenix result of the same name.
- `ice_push_src.sh <phoenix-src-dir> [<name>]` sends only what a build needs:
  the top-level build files plus ci, perf, hw, sim, sw, tests, third_party
  (built), profiles and miscs. It never sends `.git`, docs, study, `*.pdf`
  (JESD238B.01.pdf), `.omc`, `.claude` or `.agents`. It writes `.ice_src_info`
  (Phoenix path, git rev, dirty count, push time), and run_point_ice.sbatch
  copies it into meta.txt. A copy such as src_all has no git, so the rev is empty.
- `run_point_ice.sbatch` is run_point.sbatch with these changes: `-p ice-cpu -q coc-ice`,
  no account, ICE paths, `module load gcc/12.3.0`, and extra meta lines
  (`cpu=`, `cluster=ice`). The build is in node-local `/tmp` (7.3 TB NVMe md
  RAID on the cpu nodes).
- `gpu_check_ice.sbatch` is the one-off GPU check below.

## What matches Phoenix and what differs

- **OS and toolchain:** identical. Both run RHEL 9.8 with glibc 2.34 and the
  spack gcc 12.3.0 at the same path. The boost 1.86 path that `vortex-tools/env.sh`
  names exists on ICE. Python is 3.9, perl 5.32, make 4.3. The copied
  tools run unchanged: no missing libraries, and no Phoenix path is embedded in
  vortex-tools. The prebuilt third_party in the source copy links and runs as is,
  so nothing was rebuilt. ICE has no ccache either, and env.sh's `OBJCACHE=` handles that.
- **CPU:** both clusters' ice-cpu / cpu-small nodes are Xeon Gold 6226 at 2.7 GHz.
  The ICE login node is a Gold 6538Y+ (do not time on it).
- **Time limits:** ice-cpu allows 18 h (Phoenix 12 h in run_point.sbatch) and
  ice-gpu 16 h. ICE has no 3-day QOS.
- **Queue:** at submit time ice-cpu had 344 idle cpus and nothing pending, and
  CPU jobs started within seconds. Later the same evening it was 91% busy. An
  `--exclusive` Gold 6226 node waited (see below).

## Validation (2026-11-21, source snapshot `vortex-exp/src_ice_val` = copy of src_all)

Each case ran with the same source and tools on both clusters: Phoenix through
run_point.sbatch with `--account=gts-hkim358`, ICE through ice_submit.sh.

| Case | Phoenix cycles | ICE cycles | All `PERF:` lines | Wall Phoenix / ICE (s) |
|---|---|---|---|---|
| `tmem_roof -m0 -n512 -i256`, h200s, 1 core | 134,378 | 134,378 | identical (5 lines) | 252 / 236 |
| `bwstream -n512 -r1 -p0 -u4 -b32`, h200s, 4 cores | 115,222 | 115,222 | identical (14 lines) | 284 / 251 |
| `sgemm -n64`, default profile | 792,351 | 792,351 | identical (4 lines) | 153 / 163 |
| rtlsim `sgemm -n16`, default profile | 18,398 | 18,398 | identical (4 lines) | 39 / 72 |

After normalising build paths, the whole run.log is the same on both clusters
except three environment lines (LD_LIBRARY_PATH and the PATH in `which ccache`)
and a `PERF=7` on make's command line in the first ICE runs. That came from
the first version of ice_submit.sh exporting PERF and is now fixed: only
variables the caller sets are forwarded. rtlsim builds on ICE with the copied
Verilator 5.050.

**Timing noise:** the bwstream case above was run 5 times on each cluster, on
shared nodes with 8 cpus and Gold 6226 (wall = blackbox build of the simulator
plus the run). Cycles were identical in all 10 runs.

| | runs (s) | mean | spread |
|---|---|---|---|
| ICE, shared nodes | 289, 290, 250, 249, 253 | 266 | 249-290 (±8%) |
| Phoenix, shared nodes | 260, 253, 279, 227, 251 | 254 | 227-279 (±10%) |
| ICE, `--exclusive` node | not measured: two `EXCLUSIVE=1 CONSTRAINT=gold6226` jobs stayed PENDING (Resources) for 30 min at 91% partition load, and I cancelled them so they would not hold a node reservation | | |

The two clusters run at the same speed, and both have shared-node noise of
about ±10%. Two ICE runs on one node (atl1-1-02-010-5-2) were the slowest
pair. For timing comparisons, pin `CONSTRAINT=gold6226`, repeat, and compare
spreads. A whole node (`EXCLUSIVE=1`) is accepted by Slurm but only starts
when a 24-core node is completely free, which can take a long time in the
evening.

## GPU side (ICE H200)

**Built on ICE: yes.** On the login node with `module load cuda/12.9.1`
(the version Phoenix's tpc_srun.sh uses) and `-arch=sm_90a`, these build
without errors: `l2s_lab`, `tpc_lab`, `tpc_bwsweep` (from bwsweep.cu), and
`tpc_preload.so`. The preload shim and libsmctrl need
`-L$CUDA/lib64/stubs`, because the login node has no driver and so no
libcuda; the shim still binds `libcuda.so.1` at run time. libsmctrl's
`libsmctrl.a`, `libsmctrl_test_gpc_info` and `libsmctrl_test_global_mask`
also build. ICE has the modules cuda 11.6-12.9.1 and 13.0.1; 12.6.1 is
loaded by the user's .bashrc.

**Run on an H200: not yet (2026-11-21 19:40).** `gpu_check_ice.sbatch` is
1 H200, 4 cpus, 20 min, `-q coc-ice --nice=200`, so it only backfills and
never jumps ahead of the user's own E13 jobs (priority 999900; ours 583133).
It is job **7232556** with `--deadline=2026-11-21T23:00`, so Slurm drops it
if it has not run by then. All 48 ICE H200 GPUs were allocated the whole
evening (`AllocTRES gres/gpu:h200=8` on every node), and the user's nine
3-GPU jobs are waiting ahead of it. If it runs, the whole answer is in
`ice:~/scratch/vortex-ice/slurm/gpucheck_7232556.out`, with the TPC map in
`results_gpu/gpucheck_7232556/map.txt`:
- GPU, driver, CUDA, compute mode, MIG mode
- the process list and the node's other jobs (exclusivity)
- NVML clocks, power and throttle reasons; the `-lgc` attempt
- `tpc_lab probe` (per-launch 128-bit masks without root; `%smid`). The SM
  count and a diff against Phoenix `h200_l2s_map.txt`.
- the masked cluster launches of tpc_gpcmap.sh (GPC layout)
- an LD_PRELOAD mask test and the libsmctrl tests
- one SM with TMA 32 KB x 6, 3 times (Phoenix 195.7-195.9 GB/s)
- full GPC6, LD16B and TMA, 3 times each, plus Phoenix's n=10 list
  verbatim (Phoenix 1277-1285 GB/s)
- `ncu` and the counter-permission check
- starting MPS as the user

To rerun later:
`ssh ice 'cd ~/scratch/vortex-ice/gpu && sbatch --nice=200 gpu_check_ice.sbatch'`.

Known without the GPU run:
- The ICE H200 nodes have driver 615.71.09, the same as the one the
  tpc_smctrl.h TMD offsets were checked on (pcie-introspection
  repo_survey_mechanism.md). Each GPU sits on its own root port and switch.
- **GPU exclusivity:** Slurm gives each job whole GPUs (`GresTypes=gpu`
  only, no mps or shard gres; `cgroup.conf ConstrainDevices=yes`, so a job
  sees only its own `/dev/nvidia*`), so the GPU itself, with its L2 and HBM, is ours alone.
  The node, though, is shared with other users' jobs on the other 7 GPUs and
  the remaining cpus. Host memory, PCIe switches, NVLink and CPU are shared,
  and `squeue` hides other users' jobs (PrivateData). On-GPU bandwidth
  numbers should match Phoenix; host-side and copy-engine measurements
  will see the neighbours.
- No clock lock without root, the same as Phoenix.

## Recommendation

- **Move to ICE: SimX sweeps.** Cycle counts are identical to Phoenix (4/4
  cases, every `PERF:` line), the nodes are the same Gold 6226 at the same
  speed, and it costs nothing. Use `ice_submit.sh` in place of
  `sbatch run_point.sbatch` and `ice_fetch.sh` afterwards. Stay at 20 jobs or
  fewer: the 50-job QOS limit also has to carry the user's GPU jobs. Runs
  longer than 12 h also fit (18 h limit).
- **Move to ICE: small and medium rtlsim.** It builds and matches (sgemm
  -n16, 18,398 cycles both). Long rtlsim runs (more than 18 h) must stay on
  Phoenix, and so must anything that needs more than 191 GB unless you
  request the 772 GB nodes.
- **Stay on Phoenix for now: H200 measurements.** ICE's H200s were fully
  booked all evening, the user's own GPU jobs queue there at higher priority,
  and the harness run on ICE has not happened yet. Once job 7232556 (or a
  rerun) confirms masks, ncu and the reference numbers, ICE is a good
  overflow for short, GPU-local measurements (TMA, L2, GPC bandwidth): the
  GPU is exclusive and the driver is the same. Host or PCIe sensitive
  measurements are better on Phoenix nodes we hold alone.
- **Stay on Phoenix:** timing-sensitive comparisons that need a quiet whole
  node, because `--exclusive` on ICE did not start within 30 min at 91% load;
  and anything reading `/storage/project` paths directly, since there is no
  shared filesystem.
- **Housekeeping:** ICE scratch was 87% full before we started. Delete
  `~/scratch/vortex-ice/src/<old copies>` and fetched results now and then.
  ICE results are not backed up, so fetch them promptly.
