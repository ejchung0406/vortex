#!/usr/bin/bash
# bar_submit.sh <tag> <src> <driver> <scope|-> <test-id>...
# Submits one bar_point.sbatch job per test id; NAME=pr_bds_<tag>_<id>_<driver>.
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
tag=$1; src=$2; drv=$3; scope=$4; shift 4
for id in "$@"; do
  app=; args=; xc=
  case $id in
    barvis)   app=bar_vis;        args="-w4"; xc="-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_ISSUE_WIDTH=2";;
    abar)     app=async_barrier;;
    bslot)    app=bar_slot_phase; xc="-DVX_CFG_NUM_WARPS=2";;
    wsync)    app=wsync;;
    sgemm2)   app=sgemm2;         args="-n64 -t16"; xc="-DVX_CFG_NUM_THREADS=16 -DVX_CFG_NUM_WARPS=16 -DVX_CFG_ISSUE_WIDTH=2";;
    sgemm2t8) app=sgemm2;         args="-n128 -t8"; xc="-DVX_CFG_NUM_THREADS=16 -DVX_CFG_NUM_WARPS=16 -DVX_CFG_ISSUE_WIDTH=2";;
    tcuwg)    app=sgemm_tcu_wg;   xc="-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=2 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=16 -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1";;
    tcusp)    app=sgemm_tcu_wg_sp; args="-k128"; xc="-DVX_CFG_NUM_THREADS=16 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=2 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DVX_CFG_TCU_SPARSE_ENABLE -DWGMMA_NRC=32 -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1";;
    dxa)      app=dxa_copy;       args="-d1"; xc="-DVX_CFG_EXT_DXA_ENABLE";;
    fence)    app=fence;;
    *) echo "unknown $id"; continue;;
  esac
  [ "$scope" != "-" ] && xc="$xc -DVX_CFG_BAR_DRAIN_SCOPE=$scope"
  [ -n "${EXTRA:-}" ] && xc="$xc $EXTRA"
  xc="${xc# }"
  name=pr_bds_${tag}_${id}_${drv}
  rm -rf "$EXP/results/$name"
  SRC=$src NAME=$name APP=$app ARGS="$args" DRIVER=$drv XCONF="$xc" XLEN=${XLEN:-64} \
    sbatch -J $name --export=ALL "$EXP/pr/scripts/bar_point.sbatch" | sed "s/$/ $name/"
done
