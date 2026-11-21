#!/usr/bin/bash
# E3 addendum: write-only single-thread TMA stream (fused_kernel) co-resident
# with the FMA kernel and the GEMM. usage: borrow_e3b.sh
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_corun borrow_corun.cu -lcublas 2>&1 | grep -i error
out=$R/h200_borrow_e3b.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for b in 132 16 8; do
  for D in 4 8; do ./borrow_corun --mem tmafused --depth $D --fma-smem-kb 90 --mem-blocks $b --cp fma --reps 3 >> $out 2>&1; done
done
./borrow_corun --mem tmafused --depth 4 --mem-blocks 16 --cp gemm --reps 3 >> $out 2>&1
echo done >> $out
