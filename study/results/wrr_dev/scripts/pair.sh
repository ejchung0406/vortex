#!/usr/bin/bash
# pair.sh <src> <drv> <tag> "<configs>" "<kinds>" "<warps>" <iters>: small config (8 warps x 8 threads, no profile)
src=$1; drv=$2; tag=$3; cf=$4; kinds=$5; warps=$6; it=$7
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wrr
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results/wrr_dev
b=${src}_${drv}_$tag
[ -f /tmp/$USER/wrr/$b/build.log ] || $D/build.sh $src $b > /dev/null || exit 1
CONFIGS="$cf" $D/sweep.sh $b $R/small_${b}.txt $drv - "$kinds" "$warps" $it --warps=8 --threads=8
