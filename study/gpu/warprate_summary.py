#!/usr/bin/env python3
"""Table of the warprate runs: cycles per instruction of one warp and SM IPC vs warps.
usage: warprate_summary.py <sass_counts.txt> <run outputs...>"""
import re, sys
execd = {}
for l in open(sys.argv[1]):
    m = re.match(r"(\S+): loop \d+ instr \(static\) executed_per_iter=(\d+)", l)
    if m: execd[m.group(1)] = int(m.group(2))
rows = {}
for f in sys.argv[2:]:
    for l in open(f):
        m = re.match(r"kind=(\S+) W=(\d+) iters=\d+ nsm=\d+ warps_per_sm=(\d+) cyc_per_iter_warp=([\d.]+) cyc_per_iter_sm=([\d.]+)", l)
        if m:
            k, W, wsm, cw, cs = m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4)), float(m.group(5))
            rows.setdefault(k, {})[W] = (wsm, cw, cs)
Ws = [1, 2, 4, 8, 16, 32, 64]
print("SASS instructions per loop iteration (executed), one warp: cycles/iteration, cycles/instr, instr/cycle;")
print("then SM instructions per cycle (warps on the SM x instr / SM span) for W = " + ",".join(map(str, Ws)))
for k, d in rows.items():
    n = execd[k]
    w1 = d[1][1]
    sm = []
    for W in Ws:
        if W in d:
            wsm, cw, cs = d[W]
            sm.append(f"{wsm * n / cs:5.2f}")
    print(f"{k:22s} n={n:3d}  1 warp: {w1:8.1f} cyc/iter {w1 / n:6.2f} cyc/instr {n / w1:5.2f} IPC | SM IPC: " + " ".join(sm))
