#!/usr/bin/bash
# E4 addendum: stage loop on 8 SMs only (no DRAM saturation) and probe kinds.
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_e4 borrow_e4.cu 2>&1 | grep -i error
out=$R/h200_borrow_e4_stage8.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for K in 16 8; do
  ./borrow_e4 --mode stage --per-sm $K --active-sms 8 --stagger none --iters 2000 --reps 3 > /tmp/$USER.e4b 2>&1; cat /tmp/$USER.e4b >> $out
  P=$(tail -1 /tmp/$USER.e4b | grep -o 'pooled_iter_cycles: p25=[0-9]* p50=[0-9]*' | grep -o 'p50=[0-9]*' | cut -d= -f2)
  for s in linear random; do ./borrow_e4 --mode stage --per-sm $K --active-sms 8 --stagger $s --period $P --iters 2000 --reps 3 >> $out 2>&1; done
done
./borrow_e4 --mode stage --per-sm 1 --active-sms 8 --stagger none --iters 2000 --reps 3 >> $out 2>&1
echo done >> $out
out=$R/h200_borrow_e4_probe2.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for kind in smem_bar bar_only smem_only; do
  for n in idle loads loads_bar; do ./borrow_e4 --mode probe --probe-kind $kind --per-sm 16 --neighbours $n --reps 2 >> $out 2>&1; done
done
echo done >> $out
cat $R/h200_borrow_e4_stage8.txt $out
