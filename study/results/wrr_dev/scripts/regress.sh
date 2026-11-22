#!/usr/bin/bash
# regress.sh <src> <tag> <drv> "<configs>" [set]: control-flow regression subset; small sizes on rtlsim
src=$1; tag=$2; drv=$3; cf=$4; set=${5:-core}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wrr
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results/wrr_dev
b=${src}_${drv}_$tag
[ -f /tmp/$USER/wrr/$b/build.log ] || $D/build.sh $src $b > /dev/null || exit 1
B=/tmp/$USER/wrr/$b; out=$R/reg_${b}_$set.txt
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B; : > $out
run() {  # name, extra configs, blackbox args...
  local name=$1 xc=$2; shift 2
  CONFIGS="$cf $xc" timeout ${TMO:-3600} ./ci/blackbox.sh --driver=$drv "$@" > $B/reg_last.log 2>&1; local rc=$?
  echo "$name rc=$rc passed=$(grep -c PASSED $B/reg_last.log) $(grep -o 'instrs=[0-9]*, cycles=[0-9]*' $B/reg_last.log | tail -1)" >> $out
  [ $rc -ne 0 ] && cp $B/reg_last.log $B/regfail_$name.log
}
case $set in
core)
run fetch_squash_w1    "" --app=fetch_squash --args="-w1 -i8"
run fetch_squash_w4    "" --app=fetch_squash --args="-w4 -i8"
run diverge            "" --app=diverge
run diverge_w8t8       "" --app=diverge --warps=8 --threads=8
run diverge_large      "" --app=diverge_large
run diverge_loop       "" --app=diverge_loop
run wsync              "" --app=wsync
run bar_vis            "" --app=bar_vis --args=-w4 --warps=16 --threads=8
run bar_vis_scope1     "-DVX_CFG_BAR_DRAIN_SCOPE=1" --app=bar_vis --args=-w4 --warps=16 --threads=8
run bar_slot_phase     "" --app=bar_slot_phase
run async_barrier      "" --app=async_barrier
run gbar_phase_c2      "" --app=gbar_phase --cores=2
run async_gbarrier_c2  "" --app=async_gbarrier --cores=2
run dogfood            "" --app=dogfood
run dogfood_bar        "" --app=dogfood --args="-n1 -tbar"
run dogfood_gbar_c2    "" --app=dogfood --args="-n1 -tgbar" --cores=2
run demo               "" --app=demo
run basic              "" --app=basic
run printf             "" --app=printf
run fence              "" --app=fence
run single_cta         "" --app=single_cta
run cta_cluster        "" --app=cta_cluster
run occupancy_t32      "" --app=occupancy --threads=32
run multikernel        "" --app=multikernel
run sort               "" --app=sort
run bfs                "" --app=bfs
run war_order          "" --app=war_order
run raw_order          "" --app=raw_order
run vecadd             "" --app=vecadd
run sgemm              "" --app=sgemm
run mstress            "" --app=mstress
run arith              "" --app=arith
run mixfu              "" --app=mixfu
run lmem_access        "" --app=lmem_access
;;
vm)
run vm_test            "-DVX_CFG_VM_ENABLE" --app=vm_test
run vm_fault           "-DVX_CFG_VM_ENABLE" --app=vm_fault
run vm_stress          "-DVX_CFG_VM_ENABLE" --app=vm_stress
run fetch_squash_vm    "-DVX_CFG_VM_ENABLE" --app=fetch_squash --args="-w1 -i8"
run diverge_vm         "-DVX_CFG_VM_ENABLE" --app=diverge
run dogfood_vm         "-DVX_CFG_VM_ENABLE" --app=dogfood
run sgemm_vm           "-DVX_CFG_VM_ENABLE" --app=sgemm
;;
tcu)
run sgemm_tcu          "-DVX_CFG_EXT_TCU_ENABLE" --app=sgemm_tcu
run sgemm_tcu_sp       "-DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_SPARSE_ENABLE" --app=sgemm_tcu_sp
run sgemm_tcu_mx       "-DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_TCU_MX_ENABLE" --app=sgemm_tcu_mx
run sgemm2_dxa         "-DVX_CFG_EXT_DXA_ENABLE" --app=sgemm2_dxa
run dxa_copy           "-DVX_CFG_EXT_DXA_ENABLE" --app=dxa_copy
run sgemm_tcu_wg_dxa   "-DVX_CFG_EXT_TCU_ENABLE -DVX_CFG_EXT_DXA_ENABLE" --app=sgemm_tcu_wg_dxa
;;
esac
echo done >> $out
