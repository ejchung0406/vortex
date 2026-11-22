#!/usr/bin/bash
# gemm_perf.sh <build> <out>: gemm_block decode shape with the core pipeline counters (--perf=1); CONFIGS from env
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrate/${1:?b}; out=${2:?out}
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B
CONFIGS="${CONFIGS:-} -DMTILES=1 -DPROFILE_ENABLE -DVX_DBG_STALL_TIMEOUT=20000000" ./ci/blackbox.sh --driver=simx --profile=h200s --cores=1 --perf=1 --app=gemm_block --args="-m16 -n1024 -k4096 -s0" > $B/gemm_perf.log 2>&1
grep "^PERF" $B/gemm_perf.log > $out
