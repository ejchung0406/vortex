#!/usr/bin/bash
# apps.sh <build> <out> : the defaults-identity apps and the issuer-bound apps, h200s, 1 core; CONFIGS from env
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
B=/tmp/$USER/wrate/${1:?b}; out=${2:?out}; shift 2
source $TOOLS/env.sh
export VORTEX_VM_MEGAPAGE_HEAP=1
cd $B
: > $out
X="${CONFIGS:-}"
run() {  # name, extra configs, blackbox args...
  local name=$1 cf=$2; shift 2
  CONFIGS="$X $cf -DVX_DBG_STALL_TIMEOUT=20000000" ./ci/blackbox.sh --driver=simx --profile=h200s --cores=1 --perf=4 "$@" > $B/app_last.log 2>&1; local rc=$?
  { echo "$name rc=$rc $(grep -c PASSED $B/app_last.log) $(grep -o 'instrs=[0-9]*, cycles=[0-9]*' $B/app_last.log | tail -1) $(grep -h 'FLOPS_PER_CYCLE\|GB/s\|bytes' $B/app_last.log | tr '\n' ' ' | cut -c1-200)"; } >> $out
  cp $B/app_last.log $B/app_$name.log
}
for a in "$@"; do
case $a in
gemm)   run gemm_block_s0 "-DMTILES=1 -DPROFILE_ENABLE" --app=gemm_block --args="-m16 -n1024 -k4096 -s0" ;;
war)    run war_order "" --app=war_order ;;
roof)   run tmem_roof_n512 "" --app=tmem_roof --args="-m0 -n512 -i256" ;;
roof64) run tmem_roof_n64 "" --app=tmem_roof --args="-m0 -n64 -i256" ;;
roof128) run tmem_roof_n128 "" --app=tmem_roof --args="-m0 -n128 -i256" ;;
bw)     run bwstream_n64_b32 "" --app=bwstream --args="-n64 -b32" ;;
dxa64)  run bwstream_dxa_e64_g1 "" --app=bwstream_dxa --args="-e64 -n256 -g1 -d4 -c0" ;;
dxa4k)  run bwstream_dxa_e4096_g1 "" --app=bwstream_dxa --args="-e4096 -n64 -g1 -d4 -c0" ;;
esac
done
echo done >> $out
