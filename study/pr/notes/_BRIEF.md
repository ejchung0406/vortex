# Shared brief for every agent preparing an upstream PR branch

Goal of the effort: clean, reviewable upstream pull-request BRANCHES for
vortexgpgpu/vortex, made from a research fork. Nothing is pushed, no PR or
issue is opened; the user sends them by hand later.

## Places
- Fork checkout (READ ONLY for you): `/storage/project/r-hkim358-0/echung67/vortex`,
  branch `feature/dxa-borrowing`. Never edit, commit on, check out in, or push it.
  Read fork commits with `git -C <fork> show <hash>`.
- Upstream: remote `upstream`, base `upstream/master` (ccf811e11). The fork's merge
  base with it is 01d0ac750; upstream is 5 commits ahead and touched
  `hw/rtl/tcu/VX_tcu_bbuf.sv`, `sim/simx/tcu/tcu_unit.cpp`, `sim/simx/dxa/dxa_core.cpp`,
  `hw/rtl/dxa/VX_dxa_setup.sv`, `VX_dxa_smem_wr.sv`, `sw/runtime/include/dxa.h`.
  The fork ALSO merged upstream's unmerged `feature/tmem-dataflow` (merge 7252188f7,
  2026-11-21): fork commits after it may lean on code that is not on upstream/master.
- Your worktree: `/storage/project/r-hkim358-0/echung67/vortex-exp/pr/<slug>`, branch
  `pr/<slug>`, already created off `upstream/master`. Commit only there. For a
  stacked branch, create it yourself:
  `git -C /storage/project/r-hkim358-0/echung67/vortex worktree add /storage/project/r-hkim358-0/echung67/vortex-exp/pr/<slug2> -b pr/<slug2> pr/<slug>`.
- Candidates and evidence: `<fork>/study/notes/upstream-pr-candidates.md` and
  `upstream-issue-drafts.md`. Extra notes and patches:
  `/storage/project/r-hkim358-0/echung67/vortex-exp/notes/`.
- Your PR note: `/storage/project/r-hkim358-0/echung67/vortex-exp/pr/notes/<slug>.md`
  (NOT in the branch).

## Rules of the repository (read before editing)
`AGENTS.md`, `docs/review/code-review-policy.md`, `docs/bug_fixes.md`,
`docs/coding_guidelines_cpp.md` / `_verilog.md`, and the skills
`.agents/skills/{code-review,rtl-review,simx-review}/SKILL.md` in your worktree.
Every branch must satisfy them. In particular:
- comments say why, never name the task/PR/issue/fix; default to no comment;
- RTL and SimX move together (lockstep); a cycle-moving change needs a
  `model_parity` case in `ci/testcases/`;
- a new `VX_CFG_*` knob is R3: profiles, CI catalog and docs that name the knob
  are updated; a new test gets a CI case (`ci/testcases/*.yaml`) and an entry in
  `tests/regression/Makefile` as the neighbours have;
- never hand-edit `ci/baselines/**` (golden data); if a baseline would move, say
  so in the PR note as something the maintainer re-records;
- CHANGELOG.md entry if neighbours of that kind have one;
- run `python3 ci/change_impact.py explain --base upstream/master --head HEAD`
  (see policy section 5 for the exact form) in your worktree and report the class.

## Rules of this effort
- One topic per branch. Cherry-pick (`git cherry-pick -x` is NOT wanted: no fork
  hashes in upstream commit messages) or re-implement minimally on upstream/master.
- Nothing fork-only: no `h200s`/`h200q`/`h100q` profiles, no `study/`, no fork test
  that upstream lacks unless this branch is the one adding it.
- Keep default behaviour identical where the fork did (knobs default off).
- Commit messages: upstream style (`area: what` — see `git log upstream/master`),
  body explains why and what was measured, and ends with exactly these trailers:
  ```
  Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01S5PBErhG8sHe1yUkDDxz1o
  ```
  Author stays the configured git user. No push, no `gh pr`, no `gh issue`.
- Do not touch other worktrees, or any `/storage/project/r-hkim358-0/echung67/vortex-exp/src_*`
  directory not created by this effort. Do not cancel or modify Slurm jobs that
  are not yours (the user has ~100 `noc_*` jobs of other work in the queue).

## Building and running
- Build copy: `cp -a /storage/project/r-hkim358-0/echung67/vortex-exp/src_all /storage/project/r-hkim358-0/echung67/vortex-exp/src_pr_<slug>`
  then, inside the copy, remove what the fork tracks and overlay your branch:
  `cd src_pr_<slug> && git -C <worktree> archive HEAD | tar -x`. NOTE `src_all`
  is a snapshot of the FORK: files that exist on the fork but not on your
  branch stay behind after the overlay (fork tests, profiles, `study/`,
  changed sources you did not touch!). The overlay overwrites every file your
  branch tracks, so tracked files are right, but stale fork-only files remain:
  delete the copy's tracked-looking tree first and keep only `third_party`
  build products, e.g. overlay into an empty dir and copy just
  `src_all/third_party` over it (then overlay again so upstream's
  `third_party/Makefile` wins). `src_all/third_party` was built for the fork
  (it has a Windowed ramulator controller that upstream's Makefile does not
  build); if in doubt rebuild third_party as `docs/install_vortex.md` says.
  For a plain upstream reference make `src_pr_<slug>_base` the same way from
  `upstream/master`. Re-overlay after every new commit you want to test.
- Run: `/storage/project/r-hkim358-0/echung67/vortex-exp/run_point.sbatch` (read
  it): `sbatch -J pr_<x> --export=ALL,SRC=<copy>,NAME=pr_<slug>_<x>,APP=...,ARGS="...",CORES=..,XCONF="-D...",DRIVER=simx|rtlsim,PERF=.. run_point.sbatch`.
  Results land in `/storage/project/r-hkim358-0/echung67/vortex-exp/results/<NAME>/`
  (`run.log`, `meta.txt` with `rc=`). NAME must start with `pr_`. Use
  `--mem=64G` for multi-core or rtlsim. rtlsim small configurations take
  minutes to tens of minutes. `VORTEX_VM_MEGAPAGE_HEAP=1` is the script default;
  keep it. Jobs currently start within a few minutes. Wait for jobs with a
  background until-loop on `meta.txt` containing `rc=` (foreground `sleep` is
  blocked), not by busy polling. An XCONF value with commas breaks `--export`:
  then export the variables in the environment and use `--export=ALL`.
- The app is built by blackbox from the build tree with default macros; when a
  test needs CONFIGS on the app side too, check how `run_point.sbatch` passes
  XCONF (it sets CONFIGS for blackbox only) and, if the app must be rebuilt
  with them, write your own small sbatch script under
  `/storage/project/r-hkim358-0/echung67/vortex-exp/pr/scripts/` modelled on it.
- This shell is inside a shared 8-core job: do not run builds or simulations
  here beyond seconds-long commands; use sbatch.

## What to verify on the branch (report `run` / `not run` / `failed` honestly)
1. It builds (SimX; rtlsim when RTL or a shared file changed).
2. The tests named in the candidate's evidence, on SimX and on rtlsim, at small
   configurations.
3. Defaults are cycle-identical to plain `upstream/master` for a few unrelated
   tests (e.g. `sgemm -n16`, `dxa_copy`, `sgemm_tcu`, `vecadd`; pick ones that
   exercise the code you touched with the knob off).
4. For a bug fix: the reproducer FAILS on plain upstream/master and PASSES on
   the branch. If it does not fail on upstream/master, stop and report that:
   the bug may be fork-specific or already fixed. Do not invent evidence.
A test that did not run is not a pass. Numbers in the PR note must come from
runs you made on this branch (cite results/<NAME>); numbers quoted from the
fork's notes must be labelled as fork measurements.

## PR note (`pr/notes/<slug>.md`)
Title (upstream style); Summary; Motivation with evidence numbers; What
changed (files); How verified (commands, result dirs, numbers); Change class
(R0-R3 from `change_impact.py`, plus detectors hit) and what the policy still
requires that you could not do (Beacon `logic_gate` logic depth, perf-baseline
re-record, xrt run, maintainer approval, independent review); Dependencies on
other PR branches; Open questions for the maintainer. Write it as the PR
description the user will paste, followed by a short "notes for the sender"
section. English, plain, no emojis.

## Final report to the lead
Branch name(s) and commit hashes; class; a verification table (what, driver,
config, result, results dir); anything that did not reproduce on
upstream/master; conflicts you expect with sibling branches (which files);
what the user must still do by hand.
