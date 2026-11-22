#!/usr/bin/bash
# usage: submit.sh <tag> <app> "<args>" <ndxa> [extra XCONF] [APPCONF]
E=/storage/project/r-hkim358-0/echung67/vortex-exp
S=$E/pr/scripts/dxa-dispatch-idle_point.sbatch
tag=$1; app=$2; args=$3; ndxa=$4; extra=${5:-}; appconf=${6:-0}
for side in base br; do
  if [ $side = base ]; then src=$E/src_pr_dxa-dispatch-idle_base; else src=$E/src_pr_dxa-dispatch-idle; fi
  for drv in simx rtlsim; do
    name=pr_ddi_${tag}_w${ndxa}_${side}_${drv}
    xc="-DVX_CFG_EXT_DXA_ENABLE${extra:+ $extra}"
    [ "$ndxa" != d ] && xc="$xc -DVX_CFG_NUM_DXA_CORES=$ndxa"
    sbatch -J $name --mem=64G --export=ALL,SRC=$src,NAME=$name,APP=$app,ARGS="$args",XCONF="$xc",DRIVER=$drv,XLEN=64,APPCONF=$appconf $S | sed "s/$/ $name/"
  done
done
