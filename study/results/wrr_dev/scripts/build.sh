#!/usr/bin/bash
# build.sh <src-name> <build-name>: configure + make software in node-local /tmp (run inside a job)
set -eo pipefail
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
SRC=/storage/project/r-hkim358-0/echung67/vortex-exp/src_wr_${1:?src}
B=/tmp/$USER/wrr/${2:?build}
source $TOOLS/env.sh
mkdir -p $B && cd $B
$SRC/configure --xlen=64 --tooldir=$TOOLS > configure.log 2>&1
make -s -j${JOBS:-8} software > build.log 2>&1 || { tail -30 build.log; exit 1; }
echo built $B
