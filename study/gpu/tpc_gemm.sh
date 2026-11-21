#!/usr/bin/bash
# Disjoint-SM co-run with a real tensor-core GEMM (CUTLASS 54 Hopper FP8 warp-
# specialized, 2048x2048x1024, 447 TFLOP/s on 132 SMs) in one MPS client and the
# MEM stream in another; TPC masks (LD_PRELOAD for the GEMM, next-mask in
# tpc_lab) keep them on disjoint TPCs. MEM = 32 SMs, 2 TPCs in each physical GPC.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
M=../results_gpu/h200_tpc_map.txt
MEMBITS="0 75 9 10 18 19 27 28 36 37 45 46 54 55 63 64"
read MEMSMS CMPBITS < <(python3 - "$MEMBITS" <<'PY'
import sys
m = {}
for l in open('../results_gpu/h200_tpc_map.txt'):
    f = l.split(); m[int(f[1])] = f[3:]
mb = [int(x) for x in sys.argv[1].split()]
print(';'.join(s for b in mb for s in m[b]), ','.join(str(b) for b in sorted(m) if b not in mb))
PY
)
echo "MEM SMs: $MEMSMS"; echo "CMP bits: $CMPBITS"
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/tpc_gemm_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/tpc_gemm_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
G="./hopper_bin/hopper_fp8_gemm --m=2048 --n=2048 --k=1024"
mem() { ./tpc_lab run --map $M --mem tma --fused 1 --tile-kb 32 --depth 6 --spin 100 --roles "$MEMSMS" --gib 16 "$@"; }
meml() { ./tpc_lab run --map $M --mem ld --per-sm 2 --mem-sms "$(echo $MEMSMS | tr ';' ,)" --gib 16 "$@"; }
echo "### GEMM solo, all 132 SMs";        $G --iterations=100000 2>&1 | grep -E 'Avg|GFLOPS'
echo "### GEMM solo, CMP TPCs (100 SMs)"; TPC_ENABLE_BITS=$CMPBITS LD_PRELOAD=./tpc_preload.so $G --iterations=100000 2>&1 | grep -E 'Avg|GFLOPS|tpc_preload'
echo "### MEM W solo";                    mem --reps 50 --tag W_solo
echo "### MEM L16 solo";                  meml --reps 50 --tag L16_solo
echo "### MEM W while GEMM runs (GEMM started 3 s earlier)"
TPC_ENABLE_BITS=$CMPBITS LD_PRELOAD=./tpc_preload.so $G --iterations=400000 > /tmp/$USER.tpc_gemmA.txt 2>&1 &
gp=$!; sleep 3; mem --reps 50 --tag W_with_gemm; meml --reps 50 --tag L16_with_gemm; wait $gp; grep -E 'Avg|GFLOPS' /tmp/$USER.tpc_gemmA.txt
echo "### GEMM while MEM W streams (MEM started 3 s earlier, 3000 reps)"
mem --reps 3000 --tag W_bg > /tmp/$USER.tpc_memB.txt 2>&1 &
mp=$!; sleep 3; TPC_ENABLE_BITS=$CMPBITS LD_PRELOAD=./tpc_preload.so $G --iterations=100000 2>&1 | grep -E 'Avg|GFLOPS'
kill -0 $mp 2>/dev/null && echo "MEM still running after the GEMM (good)"; wait $mp; cat /tmp/$USER.tpc_memB.txt
echo quit | nvidia-cuda-mps-control
