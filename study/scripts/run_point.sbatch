#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=12:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# One SimX data point, in its own build tree on node-local /tmp.
#
#   sbatch -J <name> --export=ALL,NAME=<name>,APP=sgemv,ARGS="-m64 -n64",\
#          PROFILE=h100,CORES=16,XCONF="-DVX_CFG_...",PERF=7 run_point.sbatch
#
# NAME    result directory under results/
# APP     tests/regression app
# ARGS    app arguments
# PROFILE hardware profile (optional)
# CORES   core count override (optional)
# XCONF   extra -D config overrides (optional)
# PERF    --perf class (default 7)
# DRIVER  simx (default) or rtlsim
set -eo pipefail
# 2 MB pages for the heap unless the caller says otherwise: with 4 KB pages the
# data sets of this study miss the TLB (25% hit rate) and the run measures page walks.
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:-/storage/project/r-hkim358-0/echung67/vortex}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$

mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"

cd "$BUILD"
"$SRC/configure" --xlen=64 --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$OUT/build.log" 2>&1

opts=(--driver="${DRIVER:-simx}" --app="${APP:?APP is required}" --perf="${PERF:-7}")
[ -n "${PROFILE:-}" ] && opts+=(--profile="$PROFILE")
[ -n "${CORES:-}" ] && opts+=(--cores="$CORES")
[ -n "${ARGS:-}" ] && opts+=(--args="$ARGS")

{
  echo "name=$NAME"
  echo "src_rev=$(git -C "$SRC" rev-parse HEAD)"
  echo "src_dirty=$(git -C "$SRC" status --porcelain | wc -l)"
  echo "host=$(hostname)"
  echo "xconf=${XCONF:-}"
  echo "blackbox=${opts[*]}"
} > "$OUT/meta.txt"

start=$(date +%s)
CONFIGS="${XCONF:-}" ./ci/blackbox.sh "${opts[@]}" > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
echo "rc=$rc" >> "$OUT/meta.txt"
find "$BUILD" -name ramulator.stats.log -exec cp {} "$OUT/" \; 2>/dev/null || true
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
exit $rc
