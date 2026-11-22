#!/usr/bin/bash
# xvar.sh <tag> "<configs>": build src_wr_x with the variant, sweep warp_rate W=1 at h200s
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wrr
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results/wrr_dev
[ -f /tmp/$USER/wrr/x_$1/build.log ] || $D/build.sh x x_$1 >/dev/null || exit 1
CONFIGS="$2" $D/sweep.sh x_$1 $R/x_$1_w1.txt simx h200s "${3:-0 1 2 3 4 5 6 7 8 9 10 11 12}" 1 32 --perf=4
