#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=12:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# Several blackbox runs (or host unit tests) in one build tree of one source
# copy, modelled on run_point.sbatch. CASES is a ';'-separated list of
#   label|app|args|configs|blackbox flags|env
# where env is space-separated VAR=VALUE pairs exported for that case only, and
# app "unit:<dir>" runs `make -C tests/unittest/<dir> run` with CONFIGS.
#
#   sbatch -J pr_x --export=ALL,SRC=<copy>,NAME=pr_dram_..,DRIVER=simx,CASES="..." dram_batch.sbatch
set -eo pipefail
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}

EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?SRC is required}
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
DRIVER=${DRIVER:-simx}

mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"

# The copy's own third_party/Makefile builds the Ramulator extension library;
# jobs sharing a copy take turns.
flock "$SRC/third_party/.build_lock" make -C "$SRC/third_party" ramulator > "$OUT/third_party.log" 2>&1

cd "$BUILD"
"$SRC/configure" --xlen="${XLEN:-64}" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
make -s -j"${SLURM_CPUS_PER_TASK:-8}" software > "$OUT/build.log" 2>&1

{
  echo "name=$NAME"
  echo "src=$SRC rev=$(cat "$SRC/.pr_rev" 2>/dev/null)"
  echo "host=$(hostname)"
  echo "driver=$DRIVER xlen=${XLEN:-64}"
  echo "ext_symbols=$(nm -DC "$SRC/third_party/ramulator_ext/libramulator-vortex.so" | grep -c WindowedDRAMController || true)"
} > "$OUT/meta.txt"

start=$(date +%s)
fails=0
IFS=';' read -ra cases <<< "${CASES:?CASES is required}"
for c in "${cases[@]}"; do
  IFS='|' read -r label app args configs flags envs <<< "$c"
  # shellcheck disable=SC2206
  envv=($envs)
  log="$OUT/run_$label.log"
  find "$BUILD" -name ramulator.stats.log -delete 2>/dev/null || true
  t0=$(date +%s)
  if [[ "$app" == unit:* ]]; then
    d=${app#unit:}
    CONFIGS="$configs" make -C "tests/unittest/$d" clean > "$log" 2>&1 || true
    CONFIGS="$configs" make -C "tests/unittest/$d" run >> "$log" 2>&1 && r=0 || r=$?
  else
    opts=(--driver="$DRIVER" --app="$app" --perf="${PERF:-1}")
    [ -n "$args" ] && opts+=(--args="$args")
    # shellcheck disable=SC2206
    [ -n "$flags" ] && opts+=($flags)
    env "${envv[@]}" CONFIGS="$configs" ./ci/blackbox.sh "${opts[@]}" > "$log" 2>&1 && r=0 || r=$?
  fi
  s=$(find "$BUILD" -name ramulator.stats.log | head -1)
  [ -n "$s" ] && cp "$s" "$OUT/ramulator_$label.stats.log"
  cyc=$(grep -o 'cycles=[0-9]*' "$log" | tail -1 || true)
  ins=$(grep -o 'instrs=[0-9]*' "$log" | tail -1 || true)
  res=$(grep -c '^FAIL' "$log" || true)
  echo "case $label app=$app args=\"$args\" configs=\"$configs\" flags=\"$flags\" env=\"$envs\" rc=$r $cyc $ins unit_fails=$res wall=$(( $(date +%s) - t0 ))" >> "$OUT/meta.txt"
  [ $r -ne 0 ] && fails=$((fails + 1))
done
echo "fails=$fails of ${#cases[@]}" >> "$OUT/meta.txt"
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
echo "rc=$fails" >> "$OUT/meta.txt"
