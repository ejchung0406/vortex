#!/usr/bin/bash
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --time=1:00:00
#SBATCH --account=gts-hkim358
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# One rtlsim build of the branch copy; the base and branch rtlsim runtimes
# (libvortex-rtlsim.so, the only artifact that differs) are both built against
# it, and each case runs once per runtime by LD_LIBRARY_PATH order.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
SRC=$EXP/src_pr_runtime-cp-host-tag
BASE=$EXP/src_pr_runtime-cp-host-tag_base
OUT=$EXP/results/pr_runtime-cp-host-tag_rtlsim
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
export VORTEX_VM_MEGAPAGE_HEAP=1
rm -rf "$OUT"; mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"
{ echo "host=$(hostname)"; echo "aslr=$(cat /proc/sys/kernel/randomize_va_space)"; } > "$OUT/meta.txt"
t0=$(date +%s)
"$SRC/configure" --xlen=64 --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
make -s -j4 software > "$OUT/build.log" 2>&1 || { echo "rc=build" >> "$OUT/meta.txt"; exit 1; }
make -s -j4 -C sw/runtime rtlsim > "$OUT/rt_rtlsim_build.log" 2>&1 || { echo "rc=rtbuild" >> "$OUT/meta.txt"; exit 1; }
echo "build_s=$(( $(date +%s) - t0 ))" >> "$OUT/meta.txt"
RT=$PWD/sw/runtime
B=$PWD/base_rt; mkdir -p $B
ln -s $RT/librtlsim.so $B/librtlsim.so
g++ $(cat $RT/rtlsim_rt_config.stamp) $BASE/sw/runtime/rtlsim/vortex.cpp $SRC/sw/runtime/common/utils.cpp \
    $SRC/sim/common/cmd_processor.cpp -shared -pthread -Wl,-rpath,'$ORIGIN' -L$RT -lrtlsim \
    -o $B/libvortex-rtlsim.so > "$OUT/base_rt_build.log" 2>&1 || { echo "rc=basert" >> "$OUT/meta.txt"; exit 1; }
make -s -C tests/runtime test_host_alias > "$OUT/test_build.log" 2>&1
for a in demo sgemm vecadd; do make -s -C tests/regression/$a > "$OUT/app_$a.log" 2>&1; done
# libvortex.so and the tests carry DT_RPATH to $RT, which wins over
# LD_LIBRARY_PATH, so each variant's runtime is copied into $RT itself.
P=$PWD/pr_rt; mkdir -p $P; cp $RT/libvortex-rtlsim.so $P/
use() { cp $([ $1 = base ] && echo $B || echo $P)/libvortex-rtlsim.so $RT/libvortex-rtlsim.so; }
run() { # run <variant> <tag> <dir> <cmd...>
  local v=$1 tag=$2 dir=$3; shift 3
  ( cd $dir && LD_DEBUG=files LD_DEBUG_OUTPUT=$OUT/lddebug_${v}_${tag} LD_LIBRARY_PATH=$RT VORTEX_DRIVER=rtlsim \
      timeout 900 "$@" ) > "$OUT/${v}_${tag}.log" 2>&1
  local rc=$?
  local lib=$(grep -ho "calling init: [^ ]*libvortex-rtlsim.so" $OUT/lddebug_${v}_${tag}.* 2>/dev/null | head -1 | sed 's/calling init: //')
  rm -f $OUT/lddebug_${v}_${tag}.*
  echo "$v $tag rc=$rc lib=$lib md5=$(md5sum < $RT/libvortex-rtlsim.so | cut -c1-8) $(grep -E 'PERF: instrs' $OUT/${v}_${tag}.log | tail -1) $(grep -E 'PASSED|FAILED' $OUT/${v}_${tag}.log | tail -1)"
}
echo "base md5=$(md5sum < $B/libvortex-rtlsim.so | cut -c1-8) pr md5=$(md5sum < $P/libvortex-rtlsim.so | cut -c1-8)" > "$OUT/summary.txt"
for v in base pr; do
  use $v
  run $v host_alias $PWD/tests/runtime ./test_host_alias
  run $v demo $PWD/tests/regression/demo ./demo -n64
  run $v sgemm16 $PWD/tests/regression/sgemm ./sgemm -n16
  run $v vecadd $PWD/tests/regression/vecadd ./vecadd -n64
done >> "$OUT/summary.txt"
echo "wall_s=$(( $(date +%s) - t0 ))" >> "$OUT/meta.txt"
echo "rc=0" >> "$OUT/meta.txt"
