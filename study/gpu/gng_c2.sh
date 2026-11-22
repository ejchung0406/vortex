#!/usr/bin/bash
# Experiment C, step 2: no MPS, SM count faked in torch/Triton/FLA; NaN hooks; poisoned allocator.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
py=$HOME/scratch/venvs/mdl/bin/python
export HF_HOME=$HOME/scratch/hf-cache HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 TRITON_DEFAULT_FP_FUSION=1
out=../results_gpu/h200_gng_c_diag.txt
run() { $py gng_llm_diag.py "$@" 2>>../results_gpu/h200_gng_c_diag.err | grep "^tag=" >> $out; }
echo "### step 2: no MPS, faked SM counts, hooks, poison ($(date))" >> $out
run --tag fake104_hooks --fake-sms 104 --hooks
run --tag fake78 --fake-sms 78
run --tag nomps_poison --poison
