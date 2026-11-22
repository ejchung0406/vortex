#!/usr/bin/bash
#SBATCH --account=gts-hkim358
#SBATCH -p cpu-small
#SBATCH -N1 --ntasks=1 --cpus-per-task=24
#SBATCH --mem=16G
#SBATCH -J perf_mt_scale
#SBATCH -o /storage/project/r-hkim358-0/echung67/vortex-exp/slurm/perf_mt_scale_%j.out
# MT agent: thread scaling + identity + determinism on a dedicated node, then the
# default-config serial-vs-MT blackbox matrix. Args: <plan file> (lines "<case> <stage> <threads>",
# stage = base | uf | ls), <src tree for the blackbox matrix or "none">.
set -u
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
PLAN=$1; SRC=$2
OUT=$EXP/results/perf_mt_scale_${SLURM_JOB_ID}; mkdir -p $OUT
L=/tmp/$USER/perf_mt_job_$SLURM_JOB_ID; mkdir -p $L/base $L/uf $L/ls
trap 'rm -rf $L' EXIT
cases=$(awk '{print $1}' $PLAN | sort -u)
for c in $cases; do
  cp -a $EXP/perf/stage_base/$c $L/base/ 2>/dev/null
  cp -a $EXP/perf/stage_mt/uf/$c $L/uf/ 2>/dev/null
  cp -a $EXP/perf/stage_mt/ls/$c $L/ls/ 2>/dev/null
done
source $EXP/perf/cases.sh
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
{ hostname; lscpu | grep -E "Model name|^CPU\(s\)|NUMA node"; uptime; } > $OUT/host.txt
while read -r c st t; do
  [ -z "$c" ] && continue
  IFS='|' read -r name app profile cores perf xconf args <<<"$(echo "$CASES" | grep "^$c|")"
  d=$L/$st/$c; o=$OUT/${c}_${st}_t${t}; i=1; while [ -e $o.r$i.log ]; do i=$((i+1)); done; o=$o.r$i
  ( cd $d; VORTEX_SIMX_THREADS=$t LD_LIBRARY_PATH=$d:$LD_LIBRARY_PATH VORTEX_DRIVER=simx VORTEX_PROFILING=${perf:-0} \
    /usr/bin/time -o $o.time -f 'wall_s=%e user_s=%U sys_s=%S rss_kb=%M cpu=%P' ./$app $args > $o.log 2>&1 )
  cyc=$(grep -a -o 'cycles=[0-9]*' $o.log | tail -1 | cut -d= -f2)
  w=$(grep -o 'wall_s=[0-9.]*' $o.time | cut -d= -f2)
  echo "$c $st t=$t $(cat $o.time) cycles=$cyc cps=$(awk -v c=${cyc:-0} -v w=${w:-1} 'BEGIN{printf "%.0f", c/w}')" | tee -a $OUT/summary.txt
done < $PLAN
[ "$SRC" = none ] && exit 0
# Default-config blackbox matrix: serial vs SIMX_MT, with and without VM.
B=$L/build; mkdir -p $B; cd $B
$SRC/configure --xlen=64 --tooldir=/storage/project/r-hkim358-0/echung67/vortex-tools > $OUT/configure.log 2>&1
make -s -j24 software > $OUT/sw.log 2>&1
run() {
  local n=$1 app=$2 args=$3 cfg=$4
  /usr/bin/time -o $OUT/bb_$n.time -f 'wall_s=%e cpu=%P' env CONFIGS="$cfg" ./ci/blackbox.sh --driver=simx --app=$app --cores=4 --l2cache ${args:+--args="$args"} > $OUT/bb_$n.log 2>&1
  echo "bb $n rc=$? $(cat $OUT/bb_$n.time) $(grep -a -o 'cycles=[0-9]*' $OUT/bb_$n.log | tail -1)" | tee -a $OUT/summary.txt
}
for spec in "sgemm|-n128" "amo|" "async_gbarrier|" "fence_mp|" "dogfood|" "bfs|"; do
  app=${spec%%|*}; args=${spec#*|}
  for vm in novm vm; do
    v=""; [ $vm = vm ] && v="-DVX_CFG_VM_ENABLE"
    run ${app}_${vm}_serial $app "$args" "$v"
    run ${app}_${vm}_mt4 $app "$args" "$v -DSIMX_MT=4"
    run ${app}_${vm}_mt2 $app "$args" "$v -DSIMX_MT=2"
  done
done
