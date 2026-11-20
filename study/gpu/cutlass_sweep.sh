#!/usr/bin/bash
# CUTLASS RTX-Blackwell low-precision GEMMs (MXFP8, NVFP4) in a decode shape,
# C(m,n) = A(m,k) B(k,n) with a small batch m and large weights B, under MPS
# SM limits.
# usage: cutlass_sweep.sh <output file>
cd "$(dirname "$0")/cutlass_bin"
module load cuda/12.9.1
out=${1:?output file}
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
: > "$out"
K=8192; N=57344
for bin in 79c_blackwell_geforce_mixed_mxfp8_mxfp6_bf16_gemm 79a_blackwell_geforce_nvfp4_bf16_gemm; do
  for pct in 100 25 12 6; do
    for m in 1 2 4 8 16 32 64 128 256; do
      r=$(CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct ./$bin --m=$m --n=$N --k=$K --iterations=5 2>&1 | tr '\n' ' ')
      echo "bin=${bin:0:3} pct=$pct m=$m n=$N k=$K :: $r" >> "$out"
    done
  done
done
echo quit | nvidia-cuda-mps-control
echo done >> "$out"
