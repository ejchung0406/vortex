#!/usr/bin/bash
# H200 follow-ups: streaming with every warp of each SM busy under MPS SM
# limits, and the co-run with one block per SM.
# usage: h200_extra2.sh <output prefix>
cd "$(dirname "$0")"
module load cuda/12.9.1
pre=${1:?output prefix}
h=$(hostname -s)
nvcc -O3 -std=c++17 -arch=sm_90 -o corun2_$h corun.cu || exit 1
: > ${pre}_corun_sep.txt
for b in 1 4 16 32 48 64; do
  gib=$(python3 -c "print(max(1, min(8, $b / 4)))")
  for c in $((132 - b)) 0; do
    ./corun2_$h --gib $gib --bw-blocks $b --cp-blocks $c --unroll 4 --smem-kb 128 --reps 3 >> ${pre}_corun_sep.txt || echo "FAILED b=$b c=$c" >> ${pre}_corun_sep.txt
  done
done
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
: > ${pre}_fullsm.txt
for pct in 100 50 36 25 18 12 6 3; do
  for u in 1 2 4 8; do
    # --blocks 0: as many 1024-thread blocks as fill every SM the process sees
    echo "pct=$pct $(CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct ./bwsweep_$h --gib 8 --blocks 0 --threads 1024 --unroll $u --reps 3 2>&1 | tail -1)" >> ${pre}_fullsm.txt
  done
done
echo quit | nvidia-cuda-mps-control
echo done >> ${pre}_fullsm.txt
