#!/usr/bin/bash
# Block-count sweep of tmasweep on the GPU of the current allocation.
# usage: tma_sweep.sh <output file> [arch]
set -eo pipefail
cd "$(dirname "$0")"
module load cuda/12.9.1
out=${1:?output file}
arch=${2:-sm_120}
bin=./tmasweep_$(hostname -s)
nvcc -O3 -std=c++17 -arch=$arch -o $bin tmasweep.cu
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader > "$out"
for threads in 1024 256; do
  for spec in "4 1" "4 2" "4 4" "4 8" "8 1" "8 2" "8 4" "8 8" "16 1" "16 2" "16 4" "16 6" "32 1" "32 2" "32 3"; do
    set -- $spec; tile=$1; depth=$2
    for blocks in 1 2 4 8 16 32 64 94 188 376; do
      gib=$(python3 -c "print(max(0.25, min(8, $blocks / 16)))")
      $bin --gib $gib --blocks $blocks --threads $threads --tile-kb $tile --depth $depth --reps 5 >> "$out" \
        || echo "FAILED threads=$threads tile_kb=$tile depth=$depth blocks=$blocks" >> "$out"
    done
  done
done
echo done >> "$out"
