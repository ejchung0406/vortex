#!/usr/bin/env python3
"""Per-kernel stall-reason totals from pc_sampling_utility output (CUPTI continuous PC sampling).
usage: gng_pcsum.py <util output> [kernel substring ...]"""
import re, sys
from collections import defaultdict
tot = defaultdict(lambda: defaultdict(int))
for l in open(sys.argv[1]):
    m = re.match(r"functionName: ([^,]+),", l)
    if not m:
        continue
    f = m.group(1)
    for r, n in re.findall(r"smsp__pcsamp_warps_issue_stalled_(\w+?): (\d+)", l):
        if not r.endswith("_not_issued"):
            tot[f][r] += int(n)
keys = sys.argv[2:]
for f, d in sorted(tot.items(), key=lambda x: -sum(x[1].values())):
    if keys and not any(k in f for k in keys):
        continue
    s = sum(d.values())
    top = sorted(d.items(), key=lambda x: -x[1])[:7]
    print(f"{f[:60]:60s} samples={s:8d} " + " ".join(f"{r}={100*n/s:.1f}%" for r, n in top))
