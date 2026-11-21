#!/usr/bin/bash
# E2 controls: (1) several issuing warps inside the consumer CTA itself (same
# hand-off protocol as the helpers, no other SM involved); (2) the consumer
# warps' shared-memory read rate with no TMA traffic. usage: borrow_e2_controls.sh
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_tma borrow_tma.cu 2>&1 | grep -i error
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_smemread borrow_smemread.cu 2>&1 | grep -i error
out=$R/h200_borrow_e2_controls.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
for tile in 16 32; do
  for consume in 0 1; do
    for L in 1 2 3 4 7 14; do
      for D in 1 2 4; do
        ./borrow_tma --tag local --local-issuers $L --depth $D --tile-kb $tile --consume $consume --reps 5 >> $out 2>&1
      done
    done
  done
done
for w in 0 16; do
  for L in 3 7; do ./borrow_tma --tag local --local-issuers $L --depth 2 --window-mb $w --reps 5 >> $out 2>&1; done
done
for nw in 1 2 4 7 8 12 14 16; do ./borrow_smemread --nw $nw --slots 14 >> $out 2>&1; done
echo done >> $out
