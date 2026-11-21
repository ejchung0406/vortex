#!/usr/bin/bash
# Run a command on the tpcmask_h200 holder job's node: tpc_srun.sh <cmd...>
exec srun --jobid=${TPC_JOB:-15954225} --overlap -n1 bash -lc "module load cuda/12.9.1 >/dev/null 2>&1; cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu; $*" 2>&1 | grep -v -E "^(---------|Begin Slurm|Job ID|User ID|Account:|Job name|Partition:|QOS:)"
