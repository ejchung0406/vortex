#!/usr/bin/bash
#SBATCH --job-name=borrow_h200
#SBATCH --gres=gpu:h200:1
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --time=4:30:00
#SBATCH --account=gts-hkim358-paid
#SBATCH -q inferno
#SBATCH -p gpu-h200
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/results_gpu/h200_borrow_holder.out
# Holder: keeps one H200 allocated; work is run inside it with srun --jobid --overlap.
hostname; nvidia-smi -L; date
srun sleep 1000000000
