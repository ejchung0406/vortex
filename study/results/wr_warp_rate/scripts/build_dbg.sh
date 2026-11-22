#!/usr/bin/bash
# build_dbg.sh <build-name> : debug-trace build from a copy of src_wrate. The
# __assert macro is renamed in the copy, since the host glibc's assert.h
# re-declares __assert (3 args) on every include in a debug build.
set -eo pipefail
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
n=${1:?name}
S=$(/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wr_scripts/snap.sh dbg_$n)
grep -rl "__assert(" $S/sim $S/sw/common | xargs sed -i 's/\b__assert(/__vx_assert(/g'
source $TOOLS/env.sh
rm -rf /tmp/$USER/wrate/$n; mkdir -p /tmp/$USER/wrate/$n && cd /tmp/$USER/wrate/$n
$S/configure --xlen=64 --tooldir=$TOOLS > configure.log 2>&1
make -s -j8 software > build.log 2>&1
echo built
