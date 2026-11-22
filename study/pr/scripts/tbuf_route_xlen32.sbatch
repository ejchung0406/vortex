#!/usr/bin/bash
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
# sgemm_tcu_wg-syrk at XLEN 32 on the simx-tbuf-route branch: simx and rtlsim, one build.
set -eo pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=$EXP/src_pr_simx-tbuf-route
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
mkdir -p "$BUILD"; trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"
"$SRC/configure" --xlen=32 --tooldir="$TOOLS" > /dev/null 2>&1
make -s -j"${SLURM_CPUS_PER_TASK:-4}" software > /dev/null 2>&1
SYRK="-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4 -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DITYPE=fp16 -DOTYPE=fp32 -DWGMMA_NRC=16 -DWGMMA_SS -DWGMMA_RMAJOR_A -DWGMMA_SYRK -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1"
for drv in simx rtlsim; do
  OUT=$EXP/results/pr_simx-tbuf-route_syrk_x32_$drv; mkdir -p $OUT
  echo "xconf=$SYRK xlen=32 driver=$drv" > $OUT/meta.txt
  CONFIGS="$SYRK" ./ci/blackbox.sh --driver=$drv --app=sgemm_tcu_wg --perf=1 > $OUT/run.log 2>&1 && rc=0 || rc=$?
  echo "rc=$rc" >> $OUT/meta.txt
done
