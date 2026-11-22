#!/usr/bin/bash
# Table of the tcu-bbuf-geometry result dirs: rc, total instrs/cycles, PASS/FAIL line.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/results
for d in pr_tcu-bbuf-geometry_*; do
  rc=$(grep -h '^rc=' $d/meta.txt 2>/dev/null | cut -d= -f2)
  perf=$(grep -h 'PERF: instrs=' $d/run.log 2>/dev/null | tail -1 | sed 's/PERF: //')
  st=$(grep -hoE 'PASSED!|FAILED!|Error[^,]*|STATIC_ASSERT|Assertion[^)]*' $d/run.log 2>/dev/null | head -1)
  printf "%-50s rc=%-3s %-45s %s\n" "${d#pr_tcu-bbuf-geometry_}" "${rc:--}" "$perf" "$st"
done
