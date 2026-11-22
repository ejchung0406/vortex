#!/usr/bin/bash
#SBATCH --job-name=bys_h200
#SBATCH --gres=gpu:h200:1
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=1:40:00
#SBATCH --account=gts-hkim358-paid
#SBATCH -q inferno
#SBATCH -p gpu-h200
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_bys_holder.out
# Holder for the bystander study: keeps one H200 allocated; work runs inside
# it with srun --jobid --overlap (gpu/bys_srun.sh).
hostname; nvidia-smi -L; nvidia-smi --query-gpu=name,driver_version,clocks.max.sm,memory.total,power.limit --format=csv,noheader; date
srun sleep 1000000000
