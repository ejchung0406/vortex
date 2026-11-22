# dxa_copy -d1 with DXA on, in a configured SimX build dir; OUT is set.
set -x
make -s -C tests/regression/dxa_copy clean > /dev/null 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" make -s -C tests/regression/dxa_copy > $OUT/dxa_build.log 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" timeout 3600 ./ci/blackbox.sh --driver=simx --app=dxa_copy --args="-d1" > $OUT/bb_dxa_copy.log 2>&1; echo "bb dxa_copy rc=$?"
grep -E "PERF: instrs|PASSED|FAILED" $OUT/bb_dxa_copy.log | tail -3
