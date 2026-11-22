#!/usr/bin/bash
# Run blackbox points sequentially in one local build tree (one CONFIGS per tree).
#   lmem_wake_local.sh <build-tree> <configs> <tag> "<app>|<blackbox opts>|<args>" ...
# The driver is built first without a time limit; only the run is timed (TMO s).
B=${1:?build}; CFG=$2; TAG=${3:?tag}; shift 3
source /storage/project/r-hkim358-0/echung67/vortex-tools/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1 MAKEFLAGS=-j3
cd "$B"; mkdir -p runs
n=0
for p in "$@"; do
  IFS='|' read -r app opts args <<< "$p"
  n=$((n+1)); log=runs/${TAG}_${n}_${app}.log
  # same CONFIGS string blackbox builds the driver with (--perf adds PERF_ENABLE)
  bcfg="$CFG"
  for o in $opts; do
    case $o in
      --cores=*) bcfg="$bcfg -DVX_CFG_NUM_CORES=${o#*=}" ;;
      --warps=*) bcfg="$bcfg -DVX_CFG_NUM_WARPS=${o#*=}" ;;
      --threads=*) bcfg="$bcfg -DVX_CFG_NUM_THREADS=${o#*=}" ;;
      --clusters=*) bcfg="$bcfg -DVX_CFG_NUM_CLUSTERS=${o#*=}" ;;
      --l2cache) bcfg="$bcfg -DVX_CFG_L2_ENABLE" ;;
    esac
  done
  bcfg="$bcfg -DPERF_ENABLE"
  CONFIGS="$bcfg" nice make -C tests/regression/$app runtime-simx > $log.build 2>&1
  s=$(date +%s)
  CONFIGS="$CFG" nice timeout ${TMO:-1200} ./ci/blackbox.sh --driver=simx --app=$app $opts --perf=6 --args="$args" > $log 2>&1; rc=$?
  cyc=$(grep -o "cycles=[0-9]*" $log | tail -1)
  res=$(grep -E -o "PASSED|FAILED" $log | tail -1)
  echo "$TAG #$n app=$app opts='$opts' args='$args' rc=$rc $res $cyc wall=$(( $(date +%s)-s ))s log=$B/$log"
done
