#!/usr/bin/bash
#SBATCH --job-name=gng_h200
#SBATCH --gres=gpu:h200:1
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=1:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH -q inferno
#SBATCH -p gpu-h200
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_gng_holder2.out
# Holder for the go/no-go experiments (A: SM sharing, B: smem bypass, C: LLM
# output under MPS); work runs inside it with srun --jobid --overlap.
hostname; nvidia-smi -L; nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,memory.total --format=csv,noheader; date
srun sleep 1000000000
