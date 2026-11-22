#!/usr/bin/bash
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --time=0:45:00
#SBATCH --account=gts-hkim358
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# The evidence runs the mass cancel dropped, in one job:
#   XLEN-32 rtlsim driver build (no riscv32 toolchain here, so build only) at
#   8 workers x 32 slots, upstream and branch; hw/unittest/dxa_core at 1 x 256,
#   upstream and branch. Each case writes results/pr_dxa-mem-tag-width_<case>/.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
source "$TOOLS/env.sh"
J=${SLURM_CPUS_PER_TASK:-4}

run_case() { # name src xlen target configs
  local name=$1 src=$2 xlen=$3 target=$4 conf=$5
  local out=$EXP/results/pr_dxa-mem-tag-width_$name build=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$name
  mkdir -p "$out" "$build"
  { echo "name=$name"; echo "src=$src"; echo "xlen=$xlen"; echo "target=$target"; echo "xconf=$conf"; echo "host=$(hostname)"; } > "$out/meta.txt"
  local start=$(date +%s) rc
  ( cd "$build" && "$src/configure" --xlen=$xlen --tooldir="$TOOLS" > "$out/configure.log" 2>&1 &&
    make -j$J -C $target CONFIGS="$conf" > "$out/run.log" 2>&1 ) && rc=0 || rc=$?
  echo "rc=$rc" >> "$out/meta.txt"; echo "wall_s=$(( $(date +%s) - start ))" >> "$out/meta.txt"
  rm -rf "$build"
}

BR=$EXP/src_pr_dxa-mem-tag-width; BA=$EXP/src_pr_dxa-mem-tag-width_base
W8="-DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_NUM_DXA_CORES=8 -DVX_CFG_DXA_MAX_INFLIGHT=32 -DPERF_ENABLE"
W1="-DVX_CFG_NUM_DXA_CORES=1 -DVX_CFG_DXA_MAX_INFLIGHT=256"
run_case base_x32_rtlbuild_w8_i32 $BA 32 sim/rtlsim "$W8"
run_case br_x32_rtlbuild_w8_i32   $BR 32 sim/rtlsim "$W8"
run_case base_ut_w1_i256 $BA 64 "hw/unittest/dxa_core run" "$W1"
run_case br_ut_w1_i256   $BR 64 "hw/unittest/dxa_core run" "$W1"
