#!/usr/bin/bash
# Remaining SimX points in priority order (one SimX build per worker count and side).
S=/storage/project/r-hkim358-0/echung67/vortex-exp/pr/scripts/dxa-dispatch-idle_local_simx.sh
while kill -0 ${1:-0} 2>/dev/null; do sleep 10; done
apps() { # side n [skip_copyd1]
  [ -z "$3" ] && $S $1 copyd1 dxa_copy "-d1" $2
  $S $1 copyt4 dxa_copy "-d1 -t0 4" $2
  $S $1 sg32 sgemm2_dxa "-n32 -t4 -m1" $2
  $S $1 mcast dxa_copy_mcast "" $2
}
apps base 4 skip
apps br d
apps br 4
apps base 8
apps br 8
echo chain-done
