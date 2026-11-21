#!/usr/bin/bash
# Reference points quoted in the report: the project's tmacopy.cu (host-timed)
# rebuilt here, and borrow_kernel with spin-waits (test_wait) instead of try_wait.
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_tma borrow_tma.cu 2>&1 | grep -i error
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_ref_tmacopy tmacopy.cu 2>&1 | grep -i error
nvcc -std=c++17 -O3 -arch=sm_90a -DBORROW_WAIT=1 -o borrow_tma_spin borrow_tma.cu 2>&1 | grep -i error
out=$R/h200_borrow_adhoc.txt
{ nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm --format=csv,noheader; hostname; date; } > $out
echo "# tmacopy.cu (one block, one thread, host chrono timing incl. launch), 1 GiB" >> $out
for d in 1 2 4 8; do ./borrow_ref_tmacopy --gib 1 --blocks 1 --depth $d --tile-kb 16 >> $out 2>&1; done
echo "# borrow_kernel split structure, try_wait (default build) vs test_wait spin (BORROW_WAIT=1)" >> $out
for d in 1 4 8 12; do
  ./borrow_tma --tag adhoc_trywait --cluster 1 --depth $d --reps 3 | grep gpu= >> $out 2>&1
  ./borrow_tma_spin --tag adhoc_spin --cluster 1 --depth $d --reps 3 | grep gpu= >> $out 2>&1
done
echo "# sanity: final borrow_tma build, E2 headline configurations" >> $out
./borrow_tma --tag adhoc_final --cluster 8 --helpers 7 --self 0 --depth 2 --reps 5 | grep gpu= >> $out
./borrow_tma --tag adhoc_final --cluster 8 --helpers 7 --self 0 --depth 2 --consume 1 --reps 5 | grep gpu= >> $out
./borrow_tma --tag adhoc_final --fused 1 --depth 6 --tile-kb 32 --reps 5 | grep gpu= >> $out
echo done >> $out
cat $out
