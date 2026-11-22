#!/usr/bin/bash
# SimX points, serial, in the local build trees. usage: <side> <tag> <app> "<args>" <ndxa|d> ["<extra configs>"] [appconf]
side=$1; tag=$2; app=$3; args=$4; ndxa=$5; extra=${6:-}; appconf=${7:-0}
E=/storage/project/r-hkim358-0/echung67/vortex-exp
B=/tmp/$USER/pr_dxa-dispatch-idle_$side
OUT=$E/results/pr_ddi_${tag}_w${ndxa}_${side}_simx
mkdir -p "$OUT"
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1 MAKEFLAGS=-j3
xc="-DVX_CFG_EXT_DXA_ENABLE${extra:+ $extra}"
[ "$ndxa" != d ] && xc="$xc -DVX_CFG_NUM_DXA_CORES=$ndxa"
cd "$B"
{ echo "side=$side"; echo "xlen=64"; echo "xconf=$xc"; echo "app=$app args=$args appconf=$appconf"; } > "$OUT/meta.txt"
if [ "$appconf" = 1 ]; then
  make -C tests/regression/$app clean > /dev/null 2>&1
  CONFIGS="$xc" nice make -s -C tests/regression/$app > "$OUT/app_build.log" 2>&1
fi
CONFIGS="$xc" nice ./ci/blackbox.sh --driver=simx --app=$app ${args:+--args="$args"} > "$OUT/run.log" 2>&1
echo "rc=$?" >> "$OUT/meta.txt"
if [ "$appconf" = 1 ]; then
  make -C tests/regression/$app clean > /dev/null 2>&1
fi
