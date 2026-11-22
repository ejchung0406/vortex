#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=1:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
# Verilator -Wall lint of the tensor DUT top (VX_tensor_top) with the catalog's tensor config.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?}; TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?}; BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
mkdir -p "$OUT" "$BUILD"; trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"; "$SRC/configure" --xlen=32 --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
R=$SRC/hw/rtl
CFG="-DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DVX_CFG_TCU_MX_ENABLE -DVX_CFG_EXT_A_ENABLE -DVX_CFG_NUM_THREADS=16 -DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_CORES=2 -DVX_CFG_SOCKET_SIZE=2 -DVX_CFG_PLATFORM_MEMORY_NUM_BANKS=1 -DVX_CFG_L2_ENABLE -DVX_CFG_L2_SIZE=262144"
INC="-I$BUILD/sw -I$BUILD/hw -I$R -I$R/libs -I$R/interfaces -I$R/core -I$R/mem -I$R/cache -I$R/vm -I$R/fpu -I$R/dxa -I$SRC/hw/unittest/tensor -I$R/tcu -I$R/tcu/tfr -I$R/tcu/dsp -I$R/tcu/dpi"
$TOOLS/verilator/bin/verilator --lint-only --language 1800-2012 -Wall -Wpedantic -Wno-DECLFILENAME -Wno-REDEFMACRO -Wno-GENUNNAMED \
  $BUILD/hw/unittest/verilator.vlt -DSIMULATION -DVX_CFG_XLEN=32 -DVX_CFG_XLEN_32 $CFG ${CONFIGS:-} $INC \
  $R/VX_gpu_pkg.sv $R/tcu/VX_tcu_pkg.sv $R/dxa/VX_dxa_pkg.sv --top-module VX_tensor_top $SRC/hw/unittest/tensor/VX_tensor_top.sv > "$OUT/lint.log" 2>&1
echo "rc=$?" > "$OUT/meta.txt"; echo "src_rev=$(cat $SRC/.pr_rev) configs=${CONFIGS:-}" >> "$OUT/meta.txt"
