#!/usr/bin/bash
# Does the LD_PRELOAD TPC mask work in an MPS client? bwsweep with 8 blocks of
# 1024 threads: on 1 TPC (2 SMs, 2 blocks per SM) it can reach ~220 GB/s; if
# the mask is ignored the 8 blocks spread over 8 SMs (~590 GB/s).
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/tpc_mpsm_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/tpc_mpsm_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
echo "-- without MPS"
TPC_ENABLE_BITS=0 LD_PRELOAD=./tpc_preload.so ./tpc_bwsweep --gib 8 --blocks 8 --threads 1024 --unroll 4 --reps 3 2>&1
./tpc_bwsweep --gib 8 --blocks 8 --threads 1024 --unroll 4 --reps 3 2>&1
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
echo "-- MPS client, mask bit 0"
TPC_ENABLE_BITS=0 LD_PRELOAD=./tpc_preload.so ./tpc_bwsweep --gib 8 --blocks 8 --threads 1024 --unroll 4 --reps 3 2>&1
echo "-- MPS client, mask bits 0-3 (8 SMs, 1 GPC)"
TPC_ENABLE_BITS=0-3 LD_PRELOAD=./tpc_preload.so ./tpc_bwsweep --gib 8 --blocks 16 --threads 1024 --unroll 4 --reps 3 2>&1
echo "-- MPS client, no mask"
./tpc_bwsweep --gib 8 --blocks 8 --threads 1024 --unroll 4 --reps 3 2>&1
echo "-- MPS client, tpc_lab next-mask (SM 0 only, plain loads)"
./tpc_lab run --map ../results_gpu/h200_tpc_map.txt --mem ld --per-sm 2 --mem-sms 0 --verbose 1 2>&1
echo quit | nvidia-cuda-mps-control
