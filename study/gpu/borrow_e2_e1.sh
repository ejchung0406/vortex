#!/usr/bin/bash
# E2 (cluster-TMA borrowing), E1 (per-SM bandwidth, L2 vs DRAM) and the
# placement/latency probes on the GPU of the current allocation.
# usage: borrow_e2_e1.sh   (writes ../results_gpu/h200_borrow_*.txt)
cd "$(dirname "$0")"
module load cuda/12.9.1 >/dev/null 2>&1
R=../results_gpu
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_tma borrow_tma.cu 2>&1 | grep -i error
nvcc -std=c++17 -O3 -arch=sm_90a -o borrow_load borrow_load.cu 2>&1 | grep -i error
hdr() { nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.sm,memory.total --format=csv,noheader; hostname; date; }

# --- placement and latency
out=$R/h200_borrow_placement.txt; hdr > $out
for c in 16 8 4 2; do ./borrow_tma --placement $c >> $out 2>&1; done
out=$R/h200_borrow_latency.txt; hdr > $out
for c in 2 8 16; do ./borrow_tma --latency 33 --cluster $c >> $out 2>&1; done
./borrow_tma --latency 33 --cluster 2 --tile-kb 32 >> $out 2>&1

# --- E2, one cluster (isolated)
out=$R/h200_borrow_e2_isolated.txt; hdr > $out
for tile in 16 32; do
  for consume in 0 1; do
    for ch in 2:1 4:1 4:2 4:3 8:1 8:2 8:3 8:5 8:7 16:7 16:11 16:14 16:15; do
      C=${ch%%:*}; h=${ch##*:}
      for self in 0 1; do
        for D in 1 2 4 8; do
          ./borrow_tma --tag iso --cluster $C --helpers $h --self $self --consume $consume --depth $D --tile-kb $tile --reps 5 >> $out 2>&1
        done
      done
    done
  done
done
# staged with more consumer warps
for ch in 4:3:4 8:7:2 16:14:1; do
  IFS=: read C h D <<< "$ch"
  for nw in 8 16; do
    ./borrow_tma --tag iso_nw --cluster $C --helpers $h --self 0 --consume 1 --depth $D --nw $nw --reps 5 >> $out 2>&1
  done
done
# L2-resident source (16 MB window), write-only and staged
for tile in 16 32; do
  for consume in 0 1; do
    for ch in 2:1 4:3 8:7 16:14; do
      C=${ch%%:*}; h=${ch##*:}
      for D in 1 2 4 8; do
        ./borrow_tma --tag iso_l2 --window-mb 16 --cluster $C --helpers $h --self 0 --consume $consume --depth $D --tile-kb $tile --reps 5 >> $out 2>&1
      done
    done
  done
done
echo done >> $out

# --- E2 baseline (c): independent CTAs fetching into their own shared memory
out=$R/h200_borrow_e2_baseline.txt; hdr > $out
for n in 1 2 4 8 16; do
  for D in 1 2 4 8 12; do ./borrow_tma --tag base_fused --fused 1 --clusters $n --depth $D --reps 5 >> $out 2>&1; done
  for D in 1 2 3 6; do ./borrow_tma --tag base_fused --fused 1 --clusters $n --depth $D --tile-kb 32 --reps 5 >> $out 2>&1; done
  for consume in 0 1; do
    for D in 1 2 4 8 12; do ./borrow_tma --tag base_split --cluster 1 --clusters $n --consume $consume --depth $D --reps 5 >> $out 2>&1; done
    for D in 2 6; do ./borrow_tma --tag base_split --cluster 1 --clusters $n --consume $consume --depth $D --tile-kb 32 --reps 5 >> $out 2>&1; done
  done
done
echo done >> $out

# --- E2 under load: as many clusters as fit (one wave)
out=$R/h200_borrow_e2_full.txt; hdr > $out
for consume in 0 1; do
  for cfg in 2:1:8:16 4:3:4:16 8:7:2:16 16:14:1:16 2:1:6:32 4:3:2:32 8:7:1:32; do
    IFS=: read C h D tile <<< "$cfg"
    ./borrow_tma --tag full --clusters 0 --cluster $C --helpers $h --self 0 --consume $consume --depth $D --tile-kb $tile --reps 5 >> $out 2>&1
  done
  for D in 8 12; do ./borrow_tma --tag full_split --clusters 0 --cluster 1 --consume $consume --depth $D --reps 5 >> $out 2>&1; done
done
for D in 4 8 12; do ./borrow_tma --tag full_fused --fused 1 --clusters 0 --depth $D --reps 5 >> $out 2>&1; done
for D in 3 6; do ./borrow_tma --tag full_fused --fused 1 --clusters 0 --depth $D --tile-kb 32 --reps 5 >> $out 2>&1; done
echo done >> $out

# --- E1: plain loads and TMA (fused single thread), L2-resident vs DRAM
out=$R/h200_borrow_e1.txt; hdr > $out
for b in 1 2 4 8; do
  for w in 0 8; do
    for U in 1 2 4 8; do ./borrow_load --blocks $b --unroll $U --window-mb $w --reps 5 >> $out 2>&1; done
    ./borrow_load --blocks $b --unroll 8 --cg 1 --window-mb $w --reps 5 >> $out 2>&1
    for D in 1 2 4 8 12; do ./borrow_tma --tag e1_tma --fused 1 --clusters $b --depth $D --window-mb $w --reps 5 >> $out 2>&1; done
    for D in 1 2 3 6; do ./borrow_tma --tag e1_tma --fused 1 --clusters $b --depth $D --tile-kb 32 --window-mb $w --reps 5 >> $out 2>&1; done
    ./borrow_tma --tag e1_tma --fused 1 --clusters $b --depth 3 --tile-kb 64 --window-mb $w --reps 5 >> $out 2>&1
  done
done
# D = 16 transfers of 16 KB do not fit one CTA (256 KB > 227 KB); 16 x 8 KB instead
for w in 0 8; do ./borrow_tma --tag e1_tma --fused 1 --clusters 1 --depth 16 --tile-kb 8 --window-mb $w --reps 5 >> $out 2>&1; done
./borrow_tma --tag e1_tma --fused 1 --clusters 1 --depth 16 --tile-kb 16 --reps 1 >> $out 2>&1
echo done >> $out
