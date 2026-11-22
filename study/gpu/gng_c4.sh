#!/usr/bin/bash
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
py=$HOME/scratch/venvs/mdl/bin/python
out=../results_gpu/h200_gng_c_gemm.txt
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/c_mps_pipe CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/c_mps_log
nvidia-cuda-mps-control -d; sleep 2
echo "### $(hostname) $(date) torch=$($py -c 'import torch;print(torch.__version__, torch.version.cuda)')" > $out
for pct in 100 80 60 40; do CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $py gng_c_gemm.py >> $out 2>&1; done
echo quit | nvidia-cuda-mps-control
