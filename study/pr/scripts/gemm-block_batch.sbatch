#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=24:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# Several blackbox points in one build tree (tests-gemm-block PR branch;
# modelled on run_point.sbatch and fwd-hold_batch.sbatch).
# LIST is a file, one point per line:
#   name|driver|app|args|blackbox flags|CONFIGS
# Each point writes results/<name>/{run.log,meta.txt}; meta.txt ends with rc=.
#
#   sbatch -J pr_gb --export=ALL,SRC=<copy>,LIST=<file>,XLEN=32|64 gemm-block_batch.sbatch
set -o pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?SRC is required}
LIST=${LIST:?LIST is required}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
LOGDIR=$EXP/results/pr_gb_jobs/${SLURM_JOB_ID:-local}

mkdir -p "$BUILD" "$LOGDIR"
trap 'rm -rf "${BUILD:?}"' EXIT
source "$TOOLS/env.sh"

cd "$BUILD"
"$SRC/configure" --xlen="${XLEN:-32}" --tooldir="$TOOLS" > "$LOGDIR/configure.log" 2>&1 || exit 1
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$LOGDIR/build.log" 2>&1 || exit 1

while IFS='|' read -r name driver app args flags configs; do
  [ -z "$name" ] && continue
  case "$name" in \#*) continue ;; esac
  OUT=$EXP/results/$name
  mkdir -p "$OUT"
  opts=(--driver="$driver" --app="$app")
  [ -n "$args" ] && opts+=(--args="$args")
  # shellcheck disable=SC2206
  [ -n "$flags" ] && opts+=($flags)
  {
    echo "name=$name"
    echo "src=$SRC"
    echo "src_rev=$(cat "$SRC/.pr_rev" 2>/dev/null)"
    echo "xlen=${XLEN:-32}"
    echo "host=$(hostname)"
    echo "configs=$configs"
    echo "blackbox=${opts[*]}"
  } > "$OUT/meta.txt"
  start=$(date +%s)
  CONFIGS="$configs" ./ci/blackbox.sh "${opts[@]}" < /dev/null > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
  grep -o 'PERF: instrs=[0-9]*, cycles=[0-9]*' "$OUT/run.log" | tail -1 >> "$OUT/meta.txt"
  echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
  echo "rc=$rc" >> "$OUT/meta.txt"
done < "$LIST"
