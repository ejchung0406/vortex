#!/usr/bin/bash
#SBATCH -A gts-hkim358-paid
#SBATCH -p cpu-small
#SBATCH -N1 --ntasks=1 --cpus-per-task=24
#SBATCH --mem=32G
#SBATCH -t 12:00:00
#SBATCH -J perf_mt_scale
#SBATCH -o /storage/project/r-hkim358-0/echung67/vortex-exp/slurm/perf_mt_scale_%j.out
# MT scaling sweep: staged serial base + MT builds, run at several thread counts.
# Args: <plan file> with lines "<case> <stage:base|mt> <threads>"
set -u
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
PLAN=$1
OUT=$EXP/results/perf_mt_scale_${SLURM_JOB_ID}
mkdir -p $OUT
L=/tmp/$USER/perf_mt_job_$SLURM_JOB_ID
mkdir -p $L
cp -a $EXP/results/perf_mt_tools/stage_base $EXP/results/perf_mt_tools/stage_mt $L/
source $EXP/perf/cases.sh
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
lscpu | grep -E "Model name|^CPU\(s\)|Thread|NUMA node" > $OUT/host.txt; hostname >> $OUT/host.txt
while read -r c st t; do
  [ -z "$c" ] && continue
  IFS='|' read -r name app profile cores perf xconf args <<<"$(echo "$CASES" | grep "^$c|")"
  d=$L/stage_$st/$c; o=$OUT/${c}_${st}_t${t}; i=1; while [ -e $o.r$i.log ]; do i=$((i+1)); done; o=$o.r$i
  ( cd $d; VORTEX_SIMX_THREADS=$t LD_LIBRARY_PATH=$d:$LD_LIBRARY_PATH VORTEX_DRIVER=simx VORTEX_PROFILING=${perf:-0} \
    /usr/bin/time -o $o.time -f 'wall_s=%e user_s=%U sys_s=%S rss_kb=%M cpu=%P' ./$app $args > $o.log 2>&1 )
  cyc=$(grep -a -o 'cycles=[0-9]*' $o.log | tail -1 | cut -d= -f2)
  w=$(grep -o 'wall_s=[0-9.]*' $o.time | cut -d= -f2)
  echo "$c $st t=$t $(cat $o.time) cycles=$cyc cps=$(awk -v c=${cyc:-0} -v w=$w 'BEGIN{printf "%.0f", c/w}')" | tee -a $OUT/summary.txt
done < $PLAN
rm -rf $L
