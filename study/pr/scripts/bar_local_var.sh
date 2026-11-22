#!/usr/bin/bash
# Local SimX runs of the drain-removal variant (no LSU wait) of bar_vis.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=$EXP/src_pr_bar-drain-scope_var; TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/pr_bar-drain-scope_var; rm -rf $B; mkdir -p $B; cd $B
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
nice $SRC/configure --xlen=64 --tooldir=$TOOLS > configure.log 2>&1 || exit 1
nice make -s -j3 software > build.log 2>&1 || exit 1
run() { name=$1; xc=$2; args=$3; OUT=$EXP/results/$name; rm -rf $OUT; mkdir -p $OUT
  { echo "name=$name"; echo "src_rev=$(cat $SRC/.pr_rev)"; echo "host=$(hostname) (local)"; echo "xconf=$xc"; echo "args=$args"; } > $OUT/meta.txt
  CONFIGS="$xc" MAKEFLAGS=-j3 nice ./ci/blackbox.sh --driver=simx --app=bar_vis --args="$args" > $OUT/run.log 2>&1; echo "rc=$?" >> $OUT/meta.txt; }
V="-DVX_CFG_BAR_DRAIN_SCOPE=1 -DBAR_DRAIN_TEST -DBAR_DRAIN_TEST_3"
run pr_bds_v3_iw4_w4l256s8d4_simx "-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_ISSUE_WIDTH=4 $V" "-w4 -l256 -s8 -d4"
run pr_bds_v3_iw4t16_w4s1d0_simx "-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=16 -DVX_CFG_ISSUE_WIDTH=4 $V" "-w4 -s1 -d0"
echo done
