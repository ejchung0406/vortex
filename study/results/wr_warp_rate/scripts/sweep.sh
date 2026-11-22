#!/usr/bin/bash
# sweep.sh <build> <out-file> <driver> <profile-or-empty> "<kinds>" "<warps>" <iters> [extra blackbox args]
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrate/${1:?b}; out=${2:?out}; drv=$3; prof=$4; kinds=$5; warps=$6; iters=$7; shift 7
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B
popt=""; [ -n "$prof" ] && popt="--profile=$prof"
: > $out
for k in $kinds; do for w in $warps; do
  ./ci/blackbox.sh --driver=$drv --app=warp_rate $popt --cores=1 --perf=1 "$@" --args="-k$k -w$w -i$iters" > $B/last.log 2>&1
  rc=$?
  { grep "^kind=" $B/last.log || echo "kind=$k W=$w FAILED rc=$rc"; grep "PERF: instrs\|PERF: stalls\|PERF: scheduler\|PERF: memory" $B/last.log; } >> $out
done; done
echo done >> $out
