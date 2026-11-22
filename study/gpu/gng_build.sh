#!/usr/bin/bash
# Build the go/no-go binaries into /tmp/$USER/gng (never into gpu/).
# usage: gng_build.sh [target ...]   targets: cutlass corun swapab smcount tpclab cupti (default all)
set -e
CUDA=${CUDA:-/storage/pace-apps/spack/packages/linux-rhel9-x86_64_v3/none-none/cuda-12.9.1-leytg2766xv4h3xx3vg4rkxawtyj5qvl}
CUTLASS=/storage/project/r-hkim358-0/echung67/cutlass
SRC=/storage/project/r-hkim358-0/echung67/vortex-exp/gpu
B=/tmp/$USER/gng; mkdir -p $B
NV="$CUDA/bin/nvcc -std=c++17 -O3 -gencode arch=compute_90a,code=sm_90a -lineinfo"
t=${*:-cutlass corun swapab smcount tpclab cupti}
for x in $t; do case $x in
cutlass) for k in bf16 bf16m1 fp8 fp8m1; do
          $NV --expt-relaxed-constexpr -I$CUTLASS/include -I$CUTLASS/tools/util/include -DGNG_KIND=$k -c $SRC/gng_cutlass_kind.cu \
            -o $B/gng_cutlass_$k.o > $B/gng_cutlass_$k.build.log 2>&1 || { tail -30 $B/gng_cutlass_$k.build.log; exit 1; } &
        done; wait ;;
corun)  $NV -Xptxas -v -c $SRC/gng_corun.cu -o $B/gng_corun.o > $B/gng_corun.build.log 2>&1 || { tail -40 $B/gng_corun.build.log; exit 1; }
        $NV $B/gng_corun.o $B/gng_cutlass_bf16.o $B/gng_cutlass_bf16m1.o $B/gng_cutlass_fp8.o $B/gng_cutlass_fp8m1.o -o $B/gng_corun \
          -lcuda -lcublasLt -ldl -lpthread || exit 1
        grep -A1 "k_lane" $B/gng_corun.build.log | grep -E "registers|spill" | head -4 ;;
swapab) $NV -Xptxas -v $SRC/gng_swapab.cu -o $B/gng_swapab -lcuda -lcublasLt -ldl -lpthread > $B/gng_swapab.build.log 2>&1 \
          || { tail -40 $B/gng_swapab.build.log; exit 1; }
        grep -E "registers|spill" $B/gng_swapab.build.log | head -12 ;;
smcount) $NV -shared -Xcompiler -fPIC $SRC/gng_smcount.cu -o $B/libgng_smcount.so ;;
tpclab) $NV $SRC/tpc_lab.cu -I$SRC -o $B/tpc_lab -lcuda ;;
cupti)  for s in pc_sampling_continuous pc_sampling_utility; do
          rm -rf $B/$s; cp -r $CUDA/extras/CUPTI/samples/$s $CUDA/extras/CUPTI/samples/common $B/ 2>/dev/null || cp -r $CUDA/extras/CUPTI/samples/$s $B/
          (cd $B/$s && make CUDA_INSTALL_PATH=$CUDA > build.log 2>&1 || tail -20 build.log)
        done ;;
esac; echo "built $x"; done
