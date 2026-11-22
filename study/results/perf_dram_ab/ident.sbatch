#!/usr/bin/bash
#SBATCH -J perf_dram_ident
#SBATCH --account=gts-hkim358
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --time=1:00:00
#SBATCH -o /storage/project/r-hkim358-0/echung67/vortex-exp/slurm/perf_dram_ident_%j.out
# Runs the perf/dram stage once per case and checks identity against the base logs.
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
R=$EXP/results/perf_dram_ab
O=$R/ident_$SLURM_JOB_ID
for c in $CASES; do
  rm -f $R/stage_dram/$c/ramulator.stats.log
  $EXP/perf/bench.sh run $R/stage_dram $O 1 $c
  echo -n "$c: "; $EXP/perf/identical.sh $EXP/results/perf_base/$c/run1.log $O/$c/run1.log | head -5
  if [ -f $R/stats_base/$c.log ]; then cmp -s $R/stats_base/$c.log $R/stage_dram/$c/ramulator.stats.log && echo "  stats identical" || echo "  STATS DIFFER"; fi
done
