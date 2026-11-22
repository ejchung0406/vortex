#!/usr/bin/bash
# Experiment B, compact rerun with the fixed kernels (no 64-bit runtime % / in the loops).
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
# (second holder: runs first)

tar xzf ~/scratch/gng_sw.tgz -C /tmp/$USER/gng 2>/dev/null
G=/tmp/$USER/gng/gng_swapab; M=../results_gpu/h200_gng_map.txt
out=../results_gpu/h200_gng_b2.txt; : > $out
T="--trials 5 --secs 1.2"
r() { echo "### $*" >> $out; $G --map $M $T "$@" >> $out 2>&1; }
r --sms 12 --mode rs_gmem --nwg 3 --pf 16
r --sms 12 --mode ld_only --nwg 3 --pf 16
r --sms 12 --mode ss --nwg 2 --stages 10 --stage-kb 16
r --sms 12 --mode rs_smem --nwg 2 --stages 10 --stage-kb 16
r --sms 12 --mode tma_only --nwg 1 --stages 10 --stage-kb 16
r --sms 12 --mode rs_gmem --nwg 2 --pf 16
r --sms 12,13,28,29 --mode rs_gmem --nwg 3 --pf 16
r --sms 12,13,28,29 --mode ss --nwg 2 --stages 10 --stage-kb 16
r --sms 12,13 --mode rs_gmem --nwg 3 --pf 16
grep summary $out
