#!/usr/bin/bash
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --time=1:00:00
#SBATCH --account=gts-hkim358
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# XLEN 32 host side only (no 32-bit RISC-V toolchain here, so no kernels):
# build the stub and simx runtimes and run tests/runtime/test_host_alias,
# for the base and the branch copy in turn.
set -o pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
OUT=$EXP/results/pr_runtime-cp-host-tag_simx32_rt
rm -rf "$OUT"; mkdir -p "$OUT"
source "$TOOLS/env.sh"
echo "host=$(hostname) aslr=$(cat /proc/sys/kernel/randomize_va_space)" > "$OUT/meta.txt"
t0=$(date +%s)
for v in base pr; do
  SRC=$EXP/src_pr_runtime-cp-host-tag$([ $v = base ] && echo _base)
  BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$v
  rm -rf $BUILD; mkdir -p $BUILD; cd $BUILD
  "$SRC/configure" --xlen=32 --tooldir="$TOOLS" > "$OUT/${v}_configure.log" 2>&1
  make -s -j4 -C sw/runtime stub simx > "$OUT/${v}_rt_build.log" 2>&1; echo "$v runtime build rc=$?"
  make -s -C tests/runtime test_host_alias > "$OUT/${v}_test_build.log" 2>&1; echo "$v test build rc=$?"
  grep -o -- "-DVX_CFG_MEM_ADDR_WIDTH=[0-9]*\|-DVX_CFG_XLEN=[0-9]*" sw/runtime/simx_rt_config.stamp | sort -u | tr '\n' ' '; echo
  for i in 1 2 3; do
    VORTEX_DRIVER=simx timeout 600 ./tests/runtime/test_host_alias > "$OUT/${v}_host_alias_$i.log" 2>&1
    echo "$v host_alias run $i rc=$? $(grep -E 'device buffer|PASSED|FAILED' $OUT/${v}_host_alias_$i.log | tr '\n' ' ')"
  done
  cd /; rm -rf $BUILD
done > "$OUT/summary.txt" 2>&1
echo "wall_s=$(( $(date +%s) - t0 ))" >> "$OUT/meta.txt"
echo "rc=0" >> "$OUT/meta.txt"
