#!/usr/bin/bash
# Inside GPC6 (mask bits 54-62): bandwidth vs number of TPCs (bit order) and
# for 3-TPC subsets, DRAM-streaming and L2-resident (24 MB window), TMA fused
# write-only 32 KB D=6 per SM. Also GPC1 (bits 9-16) for a second GPC.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
M=../results_gpu/h200_tpc_map.txt
sms() { python3 -c "
import sys
m={}
for l in open('$M'):
  f=l.split(); m[int(f[1])]=f[3:]
print(';'.join(s for b in sys.argv[1].split(',') for s in m[int(b)]))" $1; }
run() { r=$(sms $1); echo "# GPCSCAN bits=$1 $2"; ./tpc_lab run --map $M --mem tma --fused 1 --tile-kb 32 --depth 6 --roles "$r" --reps 3 $3 --tag "bits$1"; }
for g in "54 55 56 57 58 59 60 61 62" "9 10 11 12 13 14 15 16"; do
  acc=""
  for b in $g; do acc=${acc:+$acc,}$b; run $acc dram ""; run $acc l2 "--window-mb 24 --mb-per-consumer 2048"; done
done
for s in 54,55,56 57,58,59 60,61,62 54,57,60 55,58,61 56,59,62 54,58,62; do run $s dram ""; run $s l2 "--window-mb 24 --mb-per-consumer 2048"; done
