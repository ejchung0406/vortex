#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=2:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# Build the rtlsim driver library only (Verilator elaboration + C++), for an XLEN
# whose RISC-V toolchain is not installed here.
#   sbatch --export=ALL,SRC=<copy>,NAME=pr_...,XLEN=32,XCONF="-D..." dxa-mem-tag-width_rtlbuild.sbatch
set -eo pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/${NAME:?NAME is required}
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$
mkdir -p "$OUT" "$BUILD"
trap 'rm -rf "$BUILD"' EXIT
source "$TOOLS/env.sh"
cd "$BUILD"
"${SRC:?SRC is required}/configure" --xlen=${XLEN:-32} --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
{ echo "name=$NAME"; echo "src=$SRC"; echo "xlen=${XLEN:-32}"; echo "xconf=${XCONF:-}"; echo "host=$(hostname)"; } > "$OUT/meta.txt"
make -C sim/rtlsim CONFIGS="${XCONF:-} -DPERF_ENABLE" > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
echo "rc=$rc" >> "$OUT/meta.txt"
exit $rc
