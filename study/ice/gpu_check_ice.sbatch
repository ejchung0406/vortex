#!/usr/bin/bash
#SBATCH -J vx_gpucheck
#SBATCH -p ice-gpu
#SBATCH -q coc-ice
#SBATCH --gres=gpu:h200:1
#SBATCH -N1 --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=48G
#SBATCH -t 0:20:00
#SBATCH --output=/home/hice1/echung67/scratch/vortex-ice/slurm/gpucheck_%j.out
# One-off check that the Phoenix H200 harness (vortex-exp/gpu: l2s_lab,
# tpc_lab, tpc_preload.so, libsmctrl) works on an ICE H200 node. Binaries are
# built on the login node in ~/scratch/vortex-ice/gpu (cuda/12.9.1, sm_90a).
# Master copy: Phoenix vortex-exp/ice/gpu_check_ice.sbatch.
set -u
G=$HOME/scratch/vortex-ice/gpu
OUT=$HOME/scratch/vortex-ice/results_gpu/gpucheck_$SLURM_JOB_ID
mkdir -p "$OUT"; cd "$G"
module load cuda/12.9.1 > /dev/null 2>&1
s() { echo; echo "##### $*"; }
s env; date; hostname; echo "CUDA_VISIBLE_DEVICES=${CUDA_VISIBLE_DEVICES:-}"; nvcc --version | tail -1
nvidia-smi -L
nvidia-smi --query-gpu=name,pci.bus_id,driver_version,vbios_version,memory.total,compute_mode,mig.mode.current,clocks.max.sm,clocks.max.mem,power.limit --format=csv
nvidia-smi | head -30
s "other processes on our GPU / on the node (exclusivity)"
nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
nvidia-smi --query-gpu=utilization.gpu,utilization.memory,memory.used --format=csv
ls /dev/nvidia* | tr '\n' ' '; echo
squeue -w "$(hostname -s)" -h -o "%i %u %b %C" | awk '{print $1, ($2==ENVIRON["USER"]?"me":"other"), $3, $4}'
s "NVML clocks/power during idle (5 samples)"
nvidia-smi --query-gpu=timestamp,clocks.sm,clocks.mem,power.draw,temperature.gpu,clocks_throttle_reasons.active --format=csv -lms 200 -c 5 2>&1 | head -7
nvidia-smi -lgc 1980 2>&1 | head -2   # clock lock: expected to need root
s "TPC probe (per-launch 128-bit masks, %smid)"
timeout 300 ./tpc_lab probe --out "$OUT/map.txt" 2>&1 | tail -5
wc -l < "$OUT/map.txt"
awk '$1=="bit"{for(i=4;i<=NF;i++)n++} END{print "sms_in_map", n}' "$OUT/map.txt"
if diff -q "$OUT/map.txt" phoenix_ref/h200_l2s_map.txt > /dev/null; then echo "map IDENTICAL to Phoenix h200_l2s_map.txt"; else echo "map DIFFERS from Phoenix"; diff "$OUT/map.txt" phoenix_ref/h200_l2s_map.txt | head -20; fi
s "GPC layout: masked cluster launches (Phoenix tpc_gpcmap.sh hypotheses)"
t() { echo "### $*"; timeout 30 ./tpc_lab cluster --timeout-s 10 "$@" 2>&1 | grep -E "sms:|OK|TIMEOUT|rror"; echo "exit=$?"; }
t --cluster 8  --nclusters 1 --tpcs 0-3
for r in 9-16 18-25 27-34 36-43 45-52 54-61 55-62 63-70 64-71; do t --cluster 16 --nclusters 1 --tpcs $r; done
for b in 75 78 81 83; do t --cluster 4 --nclusters 1 --tpcs 0,$b; done
t --cluster 4 --nclusters 1 --tpcs 9,18
t --cluster 16 --nclusters 8
s "process-wide mask via LD_PRELOAD (tpc_preload.so): 1 TPC vs unmasked"
./tpc_bwsweep --gib 8 --blocks 8 --threads 1024 --unroll 4 --reps 3 2>&1 | tail -1
TPC_ENABLE_BITS=0 LD_PRELOAD=./tpc_preload.so ./tpc_bwsweep --gib 8 --blocks 8 --threads 1024 --unroll 4 --reps 3 2>&1 | tail -2
s "libsmctrl upstream test (64-bit global mask)"
timeout 60 third_party/libsmctrl/libsmctrl_test_gpc_info 2>&1 | tail -4
timeout 60 third_party/libsmctrl/libsmctrl_test_global_mask 2>&1 | tail -2
M=$OUT/map.txt
sms_of() { awk -v b="$1" '$1=="bit"{split(b,a,",");for(k in a)if($2==a[k])for(i=4;i<=NF;i++)printf "%s,",$i}' "$M" | sed 's/,$//'; }
GPC6=$(sms_of 54,55,56,57,58,59,60,61,62)
s "reference 1: one SM, TMA 32 KB x 6 (Phoenix 195.7-195.9 GB/s, l2s_part2)"
for r in 1 2 3; do ./l2s_lab tw --map "$M" --groups 0 --tma 1 --tile-kb 32 --depth 6 --dur-ms 500 2>&1 | grep -oE "check=[a-z]+|total_gbs=[0-9.]+|clock_mhz_avg=[0-9]+" | tr '\n' ' '; echo; done
s "reference 2: one packed GPC (GPC6 bits 54-62: SMs $GPC6), DRAM read (Phoenix 1277-1285 GB/s)"
for mode in "--ld 32 --block 4096" "--tma 1 --tile-kb 32 --depth 6"; do
  for r in 1 2 3; do ./l2s_lab tw --map "$M" --groups "$GPC6" $mode --dur-ms 300 2>&1 | grep -oE "check=[a-z]+|total_gbs=[0-9.]+|clock_mhz_avg=[0-9]+" | tr '\n' ' '; echo " ($mode)"; done
done
s "Phoenix n=10 list verbatim (12,13,28,29,44,45,60,61,74,75: 1282.7 LD / 1284.4 TMA)"
./l2s_lab tw --map "$M" --groups 12,13,28,29,44,45,60,61,74,75 --ld 32 --block 4096 --dur-ms 300 2>&1 | grep -oE "check=[a-z]+|total_gbs=[0-9.]+" | tr '\n' ' '; echo
s "ncu"
which ncu && ncu --version | tail -1
timeout 180 ncu --metrics sm__cycles_elapsed.avg,dram__bytes_read.sum ./l2s_lab tw --map "$M" --groups 0 --tma 1 --tile-kb 32 --depth 6 --dur-ms 50 2>&1 | grep -E "ERR_NVGPUCTRPERM|sm__cycles|dram__bytes|==ERROR==|==PROF==" | head -8
s "MPS as a user"
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$SLURM_JOB_ID CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$SLURM_JOB_ID
mkdir -p "$CUDA_MPS_PIPE_DIRECTORY" "$CUDA_MPS_LOG_DIRECTORY"
if nvidia-cuda-mps-control -d; then
  sleep 2
  for pct in 100 25; do echo "pct=$pct $(CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct timeout 60 ./tpc_bwsweep --gib 4 --blocks 376 --threads 768 --unroll 4 --reps 3 2>&1 | tail -1)"; done
  echo get_server_list | nvidia-cuda-mps-control
  echo quit | nvidia-cuda-mps-control; sleep 1
  tail -3 "$CUDA_MPS_LOG_DIRECTORY/control.log" 2>/dev/null
else echo "MPS daemon failed to start"; fi
rm -rf "$CUDA_MPS_PIPE_DIRECTORY" "$CUDA_MPS_LOG_DIRECTORY"
s "NVML after load"
nvidia-smi --query-gpu=clocks.sm,clocks.mem,power.draw,temperature.gpu --format=csv
nvidia-smi --query-compute-apps=pid,process_name --format=csv
date
