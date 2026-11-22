#!/usr/bin/bash
# Local SimX runs for the tests-bwstream branch (queue rules: nice, -j3, /tmp).
#   tests-bwstream_local.sh <xlen> <result-name> [catalog id prefix...]
# Configures a build tree for <xlen> (kept between calls), builds the software,
# builds every regression test, runs make run-simx for both tests and every
# matching catalog case on simx.
set -o pipefail
unset VORTEX_VM_MEGAPAGE_HEAP
XLEN=$1; NAME=$2; shift 2
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=$EXP/src_pr_tests-bwstream
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/$NAME
BUILD=/tmp/$USER/pr_tests-bwstream_x$XLEN
mkdir -p "$OUT" "$BUILD"
source "$TOOLS/env.sh"
cd "$BUILD"
{ echo "name=$NAME"; echo "src=$SRC"; echo "host=$(hostname)"; echo "xlen=$XLEN"; echo "megapage=unset"; echo "local build $BUILD"; } > "$OUT/meta.txt"
nice "$SRC/configure" --xlen="$XLEN" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1 || { echo "rc=configure" >> "$OUT/meta.txt"; exit 1; }
nice make -s -j3 software > "$OUT/build.log" 2>&1 || { echo "rc=build" >> "$OUT/meta.txt"; exit 1; }
fail=0; summ="$OUT/summary.txt"; : > "$summ"
record() { local perf; perf=$(grep -E '^PERF: instrs=' "$3" | tail -1)
  echo "$1 rc=$2 ${perf:-no-perf} $(grep -c 'PASSED!' "$3") passed" >> "$summ"; [ "$2" = 0 ] || fail=1; }
nice make -j3 -C tests/regression all > "$OUT/buildall.log" 2>&1; rc=$?; echo "buildall rc=$rc" >> "$summ"; [ $rc = 0 ] || fail=1
for t in run-simx-bwstream run-simx-bwstream_dxa; do
  nice make -C tests/regression $t > "$OUT/make_$t.log" 2>&1; record "make_$t" $? "$OUT/make_$t.log"
done
if [ $# -gt 0 ]; then
  python3 "$EXP/pr/scripts/tests-bwstream_cases.py" "$SRC" "$XLEN" "$@" > "$OUT/cases.txt"
  while IFS=$'\t' read -r tag envs cmd; do
    [ "${tag##*-}" = simx ] || continue
    [ "$envs" = - ] && envs=
    echo "$tag: $envs $cmd" >> "$OUT/commands.txt"
    env -u CONFIGS $envs nice bash -c "$cmd" > "$OUT/case_$tag.log" 2>&1 < /dev/null
    record "case_$tag" $? "$OUT/case_$tag.log"
  done < "$OUT/cases.txt"
fi
echo "rc=$fail" >> "$OUT/meta.txt"
exit $fail
