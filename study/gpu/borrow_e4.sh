#!/usr/bin/bash
# E4: barrier vs other CTAs' loads, lockstep vs staggered. usage: borrow_e4.sh
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_e4 borrow_e4.cu 2>&1 | grep -i error
out=$R/h200_borrow_e4_stage.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for K in 16 8 4; do
  ./borrow_e4 --mode stage --per-sm $K --stagger none --reps 3 > /tmp/$USER.e4a 2>&1; cat /tmp/$USER.e4a >> $out
  P=$(tail -1 /tmp/$USER.e4a | grep -o 'pooled_iter_cycles: p25=[0-9]* p50=[0-9]*' | grep -o 'p50=[0-9]*' | cut -d= -f2)
  for s in linear random; do ./borrow_e4 --mode stage --per-sm $K --stagger $s --period $P --reps 3 >> $out 2>&1; done
done
./borrow_e4 --mode stage --per-sm 1 --stagger none --reps 3 >> $out 2>&1
echo done >> $out
out=$R/h200_borrow_e4_probe.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for n in idle loads loads_bar; do ./borrow_e4 --mode probe --per-sm 16 --neighbours $n --reps 3 >> $out 2>&1; done
echo done >> $out
