#!/usr/bin/bash
# Experiment A stall reasons: CUPTI continuous PC sampling (CUDA toolkit sample injection
# library) on a few same-process co-run slots; per-kernel stall-reason totals.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
C=/storage/pace-apps/spack/packages/linux-rhel9-x86_64_v3/none-none/cuda-12.9.1-leytg2766xv4h3xx3vg4rkxawtyj5qvl
B=/tmp/$USER/gng; tar xzf ~/scratch/gng_cupti.tgz -C $B
out=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_gng_a_stall.txt
cd $B && rm -f *pcs.dat
t0=$(( $(date +%s%N) + 30000000000 ))
CUDA_INJECTION64_PATH=$B/pc_sampling_continuous/libpc_sampling_continuous.so INJECTION_PARAM="--collection-mode 1 --sampling-period 12 --file-name pcs.dat" \
  LD_LIBRARY_PATH=$C/extras/CUPTI/lib64:$LD_LIBRARY_PATH timeout 150 $B/gng_corun --map /storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_gng_map.txt \
  --schedule ~/scratch/gng_stall_sched.txt --roles lane,lender --t0 $t0 > $out 2>&1
echo "exit=$?" >> $out
ls -la $B/*pcs.dat >> $out 2>&1
for f in $B/*pcs.dat; do LD_LIBRARY_PATH=$C/extras/CUPTI/lib64 timeout 120 $B/pc_sampling_utility/pc_sampling_utility --file-name $f > $out.util 2>&1; done
