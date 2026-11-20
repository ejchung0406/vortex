#!/usr/bin/bash
# Block until every named result directory has recorded an exit code.
# usage: wait_results.sh <name> [<name> ...]
cd "$(dirname "$0")/results" || exit 1
while :; do
  missing=0
  for n in "$@"; do
    grep -qs '^rc=' "$n/meta.txt" || missing=$((missing + 1))
  done
  [ "$missing" -eq 0 ] && break
  sleep 30
done
