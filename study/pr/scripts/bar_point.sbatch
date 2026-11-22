#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=8:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# run_point.sbatch for the bar-drain-scope PR branch: records the copy's
# .pr_rev, takes XLEN, and runs PERF only when asked.
#   sbatch -J pr_x --export=ALL,SRC=..,NAME=pr_..,APP=..,ARGS="..",XLEN=32|64,DRIVER=simx|rtlsim,XCONF=".." bar_point.sbatch
set -eo pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?SRC is required}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$

mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"

cd "$BUILD"
"$SRC/configure" --xlen="${XLEN:-64}" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$OUT/build.log" 2>&1

opts=(--driver="${DRIVER:-simx}" --app="${APP:?APP is required}")
[ -n "${PERF:-}" ] && opts+=(--perf="$PERF")
[ -n "${CORES:-}" ] && opts+=(--cores="$CORES")
[ -n "${ARGS:-}" ] && opts+=(--args="$ARGS")

{
  echo "name=$NAME"
  echo "src=$SRC"
  echo "src_rev=$(cat "$SRC/.pr_rev" 2>/dev/null)"
  echo "host=$(hostname)"
  echo "xlen=${XLEN:-64}"
  echo "xconf=${XCONF:-}"
  echo "blackbox=${opts[*]}"
} > "$OUT/meta.txt"

start=$(date +%s)
CONFIGS="${XCONF:-}" ./ci/blackbox.sh "${opts[@]}" > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
echo "rc=$rc" >> "$OUT/meta.txt"
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
exit $rc
