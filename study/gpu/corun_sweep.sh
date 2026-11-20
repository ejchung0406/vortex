#!/usr/bin/bash
# Co-run sweep: streaming blocks against a compute kernel on the remaining SMs.
# usage: corun_sweep.sh <output file> [arch]
set -eo pipefail
cd "$(dirname "$0")"
module load cuda/12.9.1
out=${1:?output file}
arch=${2:-sm_120}
bin=./corun_$(hostname -s)
nvcc -O3 -std=c++17 -arch=$arch -o $bin corun.cu
nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv,noheader > "$out"
sms=$(nvidia-smi --query-gpu=name --format=csv,noheader >/dev/null; $bin --gib 0.25 --bw-blocks 1 --cp-blocks 0 | grep -o 'sms=[0-9]*' | cut -d= -f2)
for unroll in 4 1; do
  for b in 1 2 4 8 16 24 32 48 64 94; do
    gib=$(python3 -c "print(max(1, min(8, $b / 4)))")
    # compute on every other SM, on half of them, and none
    for c in $((sms - b)) $(((sms - b) / 2)) 0; do
      $bin --gib $gib --bw-blocks $b --cp-blocks $c --unroll $unroll --reps 3 >> "$out" \
        || echo "FAILED unroll=$unroll bw=$b cp=$c" >> "$out"
    done
  done
done
echo done >> "$out"
