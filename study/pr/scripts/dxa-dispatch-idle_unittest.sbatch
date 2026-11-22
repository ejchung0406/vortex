#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=2:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
# hw/unittest/dxa_core build + run at the default and at 4 / 8 workers.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "${BUILD:?}"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"
"$SRC/configure" --xlen=32 --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
echo "src=$SRC" > "$OUT/meta.txt"
for cfg in "" "-DVX_CFG_NUM_DXA_CORES=4" "-DVX_CFG_NUM_DXA_CORES=8"; do
  tag=${cfg:-default}; tag=${tag//[^A-Za-z0-9=]/_}
  make -C hw/unittest/dxa_core clean > /dev/null 2>&1
  CONFIGS="$cfg" make -C hw/unittest/dxa_core run > "$OUT/ut_$tag.log" 2>&1
  echo "rc_$tag=$?" >> "$OUT/meta.txt"
done
echo "rc=done" >> "$OUT/meta.txt"
