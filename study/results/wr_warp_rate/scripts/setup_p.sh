#!/usr/bin/bash
# setup_p.sh <tag>: snapshots base<tag> (HEAD + warp_rate test) and p<tag> (src_wrate) and
# their builds: bbase<tag>, bp<tag> (defaults), bp<tag>k4 (FETCH_INFLIGHT=4 runs)
set -e
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wr_scripts
t=${1:?tag}
$D/snap.sh base$t base > /dev/null
$D/snap.sh p$t > /dev/null
SRC=/tmp/$USER/wrate/src_base$t $D/build.sh bbase$t > /dev/null &
SRC=/tmp/$USER/wrate/src_p$t $D/build.sh bp$t > /dev/null &
SRC=/tmp/$USER/wrate/src_p$t $D/build.sh bp${t}k4 > /dev/null &
wait
echo ok
