#!/usr/bin/bash
# Make (or refresh) a clean build copy of a git revision: the revision's tracked
# files only, plus src_all's third_party submodule trees, with the generated
# ramulator_ext products removed so the copy's own third_party/Makefile builds
# them (dram_prep.sbatch).
#
#   dram_mkcopy.sh <dest> <git-dir> <rev>
set -eo pipefail
DEST=${1:?dest}
GITDIR=${2:?git dir}
REV=${3:?rev}
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
case "$DEST" in
  $EXP/src_pr_dram*) ;;
  *) echo "refusing: $DEST is not an src_pr_dram* copy" >&2; exit 1 ;;
esac
if [ ! -d "$DEST/third_party/ramulator" ]; then
  mkdir -p "$DEST"
  cp -a "$EXP/src_all/third_party" "$DEST/"
fi
# Drop every tracked-looking file except the third_party trees, then overlay.
find "$DEST" -mindepth 1 -maxdepth 1 ! -name third_party -exec rm -rf {} +
git -C "$GITDIR" archive "$REV" | tar -x -C "$DEST"
rm -f "$DEST"/third_party/ramulator_ext/*.cpp "$DEST"/third_party/ramulator_ext/*.o "$DEST"/third_party/ramulator_ext/*.so
echo "$(git -C "$GITDIR" rev-parse "$REV")" > "$DEST/.pr_rev"
echo "copy $DEST at $(cat "$DEST/.pr_rev")"
