#!/usr/bin/bash
# Configure + build software for one tree in /tmp/$USER/pr_dxa-dispatch-idle_<side> (XLEN 64).
set -eo pipefail
side=${1:?base|br}
E=/storage/project/r-hkim358-0/echung67/vortex-exp
src=$E/src_pr_dxa-dispatch-idle; [ "$side" = base ] && src=${src}_base
B=/tmp/$USER/pr_dxa-dispatch-idle_$side
mkdir -p "$B"; cd "$B"
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
"$src/configure" --xlen=64 --tooldir=/storage/project/r-hkim358-0/echung67/vortex-tools > configure.log 2>&1
nice make -s -j3 software > build.log 2>&1
echo "built $side"
