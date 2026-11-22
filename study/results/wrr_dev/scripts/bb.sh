#!/usr/bin/bash
# bb.sh <build-name> <logname> <configs> <blackbox args...>: one blackbox run; prints a summary line
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrr/${1:?build}; name=$2; cf=$3; shift 3
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B && mkdir -p logs
CONFIGS="$cf" ./ci/blackbox.sh "$@" > logs/$name.log 2>&1; rc=$?
echo "$name rc=$rc passed=$(grep -c PASSED logs/$name.log) $(grep -o 'instrs=[0-9]*, cycles=[0-9]*' logs/$name.log | tail -1) $(grep -o 'kind=.*' logs/$name.log | tail -1)"
