#!/usr/bin/bash
# E3: TMA (and plain-load) stream co-resident with an FMA kernel and a cuBLAS
# FP16 GEMM, same process, two streams. usage: borrow_e3.sh
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_corun borrow_corun.cu -lcublas 2>&1 | grep -i error
out=$R/h200_borrow_e3.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for b in 132 66 16 8; do
  ./borrow_corun --mem tma --depth 4 --mem-blocks $b --cp fma --reps 3 >> $out 2>&1
  ./borrow_corun --mem load --mem-blocks $b --cp fma --reps 3 >> $out 2>&1
done
for b in 132 16; do
  ./borrow_corun --mem tma --depth 2 --mem-blocks $b --cp fma --reps 3 >> $out 2>&1
  ./borrow_corun --mem tma --depth 6 --fma-smem-kb 120 --mem-blocks $b --cp fma --reps 3 >> $out 2>&1
  ./borrow_corun --mem tma --depth 4 --mem-blocks $b --cp gemm --reps 3 >> $out 2>&1
  ./borrow_corun --mem tma --depth 2 --mem-blocks $b --cp gemm --reps 3 >> $out 2>&1
  ./borrow_corun --mem load --mem-blocks $b --cp gemm --reps 3 >> $out 2>&1
done
# driver-default carveout (documents that the TMA block then cannot sit beside the FMA block)
for b in 132 16; do ./borrow_corun --carveout -1 --mem tma --depth 4 --mem-blocks $b --cp fma --reps 3 >> $out 2>&1; done
echo done >> $out
# launch configuration of the cuBLAS GEMM kernel (grid, block, registers, shared memory)
out=$R/h200_borrow_e3_gemm_kernel.txt
rm -f /tmp/$USER/borrow_e3_nsys.nsys-rep /tmp/$USER/borrow_e3_nsys.sqlite; mkdir -p /tmp/$USER
nsys profile -o /tmp/$USER/borrow_e3_nsys --force-overwrite true ./borrow_corun --mem tma --mem-blocks 16 --cp gemm --reps 1 > /dev/null 2>&1
nsys stats --report cuda_gpu_kern_sum --report cuda_gpu_trace --format csv /tmp/$USER/borrow_e3_nsys.nsys-rep 2>&1 \
  | awk -F, '!/borrow_kernel|gate/' | head -40 > $out
