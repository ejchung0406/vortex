#!/usr/bin/bash
# Experiment C, step 3: MPS 80% with NaN hooks (first module whose output goes non-finite)
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
py=$HOME/scratch/venvs/mdl/bin/python
export HF_HOME=$HOME/scratch/hf-cache HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 TRITON_DEFAULT_FP_FUSION=1
out=../results_gpu/h200_gng_c_diag.txt
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/c_mps_pipe CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/c_mps_log
nvidia-cuda-mps-control -d; sleep 2
echo "### step 3: MPS with hooks ($(date))" >> $out
for pct in ${PCTS:-80}; do
CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $py gng_llm_diag.py --tag mps${pct}_hooks --hooks $EXTRA 2>>../results_gpu/h200_gng_c_diag.err | grep "^tag=" >> $out
done
echo quit | nvidia-cuda-mps-control
