#!/usr/bin/bash
# run.sh <build-name> <log> <blackbox args...>; CONFIGS from the environment
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrate/${1:?name}; log=${2:?log}; shift 2
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B
./ci/blackbox.sh "$@" > $log 2>&1; echo "rc=$?" >> $log
