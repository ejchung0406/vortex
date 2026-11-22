#!/usr/bin/bash
# bar_collect.sh <glob>: one line per result dir: name rc cycles instrs pass
cd /storage/project/r-hkim358-0/echung67/vortex-exp/results
for d in $1; do
  [ -d "$d" ] || continue
  rc=$(grep -o "rc=[0-9]*" $d/meta.txt 2>/dev/null | tail -1)
  perf=$(grep "^PERF: instrs" $d/run.log 2>/dev/null | tail -1 | sed 's/PERF: //')
  res=$(grep -o "PASSED!*\|FAILED!*\|Found [0-9]* errors[^!]*" $d/run.log 2>/dev/null | tail -1)
  printf "%-36s %-6s %-44s %s\n" "$d" "${rc:-run}" "$perf" "$res"
done
