# XLEN 32: the runtime suite (CI case runtime-1) and two smoke tests on simx.
set -x
make -s -C sw/runtime simx > $OUT/rt_simx_build.log 2>&1 || exit 2
make -s -C tests/regression/basic kernel.vxbin > $OUT/basic_build.log 2>&1 || exit 3
make -C tests/runtime run-simx VXBIN=$PWD/tests/regression/basic/kernel.vxbin > $OUT/runtime_suite.log 2>&1; echo "runtime suite rc=$?"
grep -E "^\[RUN\]|PASSED|FAILED|device buffer" $OUT/runtime_suite.log
run() { tag=$1; shift; timeout 3600 ./ci/blackbox.sh --driver=simx "$@" > $OUT/bb_$tag.log 2>&1; echo "bb $tag rc=$?"; grep -E "PERF: instrs|cycles=|PASSED|FAILED" $OUT/bb_$tag.log | tail -3; }
run demo --app=demo
run sgemm16 --app=sgemm --args="-n16"
exit 0
