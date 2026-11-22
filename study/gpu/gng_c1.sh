#!/usr/bin/bash
# Experiment C, step 1: reproduce the '!!!!' output under MPS percentages and
# read back the SM count each client really gets; then the same without MPS
# with a faked SM count, and with a NaN-poisoned allocator.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
py=$HOME/scratch/venvs/mdl/bin/python
export HF_HOME=$HOME/scratch/hf-cache HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 TRITON_DEFAULT_FP_FUSION=1
out=../results_gpu/h200_gng_c_diag.txt
run() { $py gng_llm_diag.py "$@" 2>>../results_gpu/h200_gng_c_diag.err | grep "^tag=" >> $out; }
echo "### $(hostname) $(date) no MPS" >> $out
run --tag nomps
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/c_mps_pipe CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/c_mps_log
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed" >> $out; exit 1; }
sleep 2
echo "### MPS percentages" >> $out
for pct in ${PCTS:-100 80 80 70 60 40 20 10}; do
  CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct run --tag mps$pct
done
echo quit | nvidia-cuda-mps-control
