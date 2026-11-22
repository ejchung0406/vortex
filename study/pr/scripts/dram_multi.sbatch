#!/usr/bin/bash
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --account=gts-hkim358
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/point_%j.out
#
# Several dram_batch.sbatch groups (one build tree each) in one job. PLAN is a
# bash file calling `grp <copy dir> <result name> <driver> "<cases>"` per group.
set -o pipefail
S=/storage/project/r-hkim358-0/echung67/vortex-exp/pr/scripts
E=/storage/project/r-hkim358-0/echung67/vortex-exp
grp() { SRC=$E/$1 NAME=$2 DRIVER=$3 CASES=$4 bash "$S/dram_batch.sbatch" || echo "group $2 failed rc=$?"; }
source "$S/dram_cases.sh"
source "${PLAN:?PLAN is required}"
