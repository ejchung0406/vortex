#!/usr/bin/bash
# apppar.sh <drv> <tag> "<knob configs>": applications for SimX/rtlsim parity, small configurations
drv=$1; tag=$2; cf=$3
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wrr
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results/wrr_dev
b=b_${drv}_app_$tag
[ -f /tmp/$USER/wrr/$b/build.log ] || $D/build.sh b $b > /dev/null || exit 1
B=/tmp/$USER/wrr/$b; out=$R/app_${drv}_$tag.txt
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B; : > $out
run() {
  local name=$1 xc=$2; shift 2
  CONFIGS="$cf $xc" timeout 7200 ./ci/blackbox.sh --driver=$drv "$@" > $B/app_last.log 2>&1; local rc=$?
  echo "$name rc=$rc passed=$(grep -c PASSED $B/app_last.log) $(grep -o 'instrs=[0-9]*, cycles=[0-9]*' $B/app_last.log | tail -1)" >> $out
  [ $rc -ne 0 ] && cp $B/app_last.log $B/appfail_$name.log
}
S="-DVX_CFG_NUM_THREADS=8 -DVX_CFG_NUM_WARPS=8 -DVX_CFG_ISSUE_WIDTH=4"
T="$S -DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_EXT_DXA_ENABLE -DVX_CFG_TCU_WGMMA_ENABLE -DVX_CFG_TCU_FEDP2K -DVX_CFG_TCU_TMEM_ENABLE -DVX_CFG_TCU_TMEM_B_AHEAD=2 -DVX_CFG_TCU_TMEM_A_BANKROW=1 -DVX_CFG_TCU_TYPE_DPI -DVX_CFG_NUM_ALU_BLOCKS=1 -DVX_CFG_NUM_LSU_BLOCKS=1 -DVX_CFG_NUM_FPU_BLOCKS=1"
run fetch_squash_w1  "$S" --app=fetch_squash --args="-w1 -i16"
run fetch_squash_w4  "$S" --app=fetch_squash --args="-w4 -i16"
run fetch_squash_w8  "$S" --app=fetch_squash --args="-w8 -i16"
run sgemm_n64        "" --app=sgemm --args="-n64"
run dogfood          "" --app=dogfood
run sgemm_s_n32      "$S" --app=sgemm --args="-n32"
run tmem_roof_n64    "$T" --app=tmem_roof --args="-n64 -i64"
run bwdxa_1w         "$S -DVX_CFG_EXT_DXA_ENABLE" --app=bwstream_dxa --args="-e64 -n32 -g1 -d4 -c0"
run bwstream         "$S" --app=bwstream --args="-n16 -b8"
run fetch_squash_vm  "$S -DVX_CFG_VM_ENABLE" --app=fetch_squash --args="-w1 -i16"
run sgemm_vm         "$S -DVX_CFG_VM_ENABLE" --app=sgemm --args="-n32"
echo done >> $out
