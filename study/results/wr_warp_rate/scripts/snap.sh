#!/usr/bin/bash
# snap.sh <name> [base]: frozen source tree in node-local /tmp for builds;
# "base" = HEAD export + the warp_rate test only, otherwise a copy of src_wrate.
# third_party (prebuilt) is linked, not copied.
set -eo pipefail
W=/storage/project/r-hkim358-0/echung67/vortex-exp/src_wrate
S=/tmp/$USER/wrate/src_${1:?name}
rm -rf $S; mkdir -p $S
if [ "${2:-}" = base ]; then
  git -C /storage/project/r-hkim358-0/echung67/vortex archive 3c2c5e51c | tar -x -C $S --exclude=third_party
  cp -a $W/tests/regression/warp_rate $S/tests/regression/
else
  rsync -a --exclude=/third_party $W/ $S/
fi
rm -rf $S/third_party; ln -s $W/third_party $S/third_party
echo $S
