#!/usr/bin/bash
# Submit one run_point on the upstream base copy and the same on the fix copy.
#   lmem_wake_pair.sh <tag> <app> "<args>" <cores> "<xconf>" [driver]
# Results: results/pr_simx-lmem-wake_{base,fix}_<tag>
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TAG=$1; APP=$2; ARGS_=$3; CORES_=$4; XCONF_=$5; DRV=${6:-simx}
for side in base fix; do
  if [ $side = base ]; then S=$EXP/src_pr_simx-lmem-wake_base; else S=$EXP/src_pr_simx-lmem-wake; fi
  N=pr_simx-lmem-wake_${side}_${TAG}
  [ -d $EXP/results/$N ] && { echo "skip $N (exists)"; continue; }
  exp="ALL,SRC=$S,NAME=$N,APP=$APP,CORES=$CORES_,DRIVER=$DRV,PERF=6"
  ( export ARGS="$ARGS_" XCONF="$XCONF_"; [ -z "$ARGS_" ] && unset ARGS
    cd $EXP && sbatch -J pr_lw_${side}_${TAG} --mem=64G --export=$exp run_point.sbatch )
done
