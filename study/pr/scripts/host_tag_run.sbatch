#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=6:00:00
#SBATCH --account=gts-hkim358
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# Configure + build one source copy on node-local /tmp, then run the bash
# script RUNSH inside the build directory (XLEN from XLEN, default 64).
#   sbatch -J pr_x --export=ALL,SRC=<copy>,NAME=pr_<..>,RUNSH=<script>,XLEN=64 host_tag_run.sbatch
# RUNSH writes its own logs into $OUT; meta.txt gets rc= at the end.
set -o pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$

mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"
export OUT

cd "$BUILD"
{
  echo "name=$NAME"
  echo "src=$SRC"
  echo "host=$(hostname)"
  echo "xlen=${XLEN:-64}"
  echo "runsh=$RUNSH"
  echo "aslr=$(cat /proc/sys/kernel/randomize_va_space)"
} > "$OUT/meta.txt"
cp "$RUNSH" "$OUT/runsh.sh"
"$SRC/configure" --xlen="${XLEN:-64}" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$OUT/build.log" 2>&1 || { echo "rc=build" >> "$OUT/meta.txt"; exit 1; }

start=$(date +%s)
bash "$RUNSH" > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
echo "rc=$rc" >> "$OUT/meta.txt"
exit $rc
