#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=12:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# war_order over several argument sets in one build tree (modelled on
# run_point.sbatch). POINTS is a ';'-separated list of --args strings.
#
#   sbatch -J pr_x --export=ALL,SRC=<copy>,NAME=pr_..,POINTS="-n65536;-n65536 -s4 -d1",\
#          XCONF="-D..",DRIVER=simx,CORES=1 simx-dcache-order_sweep.sbatch
set -eo pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?SRC is required}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
APP=${APP:-war_order}

mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"

cd "$BUILD"
"$SRC/configure" --xlen="${XLEN:-64}" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$OUT/build.log" 2>&1

{
  echo "name=$NAME"
  echo "src=$SRC"
  echo "host=$(hostname)"
  echo "xconf=${XCONF:-}"
  echo "driver=${DRIVER:-simx} cores=${CORES:-} app=$APP xlen=${XLEN:-64}"
} > "$OUT/meta.txt"

start=$(date +%s)
fails=0
i=0
IFS=';' read -ra pts <<< "${POINTS:?POINTS is required}"
for a in "${pts[@]}"; do
  opts=(--driver="${DRIVER:-simx}" --app="$APP" --perf="${PERF:-1}" --args="$a")
  [ -n "${CORES:-}" ] && opts+=(--cores="$CORES")
  CONFIGS="${XCONF:-}" ./ci/blackbox.sh "${opts[@]}" > "$OUT/run_$i.log" 2>&1 && r=0 || r=$?
  cyc=$(grep -o "cycles=[0-9]*" "$OUT/run_$i.log" | tail -1 || true)
  err=$(grep -o "Found [0-9]* errors (error bits 0x[0-9a-f]*)" "$OUT/run_$i.log" | tail -1 || true)
  echo "point $i args=\"$a\" rc=$r $cyc $err" >> "$OUT/meta.txt"
  if [ $r -ne 0 ]; then fails=$((fails + 1)); fi
  i=$((i + 1))
done
echo "fails=$fails of $i" >> "$OUT/meta.txt"
echo "rc=0" >> "$OUT/meta.txt"
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
