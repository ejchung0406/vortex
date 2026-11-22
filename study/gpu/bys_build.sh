#!/usr/bin/bash
# Build the bystander-study binary into /tmp/$USER/bys (never into gpu/).
set -e
CUDA=${CUDA:-/storage/pace-apps/spack/packages/linux-rhel9-x86_64_v3/none-none/cuda-12.9.1-leytg2766xv4h3xx3vg4rkxawtyj5qvl}
SRC=/storage/project/r-hkim358-0/echung67/vortex-exp/gpu
B=/tmp/$USER/bys; mkdir -p $B
$CUDA/bin/nvcc -std=c++17 -O3 -gencode arch=compute_90a,code=sm_90a -lineinfo -Xptxas -v -I$SRC -I$CUDA/include \
  $SRC/bys_lab.cu -o $B/bys_lab -lcuda -L$CUDA/lib64/stubs -lnvidia-ml -lpthread > $B/bys_lab.build.log 2>&1 \
  || { tail -40 $B/bys_lab.build.log; exit 1; }
grep -A2 "k_bys" $B/bys_lab.build.log | grep -E "registers|spill" | head -4
echo "built $B/bys_lab"
