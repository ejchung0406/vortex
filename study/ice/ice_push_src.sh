#!/usr/bin/bash
# Copy a Phoenix Vortex source tree to ICE as ~/scratch/vortex-ice/src/<name>.
#
#   ice_push_src.sh <phoenix-src-dir> [<name>]     (name defaults to the basename)
#
# Only what a build needs is sent (top-level build files and ci perf hw sim sw
# tests third_party profiles miscs, built third_party included); never .git,
# docs, study, top-level PDFs (JESD238B.01.pdf is a licensed standard) or
# agent state. rsync is incremental (size+mtime) and --delete keeps the copy
# identical to the Phoenix tree. Writes .ice_src_info (phoenix path, git rev,
# dirty count) into the copy; run_point_ice.sbatch puts it into meta.txt.
set -euo pipefail
SRC=$(readlink -f "${1:?usage: ice_push_src.sh <phoenix-src-dir> [<name>]}")
NAME=${2:-$(basename "$SRC")}
[ -f "$SRC/configure" ] || { echo "$SRC: not a Vortex source tree" >&2; exit 2; }
[[ "$NAME" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "bad name: $NAME" >&2; exit 2; }
SSH="ssh -o BatchMode=yes"
DEST=scratch/vortex-ice/src/$NAME

items=()
for f in configure config.mk.in Makefile.in VERSION VX_config.toml VX_types.toml vortex_opae.toml \
         ci perf hw sim sw tests third_party profiles miscs; do
  [ -e "$SRC/$f" ] && items+=("$f")
done

rev=$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)
dirty=$( [ -n "$rev" ] && git -C "$SRC" status --porcelain 2>/dev/null | wc -l || echo "")
info=$(printf 'src_rev=%s\nsrc_dirty=%s\nsrc_phoenix=%s\nsrc_pushed=%s\n' "$rev" "$dirty" "$SRC" "$(date -Is)")

$SSH ice "mkdir -p ~/$DEST"
cd "$SRC"
rsync -a --delete --relative \
  --exclude='.git' --exclude='.git/**' --exclude='*.pdf' --exclude='.omc' --exclude='.claude' \
  --exclude='.agents' --exclude='__pycache__' \
  -e "$SSH" "${items[@]}" "ice:$DEST/"
# the excludes above only filter the listed items; JESD238B.01.pdf sits at the
# top level and is never listed, but check anyway
$SSH ice "cd ~/$DEST && ! ls *.pdf > /dev/null 2>&1 && printf '%s\n' \"$info\" > .ice_src_info"
echo "pushed $SRC -> ice:~/$DEST ($rev${dirty:+, $dirty dirty})"
