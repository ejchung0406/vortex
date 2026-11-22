#!/usr/bin/bash
# Bring ICE results back: ice:~/scratch/vortex-ice/results/<NAME>/ ->
# vortex-exp/results/<NAME>/, with "ran_on=ice" appended to meta.txt.
#
#   ice_fetch.sh <NAME>...        (FORCE=1 also fetches runs that have not finished)
#
# A name is skipped while its run has no rc= line yet (still running), and
# refused if results/<NAME> already exists on Phoenix from a Phoenix run.
set -uo pipefail
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
SSH="ssh -o BatchMode=yes"
[ $# -gt 0 ] || { echo "usage: ice_fetch.sh <NAME>..." >&2; exit 2; }
fail=0
for n in "$@"; do
  dst=$EXP/results/$n
  if [ -f "$dst/meta.txt" ] && ! grep -q '^ran_on=ice' "$dst/meta.txt"; then
    echo "$n: results/$n is a Phoenix run, not overwritten" >&2; fail=1; continue
  fi
  meta=$($SSH ice "cat ~/scratch/vortex-ice/results/$n/meta.txt 2>/dev/null") || true
  if [ -z "$meta" ]; then echo "$n: no such ICE result" >&2; fail=1; continue; fi
  if ! grep -q '^rc=' <<< "$meta" && [ "${FORCE:-0}" != 1 ]; then
    echo "$n: still running on ICE, skipped" >&2; fail=1; continue
  fi
  mkdir -p "$dst"
  rsync -a -e "$SSH" "ice:scratch/vortex-ice/results/$n/" "$dst/" || { fail=1; continue; }
  grep -q '^ran_on=ice' "$dst/meta.txt" || echo "ran_on=ice (fetched $(date -Is) from ice:~/scratch/vortex-ice/results/$n)" >> "$dst/meta.txt"
  echo "$n: $(grep -E '^(rc|wall_s)=' "$dst/meta.txt" | tr '\n' ' ')"
done
exit $fail
