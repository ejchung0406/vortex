#!/usr/bin/bash
# Experiment A runner, inside the holder: gng_run_a.sh <kind: smoke|same|mps> [first_slot last_slot]
# same: one process, two streams. mps: MPS daemon, one lane client + one lender client
# with the same schedule epoch. Raw lines go to results_gpu/h200_gng_a_<kind>[_<n>].txt
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
tar xzf ~/scratch/gng_bin.tgz -C /tmp/$USER/gng 2>/dev/null; tar xzf ~/scratch/gng_sw.tgz -C /tmp/$USER/gng 2>/dev/null
B=/tmp/$USER/gng; M=../results_gpu/h200_gng_map.txt
kind=$1; a=${2:-0}; b=${3:-100000}
mkdir -p $B/sched; python3 gng_sched.py $kind $M $B/sched/$kind.txt
out=../results_gpu/h200_gng_a_${kind}${SUFFIX}.txt
t0=$(( $(date +%s%N) + ${SETUP_S:-60} * 1000000000 ))
if [ $kind = mps ]; then
  export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/a_mps_pipe CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/a_mps_log
  mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
  nvidia-cuda-mps-control -d || { echo "MPS daemon failed"; exit 1; }
  sleep 2
  $B/gng_corun --map $M --schedule $B/sched/$kind.txt --roles lane --t0 $t0 --first-slot $a --last-slot $b > $out.lane 2>&1 &
  p1=$!
  $B/gng_corun --map $M --schedule $B/sched/$kind.txt --roles lender --t0 $t0 --first-slot $a --last-slot $b > $out.lender 2>&1 &
  p2=$!
  wait $p1 $p2
  echo quit | nvidia-cuda-mps-control
  cat $out.lane $out.lender > $out
else
  $B/gng_corun --map $M --schedule $B/sched/$kind.txt --roles lane,lender --t0 $t0 --first-slot $a --last-slot $b > $out 2>&1
fi
grep -c "^slot=" $out
