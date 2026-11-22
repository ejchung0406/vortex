#!/usr/bin/bash
# sync.sh <name> [rev]: build copy vortex-exp/src_wr_<name> = src_all + the worktree's files (or a commit's)
set -eo pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
WT=$EXP/wt_wrate
D=$EXP/src_wr_${1:?name}
[ -d "$D" ] || cp -a $EXP/src_all $D
if [ -n "${2:-}" ]; then
  git -C $WT archive "$2" | tar -x -C $D
else
  (cd $WT && git ls-files -co --exclude-standard -z | rsync -a --from0 --files-from=- ./ $D/)
fi
echo synced $D
