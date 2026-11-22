#!/usr/bin/bash
# Second holder: B rerun with the fixed kernels, A stall reasons, C decode re-measure, DiT DRAM bytes.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
R=../results_gpu; B=/tmp/$USER/gng; mkdir -p $B /tmp/$USER/models
tar xzf ~/scratch/gng_all.tgz -C $B; cp ~/scratch/gng_sw.tgz /tmp/$USER/ 2>/dev/null
{ hostname; date; nvidia-smi --query-gpu=name,driver_version,pci.bus_id --format=csv,noheader; } > $R/h200_gng_env2.txt
timeout 600 $B/tpc_lab probe --out $R/h200_gng_map2.txt > $R/h200_gng_probe2.txt 2>&1
if diff -q <(sort $R/h200_gng_map2.txt) <(sort $R/h200_gng_map.txt) > /dev/null; then echo "map identical to h200_gng_map.txt" >> $R/h200_gng_env2.txt; else echo "MAP DIFFERS" >> $R/h200_gng_env2.txt; cp $R/h200_gng_map2.txt $R/h200_gng_map.txt.second_die; fi
(cp -rL ~/scratch/hf-cache/hub/models--Qwen--Qwen3.5-9B/snapshots/c202236235762e1c871ad0ccb60c8ee5ba337b9a /tmp/$USER/models/qwen; cp -rL ~/scratch/hf-cache/hub/models--Tongyi-MAI--Z-Image-Turbo/snapshots/f332072aa78be7aecdf3ee76d5c247082da564a6 /tmp/$USER/models/zimage) &
cp_pid=$!
echo "B2 $(date)" >> $R/h200_gng_env2.txt; ./gng_run_b2.sh > /dev/null 2>&1
echo "stall $(date)" >> $R/h200_gng_env2.txt; ./gng_stall.sh
wait $cp_pid
echo "C6 $(date)" >> $R/h200_gng_env2.txt; NTPCS="13 26 39 52 66" DUR=5 TMO=240 ./gng_c6.sh
echo "dit dram $(date)" >> $R/h200_gng_env2.txt
C=/storage/pace-apps/spack/packages/linux-rhel9-x86_64_v3/none-none/cuda-12.9.1-leytg2766xv4h3xx3vg4rkxawtyj5qvl
HF_HUB_OFFLINE=1 timeout 900 $C/bin/ncu --profile-from-start off --metrics dram__bytes_read.sum,dram__bytes_write.sum,gpu__time_duration.sum --csv \
  $HOME/scratch/venvs/mdl/bin/python gng_dit_dram.py > $R/h200_gng_dit_dram.csv 2> $R/h200_gng_dit_dram.err
echo "end $(date)" >> $R/h200_gng_env2.txt
