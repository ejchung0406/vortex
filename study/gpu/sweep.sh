#!/usr/bin/bash
# Block-count sweep of bwsweep on the GPU of the current allocation.
# usage: sweep.sh <output file>
set -eo pipefail
cd "$(dirname "$0")"
module load cuda/12.9.1
out=${1:?output file}
nvcc -O3 -o bwsweep_$(hostname -s) bwsweep.cu
bin=./bwsweep_$(hostname -s)
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader > "$out"
for threads in 1024 768; do
  for pattern in coalesced rows; do
    for unroll in 1 2 4 8; do
      for blocks in 1 2 4 8 16 32 64 94 188 376 752; do
        # at least 256 MiB, 64 MiB per block up to 8 GiB
        gib=$(python3 -c "print(max(0.25, min(8, $blocks / 16)))")
        $bin --gib $gib --blocks $blocks --threads $threads --pattern $pattern --unroll $unroll --reps 5 >> "$out"
      done
    done
  done
done
echo done >> "$out"
