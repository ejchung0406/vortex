#!/usr/bin/bash
#SBATCH --job-name=tpcmask_h200
#SBATCH --gres=gpu:h200:1
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=5:00:00
#SBATCH --account=gts-hkim358-paid
#SBATCH -q inferno
#SBATCH -p gpu-h200
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_tpc_holder.out
# Holder for the TPC-mask placement study: keeps one H200 allocated; work is
# run inside it with srun --jobid --overlap.
hostname; nvidia-smi -L; nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,memory.total --format=csv,noheader; date
srun sleep 1000000000
