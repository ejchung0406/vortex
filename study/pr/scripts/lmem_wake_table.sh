#!/usr/bin/bash
# Table of every pr_simx-lmem-wake result: rc, cycles, instrs, outcome.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/results
for d in pr_simx-lmem-wake_*; do
  printf "%-50s %-5s %-14s %-14s %s\n" $d "$(grep -h '^rc=' $d/meta.txt 2>/dev/null)" \
    "$(grep -h -o 'cycles=[0-9]*' $d/run.log 2>/dev/null | tail -1)" \
    "$(grep -h -o 'instrs=[0-9]*' $d/run.log 2>/dev/null | tail -1)" \
    "$(grep -h -o -E 'PASSED|FAILED|scheduler timeout|Error[^ ]*' $d/run.log 2>/dev/null | tail -1)"
done
