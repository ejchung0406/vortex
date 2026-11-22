#!/usr/bin/bash
# build.sh <build-name>: configure + make software for src_wrate in node-local /tmp
set -eo pipefail
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
SRC=${SRC:-/tmp/$USER/wrate/src_base}
B=/tmp/$USER/wrate/${1:?name}
source $TOOLS/env.sh
mkdir -p $B && cd $B
$SRC/configure --xlen=64 --tooldir=$TOOLS > configure.log 2>&1
make -s -j${JOBS:-8} software > build.log 2>&1
echo built $B
