#!/usr/bin/bash
#SBATCH --cpus-per-task=4
#SBATCH --mem=24G
#SBATCH --time=1:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
# One tree (SIDE=base|br), one software build, many rtlsim points (XLEN 64).
# POINTS: ';'-separated "tag|app|args|ndxa|extra|appconf" entries.
set -o pipefail
export VORTEX_VM_MEGAPAGE_HEAP=1
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SIDE=${SIDE:?}
SRC=$EXP/src_pr_dxa-dispatch-idle; [ "$SIDE" = base ] && SRC=${SRC}_base
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
LIMIT=${LIMIT:-2700}
mkdir -p "$BUILD"; trap 'rm -rf "${BUILD:?}"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"
t0=$(date +%s)
"$SRC/configure" --xlen=64 --tooldir="$TOOLS" > configure.log 2>&1
make -s -j4 software > build.log 2>&1 || { cp build.log $EXP/results/pr_ddi_rtlbatch_${SIDE}_build.log; exit 1; }
echo "software built in $(( $(date +%s) - t0 ))s"
IFS=';' read -ra pts <<< "$POINTS"
for p in "${pts[@]}"; do
  IFS='|' read -r tag app args ndxa extra appconf <<< "$p"
  OUT=$EXP/results/pr_ddi_${tag}_w${ndxa}_${SIDE}_rtlsim
  mkdir -p "$OUT"
  xc="-DVX_CFG_EXT_DXA_ENABLE${extra:+ $extra}"
  [ "$ndxa" != d ] && xc="$xc -DVX_CFG_NUM_DXA_CORES=$ndxa"
  { echo "side=$SIDE"; echo "xlen=64"; echo "xconf=$xc"; echo "app=$app args=$args appconf=$appconf"; echo "host=$(hostname)"; } > "$OUT/meta.txt"
  if [ $(( $(date +%s) - t0 )) -gt $LIMIT ]; then echo "rc=skipped_time" >> "$OUT/meta.txt"; continue; fi
  if [ "$appconf" = 1 ]; then
    make -C tests/regression/$app clean > /dev/null 2>&1
    CONFIGS="$xc" make -s -C tests/regression/$app > "$OUT/app_build.log" 2>&1
  fi
  s=$(date +%s)
  CONFIGS="$xc" MAKEFLAGS=-j4 ./ci/blackbox.sh --driver=rtlsim --app=$app ${args:+--args="$args"} > "$OUT/run.log" 2>&1
  echo "rc=$?" >> "$OUT/meta.txt"
  echo "wall_s=$(( $(date +%s) - s ))" >> "$OUT/meta.txt"
  [ "$appconf" = 1 ] && make -C tests/regression/$app clean > /dev/null 2>&1
done
echo "total $(( $(date +%s) - t0 ))s"
