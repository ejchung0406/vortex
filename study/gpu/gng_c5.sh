#!/usr/bin/bash
# Experiment C, step 5: Qwen3.5-9B decode re-measured with correct output. The SM
# allotment is set with a TPC mask (LD_PRELOAD tpc_preload.so) inside an MPS client
# without an active-thread percentage (the percentage is what breaks cuBLAS);
# TPCs packed GPC by GPC as MPS does. SM count read back in every run.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
py=$HOME/scratch/venvs/mdl/bin/python
export HF_HOME=$HOME/scratch/hf-cache HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 TRITON_DEFAULT_FP_FUSION=1 GNG_SMCOUNT=/tmp/$USER/gng/libgng_smcount.so
out=../results_gpu/h200_gng_c_qwen_mask.txt; : > $out
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/c_mps_pipe CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/c_mps_log
nvidia-cuda-mps-control -d; sleep 2
ALL=(0 1 2 3 75 78 81 83 9 10 11 12 13 14 15 16 18 19 20 21 22 23 24 25 27 28 29 30 31 32 33 34 36 37 38 39 40 41 42 43 45 46 47 48 49 50 51 52 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71)
for ntpc in ${NTPCS:-13 26 39 52 66}; do
  bits=$(IFS=,; echo "${ALL[*]:0:$ntpc}")
  echo "### ntpc=$ntpc bits=$bits $(date)" >> $out
  TPC_ENABLE_BITS=$bits LD_PRELOAD=./tpc_preload.so $py llm_decode.py --model /tmp/$USER/models/qwen --mode window --duration ${DUR:-10} --batches 1,16 --tag mask_tpc$ntpc >> $out 2>>../results_gpu/h200_gng_c_qwen_mask.err
done
echo "### MPS percentage 80 for comparison (expected wrong output)" >> $out
CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=80 $py llm_decode.py --model /tmp/$USER/models/qwen --mode window --duration ${DUR:-10} --batches 1 --tag mps80 >> $out 2>>../results_gpu/h200_gng_c_qwen_mask.err
echo quit | nvidia-cuda-mps-control
