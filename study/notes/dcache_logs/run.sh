#!/bin/bash
# usage: run.sh tag "extra configs" perf "args" [app] [builddir]
tag=$1; cfg=$2; perf=$3; args=$4; app=${5:-bwstream}; bd=${6:-/tmp/$USER/vx_dc}
S=/tmp/claude-3293147/-storage-project-r-hkim358-0-echung67-vortex/3b185c12-e36b-4e80-88fd-a9cbeeb87218/scratchpad/dcache
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
cd $bd
export VORTEX_VM_MEGAPAGE_HEAP=1
CONFIGS="-DVX_DBG_STALL_TIMEOUT=20000000 $cfg" ./ci/blackbox.sh --nohup --driver=simx --app=$app --profile=h200s --cores=1 --perf=$perf --args="$args" > $S/logs/$tag.full 2>&1
grep -aE "^(PASSED|FAILED|Error|PERF:|DCDBG|cores=)|error:|TCU_CYCLES" $S/logs/$tag.full | cut -c1-3000 > $S/logs/$tag.log
echo "== $tag"; cat $S/logs/$tag.log | head -60
