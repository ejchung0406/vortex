#!/usr/bin/bash
# Does an MPS active-thread percentage limit the SMs a client sees? Runs bwsweep
# with every block under several percentages.
cd "$(dirname "$0")"
module load cuda/12.9.1
export CUDA_MPS_PIPE_DIRECTORY=/tmp/$USER/mps_pipe_$$ CUDA_MPS_LOG_DIRECTORY=/tmp/$USER/mps_log_$$
mkdir -p $CUDA_MPS_PIPE_DIRECTORY $CUDA_MPS_LOG_DIRECTORY
nvidia-cuda-mps-control -d || { echo "MPS daemon failed to start"; exit 1; }
sleep 2
bin=./bwsweep_$(hostname -s)
for pct in 100 50 25 10 5; do
  echo "pct=$pct $(CUDA_MPS_ACTIVE_THREAD_PERCENTAGE=$pct $bin --gib 4 --blocks 376 --threads 768 --unroll 4 --reps 3 2>&1 | tail -1)"
done
echo quit | nvidia-cuda-mps-control
tail -5 $CUDA_MPS_LOG_DIRECTORY/control.log 2>/dev/null
