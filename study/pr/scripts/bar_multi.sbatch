#!/usr/bin/bash
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
# Several bar_vis rtlsim cases on one build tree. CASES="name|configs|args;..."
set -o pipefail
export VORTEX_VM_MEGAPAGE_HEAP=1
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SRC=${SRC:?}; TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
BUILD=/tmp/$USER/vx_${SLURM_JOB_ID:-local}_$$; mkdir -p $BUILD; trap 'rm -rf "$BUILD"' EXIT
source $TOOLS/env.sh; cd $BUILD
$SRC/configure --xlen=64 --tooldir=$TOOLS > configure.log 2>&1 && make -s -j${SLURM_CPUS_PER_TASK:-4} software > build.log 2>&1 || exit 1
IFS=';' read -ra cs <<< "$CASES"
for c in "${cs[@]}"; do
  IFS='|' read name xc args <<< "$c"; OUT=$EXP/results/$name; rm -rf $OUT; mkdir -p $OUT
  { echo "name=$name"; echo "src_rev=$(cat $SRC/.pr_rev)"; echo "host=$(hostname)"; echo "xconf=$xc"; echo "args=$args"; echo "driver=${DRIVER:-rtlsim}"; } > $OUT/meta.txt
  s=$(date +%s); CONFIGS="$xc" ./ci/blackbox.sh --driver=${DRIVER:-rtlsim} --app=${APP:-bar_vis} --args="$args" > $OUT/run.log 2>&1; echo "rc=$?" >> $OUT/meta.txt
  echo "wall_s=$(( $(date +%s) - s ))" >> $OUT/meta.txt
done
