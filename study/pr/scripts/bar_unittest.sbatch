#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=4:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
# Builds and runs hw/unittest tops: UNITS="a b c", CONFIGS optional.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?}; TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?}; BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
mkdir -p "$OUT" "$BUILD"; trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"
"$SRC/configure" --xlen="${XLEN:-32}" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
{ echo "src=$SRC"; echo "src_rev=$(cat "$SRC/.pr_rev")"; echo "configs=${CONFIGS:-}"; echo "units=$UNITS"; } > "$OUT/meta.txt"
for u in $UNITS; do
  if make -C hw/unittest/$u > "$OUT/$u.build.log" 2>&1 && make -C hw/unittest/$u run > "$OUT/$u.run.log" 2>&1; then
    echo "$u=ok" >> "$OUT/meta.txt"
  else
    echo "$u=FAIL" >> "$OUT/meta.txt"
  fi
done
echo "rc=0" >> "$OUT/meta.txt"
