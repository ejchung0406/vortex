# Blackbox cycle runs in an existing SimX build dir; OUT must be set.
set -x
export MAKEFLAGS=-j3
nice make -s -C tests/regression/demo runtime-simx > $OUT/bb_rtbuild.log 2>&1; echo "runtime-simx build rc=$?"
run() { tag=$1; shift; nice timeout 3600 ./ci/blackbox.sh --driver=simx "$@" > $OUT/bb_$tag.log 2>&1; echo "bb $tag rc=$?"; grep -E "PERF: instrs|PASSED|FAILED" $OUT/bb_$tag.log | tail -3; }
run demo --app=demo
run sgemm16 --app=sgemm --args="-n16"
run vecadd --app=vecadd
run multikernel --app=multikernel
run module_reload --app=module_reload
make -s -C tests/regression/dxa_copy clean > /dev/null 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" nice make -s -C tests/regression/dxa_copy > $OUT/dxa_build.log 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" nice make -s -C tests/regression/dxa_copy runtime-simx > $OUT/dxa_rtbuild.log 2>&1; echo "dxa runtime build rc=$?"
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" run dxa_copy --app=dxa_copy --args="-d1"
exit 0
