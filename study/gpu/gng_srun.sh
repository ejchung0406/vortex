#!/usr/bin/bash
# Run a command on the gng_h200 holder job's node: gng_srun.sh <cmd...>
J=${GNG_JOB:-$(squeue -h -u $USER -n gng_h200 -o %i | head -1)}
exec srun --jobid=$J --overlap -n1 bash -lc "module load cuda/12.9.1 >/dev/null 2>&1; cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu; $*" 2>&1 | grep -v -E "^(---------|Begin Slurm|Job ID|User ID|Account:|Job name|Partition:|QOS:)"
