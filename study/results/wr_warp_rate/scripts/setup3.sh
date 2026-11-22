#!/usr/bin/bash
# setup3.sh <tag> <builds...>: snapshot src_wrate as p<tag> and build each named tree from it
set -e
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wr_scripts
t=${1:?tag}; shift
$D/snap.sh p$t > /dev/null
for b in "$@"; do SRC=/tmp/$USER/wrate/src_p$t $D/build.sh $b > /dev/null & done
wait
echo ok
