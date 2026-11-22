#!/usr/bin/bash
# Runs inside the holder: srun --jobid=<id> --overlap warprate_run.sh <tag> [warprate args]
# (the binary is built on the login node: nvcc -std=c++17 -O3 -arch=sm_90a warprate.cu -o warprate -lcuda)
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
out=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_warprate_${1:-run}.txt
shift
hostname > $out
date >> $out
nvidia-smi --query-gpu=name,clocks.sm,clocks.max.sm,power.draw --format=csv >> $out 2>&1
./warprate "$@" >> $out 2>&1
date >> $out
nvidia-smi --query-gpu=clocks.sm --format=csv >> $out 2>&1
