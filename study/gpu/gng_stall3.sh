#!/usr/bin/bash
# A stall reasons, one process per configuration (unmasked, all 132 SMs), CUPTI continuous PC sampling.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
C=/storage/pace-apps/spack/packages/linux-rhel9-x86_64_v3/none-none/cuda-12.9.1-leytg2766xv4h3xx3vg4rkxawtyj5qvl
B=/tmp/$USER/gng; tar xzf ~/scratch/gng_cupti_corun.tgz -C $B
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu; out=$R/h200_gng_a_stall3.txt; : > $out
for k in 0 1 2; do
  cd $B && rm -f *pcs$k.dat
  t0=$(( $(date +%s%N) + 30000000000 - k * 2600000000 ))
  CUDA_INJECTION64_PATH=$B/pc_sampling_continuous/libpc_sampling_continuous.so INJECTION_PARAM="--collection-mode 1 --sampling-period 16 --hw-buf-size 1073741824 --file-name pcs$k.dat" \
    LD_LIBRARY_PATH=$C/extras/CUPTI/lib64:$LD_LIBRARY_PATH timeout 150 $B/gng_corun_cupti --map $R/h200_gng_map.txt \
    --schedule $HOME/scratch/gng_stall_sched2.txt --roles lane,lender --t0 $t0 --first-slot $k --last-slot $k >> $out 2>&1
  echo "exit=$? slot=$k" >> $out
  for f in $B/*pcs$k.dat; do LD_LIBRARY_PATH=$C/extras/CUPTI/lib64 timeout 120 $B/pc_sampling_utility/pc_sampling_utility --file-name $f > $out.slot$k.util 2>&1; done
  python3 /storage/project/r-hkim358-0/echung67/vortex-exp/gpu/gng_pcsum.py $out.slot$k.util k_lane k_ew k_fma >> $out
done
