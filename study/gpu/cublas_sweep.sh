#!/usr/bin/bash
# cuBLAS small-batch products under MPS active-thread percentages.
# usage: cublas_sweep.sh <output file> [arch]
cd "$(dirname "$0")"
module load cuda/12.9.1
out=${1:?output file}
arch=${2:-sm_120}
bin=./cublas_smallm_$(hostname -s)
nvcc -O3 -std=c++17 -arch=$arch -o $bin cublas_smallm.cu -lcublas || exit 1
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
: > "$out"
for half in 0 1; do
  for pct in 100 50 25 12 6 3 1; do
    CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $bin --gib 4 --half $half 2>&1 | sed "s/^/pct=$pct /" >> "$out"
  done
done
echo quit | nvidia-cuda-mps-control
echo done >> "$out"
