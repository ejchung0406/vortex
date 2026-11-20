#!/bin/bash
# usage: tree.sh <tree> "<extra configs>" <tag> <perf> <spec>...   spec = name:app:args  (driver built once, sims run in parallel)
tree=$1; cfg=$2; tag=$3; perf=$4; shift 4
S=/tmp/claude-3293147/-storage-project-r-hkim358-0-echung67-vortex/3b185c12-e36b-4e80-88fd-a9cbeeb87218/scratchpad/dcache
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
T=/tmp/$USER/$tree
cd $T || exit 1
export VORTEX_VM_MEGAPAGE_HEAP=1 PROFILE=h200s VORTEX_PROFILING=$perf
C="-DVX_DBG_STALL_TIMEOUT=20000000 $cfg -DVX_CFG_NUM_CORES=1 -DPERF_ENABLE"
make -C sw/runtime/stub > /dev/null 2>&1
apps=$(for s in "$@"; do echo $s | cut -d: -f2; done | sort -u)
first=$(echo $apps | cut -d' ' -f1)
CONFIGS="$C" make -C tests/regression/$first runtime-simx > $S/logs/${tag}.build 2>&1 || { echo "$tag driver build failed"; exit 1; }
for a in $apps; do
  CONFIGS="$C" make -C tests/regression/$a $a kernel.vxbin >> $S/logs/${tag}.build 2>&1 || { echo "$tag app $a build failed"; exit 1; }
done
for s in "$@"; do
  name=$(echo $s | cut -d: -f1); app=$(echo $s | cut -d: -f2); args=$(echo $s | cut -d: -f3-)
  ( cd tests/regression/$app && LD_LIBRARY_PATH=$T/sw/runtime:$LD_LIBRARY_PATH VORTEX_DRIVER=simx ./$app $args > $S/logs/${tag}_$name.full 2>&1
    grep -aE "^(PASSED|FAILED|Error|PERF:|DCDBG|cores=)|error:|TCU_CYCLES" $S/logs/${tag}_$name.full | cut -c1-3000 > $S/logs/${tag}_$name.log ) &
done
wait
echo "$tag done"
