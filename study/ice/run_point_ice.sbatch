#!/usr/bin/bash
#SBATCH -p ice-cpu
#SBATCH -q coc-ice
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=12:00:00
#SBATCH --output=/home/hice1/echung67/scratch/vortex-ice/slurm/point_%j.out
#
# ICE copy of vortex-exp/run_point.sbatch: one SimX (or rtlsim) data point in
# its own build tree on node-local /tmp. Master copy on Phoenix:
# vortex-exp/ice/run_point_ice.sbatch (ice_submit.sh pushes it before every
# submit). Same variables as run_point.sbatch:
#
# NAME    result directory under ~/scratch/vortex-ice/results/
# APP     tests/regression app
# ARGS    app arguments
# PROFILE hardware profile (optional)
# CORES   core count override (optional)
# XCONF   extra -D config overrides (optional)
# PERF    --perf class (default 7)
# DRIVER  simx (default) or rtlsim
# SRC     source copy: a name under ~/scratch/vortex-ice/src/ (from
#         ice_push_src.sh) or an absolute ICE path (default src_all)
#
# ICE has no charge account (the association is "coc"); partition ice-cpu
# allows 18 h, QOS coc-ice 50 submitted jobs per user.
set -eo pipefail
# 2 MB pages for the heap unless the caller says otherwise: with 4 KB pages the
# data sets of this study miss the TLB (25% hit rate) and the run measures page walks.
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

ICE=$HOME/scratch/vortex-ice
SRC=${SRC:-src_all}
[[ "$SRC" == /* ]] || SRC=$ICE/src/$SRC
TOOLS=$ICE/tools
OUT=$ICE/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$

[ -f "$SRC/configure" ] || { echo "no source copy at $SRC (run ice_push_src.sh)" >&2; exit 2; }
mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
# Same host compiler as Phoenix (gcc 12.3.0 spack); the login .bashrc loads it.
type module > /dev/null 2>&1 && module load gcc/12.3.0 > /dev/null 2>&1 || true
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
  # ice_push_src.sh writes .ice_src_info (Phoenix path, git rev and dirty count at push time)
  if [ -f "$SRC/.ice_src_info" ]; then cat "$SRC/.ice_src_info"; else echo "src_rev="; echo "src_dirty="; fi
  echo "host=$(hostname)"
  echo "cpu=$(lscpu | sed -n 's/^Model name: *//p')"
  echo "cluster=ice"
  echo "xconf=${XCONF:-}"
  echo "blackbox=${opts[*]}"
} > "$OUT/meta.txt"

start=$(date +%s)
CONFIGS="${XCONF:-}" ./ci/blackbox.sh "${opts[@]}" > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
echo "rc=$rc" >> "$OUT/meta.txt"
find "$BUILD" -name ramulator.stats.log -exec cp {} "$OUT/" \; 2>/dev/null || true
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
exit $rc
