#!/usr/bin/bash
# bar_var_submit.sh <variant 0..3> <driver> <cfg-id> <args...>
# variant 0: branch scope 1 as is; 1: no store-landed; 2: own-warp commits only; 3: no wait
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
v=$1; drv=$2; cid=$3; shift 3; args="$*"
case $cid in
  iw2) xc="-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_ISSUE_WIDTH=2";;
  iw4) xc="-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=8 -DVX_CFG_ISSUE_WIDTH=4";;
  iw4t16) xc="-DVX_CFG_NUM_WARPS=16 -DVX_CFG_NUM_THREADS=16 -DVX_CFG_ISSUE_WIDTH=4";;
  *) echo bad cfg; exit 1;;
esac
xc="$xc -DVX_CFG_BAR_DRAIN_SCOPE=1"
[ "$v" != 0 ] && xc="$xc -DBAR_DRAIN_TEST -DBAR_DRAIN_TEST_$v"
tag=$(echo "$args" | tr -d ' -' )
name=pr_bds_v${v}_${cid}_${tag:-def}_${drv}
rm -rf "$EXP/results/$name"
SRC=$EXP/src_pr_bar-drain-scope_var NAME=$name APP=bar_vis ARGS="$args" DRIVER=$drv XCONF="$xc" XLEN=${XLEN:-64} \
  sbatch -J $name --export=ALL "$EXP/pr/scripts/bar_point.sbatch" | sed "s/$/ $name/"
