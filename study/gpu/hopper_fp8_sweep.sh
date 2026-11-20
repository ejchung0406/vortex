#!/usr/bin/bash
# CUTLASS Hopper FP8 warp-specialized GEMM (TMA + wgmma) in a decode shape,
# C(m,n) = A(m,k) B(k,n) with a small batch m, under MPS SM limits.
# usage: hopper_fp8_sweep.sh <output file>
cd "$(dirname "$0")/hopper_bin"
module load cuda/12.9.1
out=${1:?output file}
nvcc -std=c++17 -O3 -arch=sm_90a --expt-relaxed-constexpr -I/storage/project/r-hkim358-0/echung67/cutlass/include -I/storage/project/r-hkim358-0/echung67/cutlass/tools/util/include -I/storage/project/r-hkim358-0/echung67/cutlass/examples/common /storage/project/r-hkim358-0/echung67/cutlass/examples/54_hopper_fp8_warp_specialized_gemm/54_hopper_fp8_warp_specialized_gemm.cu -o hopper_fp8_gemm -lcuda 2>&1 | grep -i ' error' | head -5
[ -x hopper_fp8_gemm ] || { echo "build failed"; exit 1; }
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
: > "$out"
for pct in 100 50 25 12 6; do
  for m in 1 2 4 8 16 32 64 128 256; do
    r=$(CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct ./hopper_fp8_gemm --m=$m --n=57344 --k=8192 --iterations=5 2>&1 | tr '\n' ' ')
    echo "bin=h54 pct=$pct m=$m n=57344 k=8192 :: $r" >> "$out"
  done
done
echo quit | nvidia-cuda-mps-control
echo done >> "$out"
