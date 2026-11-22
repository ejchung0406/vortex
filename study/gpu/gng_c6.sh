#!/usr/bin/bash
# Experiment C, step 6: decode with correct output on n SMs set by a TPC mask (no MPS), with
# the profiler pass for kernel time (it hung inside an MPS client with the mask; tried here without MPS).
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
py=$HOME/scratch/venvs/mdl/bin/python
export HF_HOME=$HOME/scratch/hf-cache HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 TRITON_DEFAULT_FP_FUSION=1 GNG_SMCOUNT=/tmp/$USER/gng/libgng_smcount.so
out=../results_gpu/h200_gng_c_qwen_mask.txt
ALL=(0 1 2 3 75 78 81 83 9 10 11 12 13 14 15 16 18 19 20 21 22 23 24 25 27 28 29 30 31 32 33 34 36 37 38 39 40 41 42 43 45 46 47 48 49 50 51 52 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71)
for ntpc in ${NTPCS:-13 26 52 66}; do
  bits=$(IFS=,; echo "${ALL[*]:0:$ntpc}")
  echo "### no MPS, mask ntpc=$ntpc packed bits=$bits $(date) ${EXTRA}" >> $out
  TPC_ENABLE_BITS=$bits LD_PRELOAD=./tpc_preload.so timeout ${TMO:-300} $py llm_decode.py --model /tmp/$USER/models/qwen --mode window --duration ${DUR:-6} --batches 1,16 --tag nomps_mask_tpc$ntpc $EXTRA >> $out 2>>../results_gpu/h200_gng_c_qwen_mask.err
  echo "exit=$?" >> $out
done
