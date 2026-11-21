#!/usr/bin/bash
# Part B step 1, inside the tpcmask_h200 holder: environment, original
# libsmctrl tests, then the 128-bit probe of mask bit -> SM ids.
module load cuda/12.9.1 >/dev/null 2>&1
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
R=../results_gpu
{
  hostname; date
  nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,clocks.max.mem,memory.total,compute_mode,mig.mode.current --format=csv
  nvidia-smi | head -5
  echo "== libsmctrl original tests (third_party/libsmctrl @ c250928)"
  for t in next_mask global_mask stream_mask; do
    echo "-- libsmctrl_test_$t"; timeout 60 ./third_party/libsmctrl/libsmctrl_test_$t > /tmp/$USER.tpc_t.txt 2>&1; rc=$?; tail -6 /tmp/$USER.tpc_t.txt; echo "exit=$rc"
  done
} > $R/h200_tpc_env.txt 2>&1
timeout 600 ./tpc_lab probe --out $R/h200_tpc_map.txt > $R/h200_tpc_probe.txt 2>&1; echo "probe exit=$?" >> $R/h200_tpc_probe.txt
