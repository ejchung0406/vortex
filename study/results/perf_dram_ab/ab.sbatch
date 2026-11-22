#!/usr/bin/bash
#SBATCH -J perf_dram_ab
#SBATCH --account=gts-hkim358
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --time=4:00:00
#SBATCH -o /storage/project/r-hkim358-0/echung67/vortex-exp/slurm/perf_dram_ab_%j.out
# Interleaved A/B of the base stage vs the perf/dram stage (both copied to
# project storage), <reps> reps per case.
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
R=$EXP/results/perf_dram_ab
REPS=${REPS:-3}
lscpu | grep "Model name"; hostname
for i in $(seq 1 $REPS); do
  for c in $CASES; do
    for v in base dram; do
      $EXP/perf/bench.sh run $R/stage_$v $R/out_$v/r$i 1 $c | sed "s/^/$v /"
      grep -h "user_s\|wall_s" $R/out_$v/r$i/$c/time1.txt | tr '\n' ' '; echo " $v $c r$i"
    done
  done
done
