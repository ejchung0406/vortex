#!/usr/bin/bash
# sweep.sh <build> <out> <driver> <profile|-> "<kinds>" "<warps>" <iters> [blackbox args...] ; CONFIGS from env
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrr/${1:?b}; out=$2; drv=$3; prof=$4; kinds=$5; warps=$6; it=$7; shift 7
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B
: > $out
names=(int_dep int_ind4 int_ind8 fma_dep fma_ind4 fma_ind8 br_every1 br_every4 br_every16 lds_own lds_chase sts_own lds_nouse)
for k in $kinds; do for w in $warps; do
  po=(); [ "$prof" != "-" ] && po=(--profile=$prof --cores=1)
  ./ci/blackbox.sh --driver=$drv "${po[@]}" --app=warp_rate --args="-k$k -w$w -i$it" "$@" > sweep_last.log 2>&1; rc=$?
  l=$(grep -o 'kind=.*' sweep_last.log | tail -1)
  echo "${l:-kind=${names[$k]} W=$w FAILED} rc=$rc passed=$(grep -c PASSED sweep_last.log) $(grep -o 'instrs=[0-9]*, cycles=[0-9]*' sweep_last.log | tail -1)" >> $out
  [ $rc -ne 0 ] && cp sweep_last.log sweep_fail_k${k}_w${w}.log
done; done
echo done >> $out
