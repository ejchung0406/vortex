#!/usr/bin/bash
# All SimX points for one side, serially.
side=$1
S=/storage/project/r-hkim358-0/echung67/vortex-exp/pr/scripts/dxa-dispatch-idle_local_simx.sh
WG="-DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DWGMMA_NRC=16 -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1 -DVX_CFG_NUM_THREADS=32 -DVX_CFG_NUM_WARPS=8"
for n in d 4 8; do
  $S $side copyd1 dxa_copy "-d1" $n
  $S $side copyt4 dxa_copy "-d1 -t0 4" $n
  $S $side sg32 sgemm2_dxa "-n32 -t4 -m1" $n
  $S $side mcast dxa_copy_mcast "" $n
done
for n in d 4 8; do
  $S $side wgdxa sgemm_tcu_wg_dxa "-m 128 -n 128 -k 64" $n "$WG" 1
done
echo "done $side"
