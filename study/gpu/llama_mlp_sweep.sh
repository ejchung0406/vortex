#!/usr/bin/bash
# The Llama 70B MLP gate_up_proj product (FP16, CUTLASS cp.async + wgmma) under
# MPS SM limits: C(batch, 57344) = A(batch, 8192) x W(57344, 8192).
# usage: llama_mlp_sweep.sh <output file>
cd "$(dirname "$0")/hopper_bin"
module load cuda/12.9.1
out=${1:?output file}
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
: > "$out"
for fmt in column row; do
  for pct in 100 50 36 25 12 6; do
    for b in 1 2 4 8 16 32 64 128 256; do
      r=$(CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct ./llama_70b_mlp --batch_size $b --format $fmt 2>&1 | grep 'GFlop' | tr -s ' ')
      echo "fmt=$fmt pct=$pct batch=$b ::$r" >> "$out"
    done
  done
done
echo quit | nvidia-cuda-mps-control
echo done >> "$out"
