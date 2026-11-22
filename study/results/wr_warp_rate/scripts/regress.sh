#!/usr/bin/bash
# regress.sh <build> <out> : control-flow regression subset on SimX; CONFIGS from env
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrate/${1:?b}; out=${2:?out}
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B
: > $out
run() {  # name, blackbox args...
  local name=$1; shift
  ./ci/blackbox.sh --driver=simx ${PROF:+--profile=$PROF} "$@" > $B/reg_last.log 2>&1; local rc=$?
  local cyc=$(grep -o "cycles=[0-9]*" $B/reg_last.log | tail -1)
  local ins=$(grep -o "instrs=[0-9]*" $B/reg_last.log | tail -1)
  local pass=$(grep -c "PASSED" $B/reg_last.log)
  echo "$name rc=$rc passed=$pass $ins $cyc" >> $out
}
run diverge            --app=diverge
run diverge_w8t8       --app=diverge --warps=8 --threads=8
run diverge_large      --app=diverge_large
run diverge_loop       --app=diverge_loop
run wsync              --app=wsync
run bar_vis            --app=bar_vis --args=-w4 --warps=16 --threads=8
run bar_slot_phase     --app=bar_slot_phase
run async_barrier      --app=async_barrier
run gbar_phase_c2      --app=gbar_phase --cores=2
run async_gbarrier_c2  --app=async_gbarrier --cores=2
run dogfood            --app=dogfood
run dogfood_bar        --app=dogfood --args="-n1 -tbar"
run dogfood_gbar_c2    --app=dogfood --args="-n1 -tgbar" --cores=2
run demo               --app=demo
run basic              --app=basic
run printf             --app=printf
run fence              --app=fence
run single_cta         --app=single_cta
run cta_cluster        --app=cta_cluster
run occupancy_t32      --app=occupancy --threads=32
run multikernel        --app=multikernel
run sort               --app=sort
run bfs                --app=bfs
run war_order          --app=war_order
run raw_order          --app=raw_order
run vecadd             --app=vecadd
run sgemm              --app=sgemm
run mstress            --app=mstress
echo done >> $out
