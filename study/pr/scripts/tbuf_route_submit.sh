#!/usr/bin/bash
# Submits the simx-tbuf-route verification runs.
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
BR=$EXP/src_pr_simx-tbuf-route
BASE=$EXP/src_pr_simx-tbuf-route_base
REPRO=$EXP/src_pr_simx-tbuf-route_repro
SYRK="-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=16 -DWGMMA_SS -DWGMMA_RMAJOR_A -DWGMMA_SYRK -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1"
AOFF="-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=8 -DWGMMA_SS -DWGMMA_RMAJOR_A -DA_PAD_K=1 -DA_OFF_K=1 -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1"
RS="-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=8 -DWGMMA_RS"
sub() { # name src app driver xconf mem
  sbatch -J pr_tbuf --mem=$6 --export=ALL,SRC=$2,NAME=pr_simx-tbuf-route_$1,APP=$3,DRIVER=$4,XCONF="$5",PERF=1 $EXP/run_point.sbatch
}
sub syrk_repro_simx    $REPRO sgemm_tcu_wg simx   "$SYRK" 32G
sub syrk_branch_simx   $BR    sgemm_tcu_wg simx   "$SYRK" 32G
sub syrk_repro_rtlsim  $REPRO sgemm_tcu_wg rtlsim "$SYRK" 64G
sub syrk_branch_rtlsim $BR    sgemm_tcu_wg rtlsim "$SYRK" 64G
for side in base branch; do
  src=$BASE; [ $side = branch ] && src=$BR
  sub tcu_$side       $src sgemm_tcu            simx "" 32G
  sub wg_$side        $src sgemm_tcu_wg         simx "" 32G
  sub wgdxa_$side     $src sgemm_tcu_wg_dxa     simx "" 32G
  sub wgsp_$side      $src sgemm_tcu_wg_sp      simx "" 32G
  sub wgspdxa_$side   $src sgemm_tcu_wg_sp_dxa  simx "" 32G
  sub aoff_$side      $src sgemm_tcu_wg         simx "$AOFF" 32G
  sub rs_$side        $src sgemm_tcu_wg         simx "$RS" 32G
  sub flash_$side     $src flash_attn_tcu_wg    simx "" 32G
done
