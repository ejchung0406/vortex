#!/usr/bin/bash
# Experiment B sweep, inside the holder. 1 SM = SM 12 (TPC bit 54, GPC 6);
# 2 SMs = its TPC (12, 13); 4 SMs = two TPCs of GPC 6 (12, 13, 28, 29).
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
tar xzf ~/scratch/gng_sw.tgz -C /tmp/$USER/gng 2>/dev/null
G=/tmp/$USER/gng/gng_swapab; M=../results_gpu/h200_gng_map.txt
out=../results_gpu/h200_gng_b.txt; : > $out
T="--trials 5 --secs 1.2"
r() { echo "### $*" >> $out; $G --map $M $T "$@" >> $out 2>&1; }
for nwg in 1 2 3; do
  r --sms 12 --mode ss --nwg $nwg --stages 10 --stage-kb 16
  r --sms 12 --mode rs_smem --nwg $nwg --stages 10 --stage-kb 16
  for pf in 8 16; do r --sms 12 --mode rs_gmem --nwg $nwg --pf $pf; done
done
r --sms 12 --mode tma_only --nwg 1 --stages 10 --stage-kb 16
r --sms 12 --mode tma_only --nwg 1 --stages 6 --stage-kb 32
r --sms 12 --mode smem_read --nwg 2 --stages 10 --stage-kb 16
for nwg in 2 3; do r --sms 12 --mode ld_only --nwg $nwg --pf 16; done
for s in 12,13 12,13,28,29; do
  r --sms $s --mode ss --nwg 2 --stages 10 --stage-kb 16
  r --sms $s --mode rs_smem --nwg 2 --stages 10 --stage-kb 16
  r --sms $s --mode rs_gmem --nwg 3 --pf 16
  r --sms $s --mode tma_only --nwg 1 --stages 10 --stage-kb 16
  for m in 1 16; do r --sms $s --mode cublas --M $m; done
done
grep summary $out
