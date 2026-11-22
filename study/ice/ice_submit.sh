#!/usr/bin/bash
# Submit one run_point data point to PACE ICE instead of Phoenix. Same
# variables as run_point.sbatch, so switching is one command:
#
#   NAME=x APP=sgemv ARGS="-m64 -n64" PROFILE=h200s CORES=4 XCONF=... PERF=7 \
#   DRIVER=simx SRC=/storage/project/r-hkim358-0/echung67/vortex-exp/src_all \
#     vortex-exp/ice/ice_submit.sh
#
# then vortex-exp/ice/ice_fetch.sh x brings results/x back to Phoenix.
#
# SRC      Phoenix source tree (default: the vortex repo, as run_point.sbatch).
#          It is rsynced to ICE as src/<basename> before the submit
#          (incremental; ICE_PUSH=0 skips it, e.g. for a burst of submits of one
#          tree). Two different trees with one basename would collide: refused.
# Slurm knobs (all optional):
#   QOS=coc-ice (default; do not use coc-grade, its 10 slots are the user's GPU jobs)
#   CPUS=8  MEM=32G  TIME=12:00:00 (ice-cpu max 18:00:00)
#   EXCLUSIVE=1   whole node (timing runs); CONSTRAINT=gold6226 pins the CPU
#                 model (39 of ICE's 52 cpu nodes, same as Phoenix cpu-small)
#   ICE_MAX_OURS=20  our ice-cpu jobs (running + pending) allowed at once
#   ICE_MAX_ALL=40   all of the user's ICE jobs (QOS limit is 50 submitted)
#   ICE_WAIT=1       wait (poll every 60 s) instead of refusing when over a cap
# Prints the ICE job id.
set -euo pipefail
HERE=$(dirname "$(readlink -f "$0")")
SSH="ssh -o BatchMode=yes"
: "${NAME:?NAME is required}" "${APP:?APP is required}"
SRC=$(readlink -f "${SRC:-/storage/project/r-hkim358-0/echung67/vortex}")
SRCNAME=$(basename "$SRC")
MAX_OURS=${ICE_MAX_OURS:-20}
MAX_ALL=${ICE_MAX_ALL:-40}

while :; do
  read -r ours all < <($SSH ice 'echo $(squeue -u $USER -h -p ice-cpu -o %i | wc -l) $(squeue -u $USER -h -o %i | wc -l)')
  if [ "$ours" -lt "$MAX_OURS" ] && [ "$all" -lt "$MAX_ALL" ]; then break; fi
  if [ "${ICE_WAIT:-0}" != 1 ]; then
    echo "ice_submit: at cap (ice-cpu jobs $ours/$MAX_OURS, all jobs $all/$MAX_ALL); set ICE_WAIT=1 to wait" >&2
    exit 3
  fi
  sleep 60
done

# one name on ICE must mean one Phoenix tree
prev=$($SSH ice "sed -n 's/^src_phoenix=//p' ~/scratch/vortex-ice/src/$SRCNAME/.ice_src_info 2>/dev/null" || true)
if [ -n "$prev" ] && [ "$prev" != "$SRC" ]; then
  echo "ice_submit: ICE src/$SRCNAME is a copy of $prev, not $SRC" >&2; exit 2
fi
if [ "${ICE_PUSH:-1}" = 1 ] || [ -z "$prev" ]; then
  "$HERE/ice_push_src.sh" "$SRC" "$SRCNAME" > /dev/null
fi
rsync -a -e "$SSH" "$HERE/run_point_ice.sbatch" ice:scratch/vortex-ice/run_point_ice.sbatch

sb=(sbatch --parsable -J "$NAME" -q "${QOS:-coc-ice}" -c "${CPUS:-8}" --mem="${MEM:-32G}" -t "${TIME:-12:00:00}"
    --output="/home/hice1/echung67/scratch/vortex-ice/slurm/point_%j.out")
[ "${EXCLUSIVE:-0}" = 1 ] && sb+=(--exclusive)
[ -n "${CONSTRAINT:-}" ] && sb+=(-C "$CONSTRAINT")
# --export lists the values explicitly (quoted for the remote shell), so the
# job does not depend on what the ssh session happens to export.
q() { printf '%q' "$1"; }
exp="ALL,NAME=$(q "$NAME"),APP=$(q "$APP"),SRC=$(q "$SRCNAME")"
remote="$(printf '%q ' "${sb[@]}")"
# only what the caller set (an exported PERF also reaches blackbox.sh's make
# line, so it is not invented here; run_point_ice.sbatch defaults it to 7)
envs=""
for v in ARGS PROFILE CORES XCONF PERF DRIVER VORTEX_VM_MEGAPAGE_HEAP; do
  [ -n "${!v:-}" ] && envs+="$v=$(q "${!v}") "
done
# sbatch --export cannot carry commas or spaces inside values reliably, so the
# free-form variables go through the environment of the sbatch process instead.
$SSH ice "cd ~/scratch/vortex-ice && env $envs $remote --export=$exp run_point_ice.sbatch"
