# Run inside a configured build dir; OUT is set by the sbatch wrapper.
set -x
run() { tag=$1; shift; timeout 5400 ./ci/blackbox.sh --driver=rtlsim "$@" > $OUT/bb_$tag.log 2>&1; echo "bb $tag rc=$?"; grep -E "PERF: instrs|cycles=|PASSED|FAILED" $OUT/bb_$tag.log | tail -3; }
run demo --app=demo
make -s -C tests/runtime test_host_alias > $OUT/test_build.log 2>&1 || exit 4
for i in 1 2 3; do
  VORTEX_DRIVER=rtlsim LD_LIBRARY_PATH=$PWD/sw/runtime timeout 1800 ./tests/runtime/test_host_alias > $OUT/host_alias_$i.log 2>&1
  echo "host_alias run $i rc=$?"
  tail -3 $OUT/host_alias_$i.log
done
run sgemm16 --app=sgemm --args="-n16"
make -s -C tests/regression/dxa_copy clean > /dev/null 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" make -s -C tests/regression/dxa_copy > $OUT/dxa_build.log 2>&1
CONFIGS="-DVX_CFG_EXT_DXA_ENABLE" run dxa_copy --app=dxa_copy --args="-d1"
run vecadd --app=vecadd
exit 0
