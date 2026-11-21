#!/usr/bin/bash
# Which SMs does an MPS active-thread percentage give a client? Runs the
# 4-byte plain-load stream (bwsweep pattern) on "all SMs" under several
# percentages and prints the SMs its CTAs ran on.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
M=../results_gpu/h200_tpc_map.txt
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/tpc_mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/tpc_mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
for pct in 6 11 17 18 25 36 50; do
  echo "### pct=$pct"
  CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct ./tpc_lab run --map $M --mem ld32 --per-sm 2 --mem-sms 0-131 --reps 3 --verbose 1 --tag mps$pct 2>&1
  CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct ./tpc_lab run --map $M --mem ld --per-sm 2 --mem-sms 0-131 --reps 3 --tag mps${pct}_l16 2>&1 | grep -o 'mem_gbs=[^ ]*\|mem_cta_sms=[^ ]*' | paste -s -d' '
done
echo quit | nvidia-cuda-mps-control
