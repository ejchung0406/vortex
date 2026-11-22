#!/usr/bin/bash
# rtl_pair.sh <issue-width> <tag> <iters> "<kinds>" "<warps>" : small config (8 warps x 8 threads, no profile),
# the same sweep on rtlsim and simx, each in its own build tree
IW=$1; tag=$2; it=$3; kinds=$4; warps=$5
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wr_scripts
for drv in rtlsim simx; do
  b=s${IW}_$drv$tag
  [ -d /tmp/$USER/wrate/$b ] || $D/build.sh $b > /dev/null
  CONFIGS="-DVX_CFG_ISSUE_WIDTH=$IW ${XCONF:-}" $D/sweep.sh $b $D/small_iw${IW}_${drv}$tag.txt $drv "" "$kinds" "$warps" $it --warps=8 --threads=8
done
