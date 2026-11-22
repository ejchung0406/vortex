#!/usr/bin/bash
# Run a command on the bys_h200 holder job's node: BYS_JOB=<jobid> bys_srun.sh <cmd...>
exec srun --jobid=${BYS_JOB:?set BYS_JOB} --overlap -n1 bash -lc "module load cuda/12.9.1 >/dev/null 2>&1; cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu; $*" 2>&1 | grep -v -E "^(---------|Begin Slurm|Job ID|User ID|Account:|Job name|Partition:|QOS:)"
