#!/usr/bin/env python3
"""warp_rate table for Vortex runs: one warp's cycles per instruction and the core's
instructions per cycle by warp count. usage: vxsummary.py <sweep outputs...>"""
import re, sys
N = {"lds_own": 258}
rows = {}
for f in sys.argv[1:]:
    for l in open(f):
        m = re.match(r"kind=(\S+) W=(\d+) iters=\d+ cyc_per_iter_warp=([\d.]+) cyc_per_iter_sm=([\d.]+)", l)
        if m:
            rows.setdefault(m.group(1), {})[int(m.group(2))] = (float(m.group(3)), float(m.group(4)))
Ws = [1, 2, 4, 8, 16, 32, 64]
print("kind                 n   1 warp: cyc/iter cyc/instr IPC | core IPC W=" + ",".join(map(str, Ws)))
for k, d in rows.items():
    n = N.get(k, 130)
    w1 = d.get(1)
    head = f"{k:18s} {n:4d}  " + (f"{w1[0]:9.1f} {w1[0] / n:7.2f} {n / w1[0]:5.3f}" if w1 else " " * 23)
    print(head + " | " + " ".join(f"{W * n / d[W][1]:5.2f}" if W in d else "    -" for W in Ws))
