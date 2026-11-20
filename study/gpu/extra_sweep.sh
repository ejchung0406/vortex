#!/usr/bin/bash
# GEMV and TMA copy-only sweeps on the GPU of the current allocation.
# usage: extra_sweep.sh <output prefix> [arch]
set -eo pipefail
cd "$(dirname "$0")"
module load cuda/12.9.1
pre=${1:?output prefix}
arch=${2:-sm_120}
h=$(hostname -s)
nvcc -O3 -std=c++17 -arch=$arch -o gemv_$h gemv.cu
nvcc -O3 -std=c++17 -arch=$arch -o tmacopy_$h tmacopy.cu
: > ${pre}_gemv.txt
for unroll in 1 2 4 8; do
  for blocks in 1 2 4 8 16 32 64 94 188; do
    gib=$(python3 -c "print(max(0.5, min(8, $blocks / 8)))")
    ./gemv_$h --gib $gib --blocks $blocks --unroll $unroll >> ${pre}_gemv.txt || echo "FAILED u=$unroll b=$blocks" >> ${pre}_gemv.txt
  done
done
: > ${pre}_tmacopy.txt
for spec in "4 1" "4 2" "4 4" "4 8" "4 16" "8 1" "8 2" "8 4" "8 8" "16 1" "16 2" "16 4" "32 1" "32 2" "64 1"; do
  set -- $spec; tile=$1; depth=$2
  for blocks in 1 4 16 64 188 376 752; do
    gib=$(python3 -c "print(max(0.5, min(8, $blocks / 8)))")
    ./tmacopy_$h --gib $gib --blocks $blocks --tile-kb $tile --depth $depth >> ${pre}_tmacopy.txt \
      || echo "FAILED tile=$tile depth=$depth b=$blocks" >> ${pre}_tmacopy.txt
  done
done
echo done >> ${pre}_tmacopy.txt
