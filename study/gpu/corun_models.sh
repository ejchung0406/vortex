#!/usr/bin/bash
# Z-Image-Turbo (DiT forward) and Qwen3.5-9B (decode) on one GPU: alone, under
# an MPS active-thread percentage, and together with complementary shares.
# usage: corun_models.sh <gpu tag, e.g. h200> [stage ...]   stages: solo prof mps corun
# Run inside a GPU job. Results go to ../results_gpu/<tag>_*.txt
cd "$(dirname "$0")"
tag=${1:?gpu tag}; shift
stages=${*:-solo prof mps corun}
out=../results_gpu
py=${PY:-$HOME/scratch/venvs/mdl/bin/python}
export HF_HOME=${HF_HOME:-$HOME/scratch/hf-cache} HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1
dur=${DUR:-30}
tmp=$(mktemp -d /tmp/corun_models.XXXXXX)
# MODEL_A / MODEL_B: local copies of the two snapshots (loading from Lustre takes ten minutes)
dit="$py dit_step.py ${MODEL_A:+--model $MODEL_A}"
llm="$py llm_decode.py ${MODEL_B:+--model $MODEL_B}"

mps_start() {
  export CUDA_MPS_PIPE_DIRECTORY=$tmp/mps_pipe CUDA_MPS_LOG_DIRECTORY=$tmp/mps_log
  mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
  nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
  sleep 2
}
mps_stop() {
  echo quit | nvidia-cuda-mps-control
  unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY
}

for stage in $stages; do
case $stage in
solo)
  for w in ${WORKLOADS:-zimage qwen}; do
    nvidia-smi --query-gpu=timestamp,name,utilization.gpu,utilization.memory,memory.used --format=csv -lms 200 \
      > $out/${tag}_${w}_solo_smi.csv &
    smi=$!
    if [ $w = zimage ]; then
      $dit --mode solo --reps 5 --save-image $out/${tag}_zimage_sample.png > $out/${tag}_zimage_solo.txt 2> $out/${tag}_zimage_solo.err
    else
      $llm --mode solo --reps 5 --batches 1,4,16,64,256 > $out/${tag}_qwen_solo.txt 2> $out/${tag}_qwen_solo.err
    fi
    kill $smi
  done ;;
prof)
  $dit --mode profile > $out/${tag}_zimage_profile.txt 2> $out/${tag}_zimage_profile.err
  $llm --mode profile --new-tokens 32 > $out/${tag}_qwen_profile.txt 2> $out/${tag}_qwen_profile.err ;;
mps)
  mps_start
  : > $out/${tag}_zimage_mps.txt; : > $out/${tag}_qwen_mps.txt
  for pct in ${PCTS:-100 80 70 60 40 20 10 5}; do
    CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $dit --mode window --duration $dur --tag solo_pct$pct \
      >> $out/${tag}_zimage_mps.txt 2>> $out/${tag}_zimage_mps.err
    CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $llm --mode window --duration $dur --tag solo_pct$pct \
      >> $out/${tag}_qwen_mps.txt 2>> $out/${tag}_qwen_mps.err
  done
  mps_stop ;;
corun)
  mps_start
  : > $out/${tag}_models_corun.txt
  for share in ${SHARES:-80:20 60:40 40:60}; do
    ip=${share%:*}; lp=${share#*:}
    d=$tmp/sync_$ip; mkdir -p $d
    CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$ip $dit --mode window --duration $dur --tag corun_img${ip}_llm${lp} \
      --sync-dir $d --role a --segments 3 >> $out/${tag}_models_corun.txt 2>> $out/${tag}_models_corun.err &
    pa=$!
    CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$lp $llm --mode window --duration $dur --batches 1,4,16 --tag corun_img${ip}_llm${lp} \
      --sync-dir $d --role b >> $out/${tag}_models_corun.txt 2>> $out/${tag}_models_corun.err &
    pb=$!
    for k in 0 1 2; do
      until [ -e $d/ready_a_$k ] && [ -e $d/ready_b_$k ]; do
        kill -0 $pa 2>/dev/null && kill -0 $pb 2>/dev/null || { echo "FAILED share=$share segment=$k" >> $out/${tag}_models_corun.txt; kill $pa $pb 2>/dev/null; break 2; }
        sleep 1
      done
      sleep 2; touch $d/go_$k
    done
    wait $pa $pb
  done
  mps_stop ;;
esac
done
rm -rf $tmp
echo "corun_models.sh done: $stages"
