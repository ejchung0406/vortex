# Run inside a configured build dir; OUT is set by the sbatch wrapper.
set -x
make -s -C sw/runtime simx > $OUT/rt_simx_build.log 2>&1 || exit 2
make -s -C tests/regression/basic kernel.vxbin > $OUT/basic_build.log 2>&1 || exit 3
make -s -C tests/runtime test_host_alias > $OUT/test_build.log 2>&1 || exit 4
file tests/runtime/test_host_alias
for i in 1 2 3 4 5; do
  VORTEX_DRIVER=simx LD_LIBRARY_PATH=$PWD/sw/runtime timeout 600 ./tests/runtime/test_host_alias > $OUT/host_alias_$i.log 2>&1
  echo "host_alias run $i rc=$?"
  tail -3 $OUT/host_alias_$i.log
done
make -C tests/runtime run-simx VXBIN=$PWD/tests/regression/basic/kernel.vxbin > $OUT/runtime_suite.log 2>&1; echo "runtime suite rc=$?"
grep -E "^\[RUN\]|PASSED|FAILED" $OUT/runtime_suite.log
run() { tag=$1; shift; timeout 3600 ./ci/blackbox.sh --driver=simx "$@" > $OUT/bb_$tag.log 2>&1; echo "bb $tag rc=$?"; grep -E "PERF: instrs|cycles=|PASSED|FAILED" $OUT/bb_$tag.log | tail -3; }
run demo --app=demo
run sgemm16 --app=sgemm --args="-n16"
make -s -C tests/regression/dxa_copy clean > /dev/null 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" make -s -C tests/regression/dxa_copy > $OUT/dxa_build.log 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" run dxa_copy --app=dxa_copy --args="-d1"
run vecadd --app=vecadd
run multikernel --app=multikernel
run module_reload --app=module_reload
VH=$(sed -n 's/^VORTEX_HOME *[:?]*= *//p' config.mk | head -1)
FLAGS=$(sed "s#-I[^ ]*/sim/simx##" sw/runtime/simx_rt_config.stamp)
g++ $FLAGS -I$VH/sim/firesim -fsyntax-only $VH/sw/runtime/firesim/vortex.cpp > $OUT/firesim_syntax.log 2>&1; echo "firesim syntax rc=$?"
exit 0
