#!/usr/bin/bash
# hrun.sh <jobid> <command...>: run inside a holder job
J=${1:?jobid}; shift
exec srun --jobid=$J --overlap -n 1 -c ${HCPUS:-8} --mem=0 bash -c "$*"
