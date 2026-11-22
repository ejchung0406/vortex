#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=12:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# Runs for the tests-bwstream PR branch, in a node-local build tree.
#
#   SRC     source copy                 NAME   result dir under results/
#   XLEN    32 or 64 (default 64)
#   CASES   catalog id prefixes: run each matching case as ci/test_runner.py would
#   MAKERUN tests/regression make targets to run (e.g. "run-simx-bwstream")
#   BUILDALL=1  make -C tests/regression all first
#   BB      raw blackbox.sh arguments for one extra run (with BBCONF as CONFIGS)
#
# VORTEX_VM_MEGAPAGE_HEAP is unset: the tests must pass without it.
set -o pipefail
unset VORTEX_VM_MEGAPAGE_HEAP

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?SRC is required}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
XLEN=${XLEN:-64}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$

mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"

cd "$BUILD"
{
  echo "name=$NAME"
  echo "src=$SRC"
  echo "host=$(hostname)"
  echo "xlen=$XLEN"
  echo "megapage=${VORTEX_VM_MEGAPAGE_HEAP:-unset}"
} > "$OUT/meta.txt"
"$SRC/configure" --xlen="$XLEN" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1 || { echo "rc=configure" >> "$OUT/meta.txt"; exit 1; }
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$OUT/build.log" 2>&1 || { echo "rc=build" >> "$OUT/meta.txt"; exit 1; }

fail=0
summ="$OUT/summary.txt"
: > "$summ"
record() { # tag rc log
  local perf
  perf=$(grep -E '^PERF: instrs=' "$3" | tail -1)
  echo "$1 rc=$2 ${perf:-no-perf} $(grep -c 'PASSED!' "$3") passed" >> "$summ"
  [ "$2" = 0 ] || fail=1
}

if [ "${BUILDALL:-0}" = 1 ]; then
  make -j"${SLURM_CPUS_PER_TASK:-8}" -C tests/regression all > "$OUT/buildall.log" 2>&1
  rc=$?; echo "buildall rc=$rc" >> "$summ"; [ $rc = 0 ] || fail=1
fi

for t in ${MAKERUN:-}; do
  make -C tests/regression "$t" > "$OUT/make_$t.log" 2>&1
  record "make_$t" $? "$OUT/make_$t.log"
done

if [ -n "${CASES:-}" ]; then
  python3 "$EXP/pr/scripts/tests-bwstream_cases.py" "$SRC" "$XLEN" $CASES > "$OUT/cases.txt"
  while IFS=$'\t' read -r tag envs cmd; do
    [ -n "${DRIVERS:-}" ] && ! [[ " $DRIVERS " == *" ${tag##*-} "* ]] && continue
    echo "$tag: $envs $cmd" >> "$OUT/commands.txt"
    [ "$envs" = - ] && envs=
    env -u CONFIGS $envs bash -c "$cmd" > "$OUT/case_$tag.log" 2>&1 < /dev/null
    record "case_$tag" $? "$OUT/case_$tag.log"
  done < "$OUT/cases.txt"
fi

if [ -n "${BB:-}" ]; then
  echo "bb: CONFIGS=${BBCONF:-} ./ci/blackbox.sh $BB" >> "$OUT/commands.txt"
  CONFIGS="${BBCONF:-}" bash -c "./ci/blackbox.sh $BB" > "$OUT/bb.log" 2>&1 < /dev/null
  record bb $? "$OUT/bb.log"
fi

echo "rc=$fail" >> "$OUT/meta.txt"
exit $fail
