#!/usr/bin/bash
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --time=4:00:00
#SBATCH --account=gts-hkim358
#SBATCH --output=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wrr/h200s_%x_%j.out
# TAG, CF from the environment: warp_rate table at h200s, SimX
D=/storage/project/r-hkim358-0/echung67/vortex-exp/slurm/wrr
R=/storage/project/r-hkim358-0/echung67/vortex-exp/results/wrr_dev
JOBS=8 $D/build.sh b h200s_$TAG > /dev/null || exit 1
CONFIGS="$CF" $D/sweep.sh h200s_$TAG $R/h200s_${TAG}_w1.txt simx h200s "0 1 2 3 4 5 6 7 8 9 10 11 12" 1 32 --perf=4
CONFIGS="$CF" $D/sweep.sh h200s_$TAG $R/h200s_${TAG}_wN.txt simx h200s "0 2 3 5 6 7 9 11 12" "2 4 8 16 32 64" 32 --perf=4
