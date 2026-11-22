#!/usr/bin/bash
# Runs on the holder node (via bys_srun.sh): bys_phase.sh <phase> [extra bys_lab args]
#   build  : compile into /tmp/$USER/bys and write the plans there
#   check  : TPC-bit map, classification and side check -> results_gpu/h200_bys_check.txt
#   <plan> : run /tmp/$USER/bys/plans/bys_<plan>.plan -> results_gpu/h200_bys_<plan>.txt
set -o pipefail
G=/storage/project/r-hkim358-0/echung67/vortex-exp/gpu
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu
B=/tmp/$USER/bys
MAPS="--map $R/h200_l2s_map.txt --gpc $R/h200_l2s_gpc.txt"
ph=$1; shift
case $ph in
build) $G/bys_build.sh && python3 $G/bys_plan.py $B/plans && cat $B/plans/*.plan > $R/h200_bys_plans.txt ;;
check) { hostname; nvidia-smi --query-gpu=name,uuid,driver_version,clocks.max.sm,power.limit --format=csv,noheader; date;
         timeout 600 $B/bys_lab check $MAPS "$@"; echo "exit=$?"; } 2>&1 | tee -a $R/h200_bys_check.txt ;;
ncuq)  ncu --query-metrics --chip gh100 2>&1 | grep -E "^(lts__|dram__|ltc|fbpa__|xbar|gpc__|gxbar)" | awk '{print $1}' > $R/h200_bys_ncu_metrics.txt; wc -l $R/h200_bys_ncu_metrics.txt ;;
ncu)   s=$1; shift; { echo "### ncu $s $(date) metrics: $NCU_METRICS";
         timeout 1500 ncu --replay-mode application -k k_bys --metrics $NCU_METRICS "$@" $B/bys_lab run $MAPS --plan $B/plans/bys_ncu.plan --only $s --nomask 1 --persist 0; echo "exit=$?"; } 2>&1 | tee -a $R/h200_bys_ncu.txt ;;
*)     { echo "### phase $ph $(date) $(hostname) args: $*";
         timeout 3000 $B/bys_lab run $MAPS --plan $B/plans/bys_$ph.plan --hist $R/h200_bys_lathist.txt "$@"; echo "exit=$?"; } 2>&1 | tee -a $R/h200_bys_$ph.txt ;;
esac
