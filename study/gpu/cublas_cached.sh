#!/usr/bin/bash
# cuBLAS small-batch products on matrices of several sizes, from cache-resident
# to DRAM-bound, under MPS SM limits.
# usage: cublas_cached.sh <output file> [arch]
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
  for pct in 12 6; do
    for spec in "0.004 2000" "0.016 500" "0.0625 200" "0.25 50" "1 10" "4 1"; do
      set -- $spec
      CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $bin --gib $1 --iters $2 --half $half 2>&1 | sed "s/^/pct=$pct /" >> "$out"
    done
  done
done
echo quit | nvidia-cuda-mps-control
echo done >> "$out"
