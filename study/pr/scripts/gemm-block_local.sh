#!/usr/bin/bash
# Local (non-Slurm) version of gemm-block_batch.sbatch for small SimX points.
#   SRC=<copy> LIST=<file> BUILD=/tmp/$USER/pr_tests-gemm-block_<x> [XLEN=64] [CONFIGURE=1] gemm-block_local.sh
# Runs under nice with make -j3 (shared node). Results go to results/<name>/.
set -o pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?}; LIST=${LIST:?}; BUILD=${BUILD:?}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
export MAKEFLAGS=-j3
source "$TOOLS/env.sh"
mkdir -p "$BUILD"
cd "$BUILD" || exit 1
if [ "${CONFIGURE:-0}" = 1 ] || [ ! -f "$BUILD/config.mk" ]; then
  "$SRC/configure" --xlen="${XLEN:-64}" --tooldir="$TOOLS" > "$BUILD/configure.log" 2>&1 || exit 1
  nice make -s -j3 software > "$BUILD/build.log" 2>&1 || exit 1
fi
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
    echo "xlen=${XLEN:-64}"
    echo "host=$(hostname) (local)"
    echo "configs=$configs"
    echo "blackbox=${opts[*]}"
  } > "$OUT/meta.txt"
  start=$(date +%s)
  CONFIGS="$configs" nice ./ci/blackbox.sh "${opts[@]}" < /dev/null > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
  grep -o 'PERF: instrs=[0-9]*, cycles=[0-9]*' "$OUT/run.log" | tail -1 >> "$OUT/meta.txt"
  echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
  echo "rc=$rc" >> "$OUT/meta.txt"
done < "$LIST"
