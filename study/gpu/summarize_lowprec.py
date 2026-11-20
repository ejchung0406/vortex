#!/usr/bin/env python3
"""Tabulate a cublas_lowprec sweep: GB/s of weights per batch size and SM count.

usage: summarize_lowprec.py <results file>
"""
import re
import sys

rows, unsupported = [], set()
for line in open(sys.argv[1]):
    if "gpu=" not in line:
        if line.strip() and line.strip() != "done":
            print("  ", line.strip()[:160])
        continue
    d = dict(re.findall(r"(\w+)=([\w.]+)", line.replace(line[line.index("gpu="):line.index("sms=")], "")))
    if "unsupported" in d:
        unsupported.add((d["dtype"], d["unsupported"]))
    else:
        rows.append(d)
if unsupported:
    print("unsupported (dtype, cublas status):", sorted(unsupported))
for dt in ("f16", "bf16"):
    for gib in sorted({r["gib"] for r in rows if r["dtype"] == dt}, key=float, reverse=True):
        sel = [r for r in rows if r["dtype"] == dt and r["gib"] == gib]
        sms = sorted({int(r["sms"]) for r in sel}, reverse=True)
        print(f"\n== {dt}, matrix {float(gib) * 1024:.0f} MiB: GB/s of weights (per SM)")
        print("   M " + " ".join(f"{s:>4} SMs    " for s in sms))
        for m in sorted({int(r["m"]) for r in sel}):
            cells = []
            for s in sms:
                q = [r for r in sel if int(r["m"]) == m and int(r["sms"]) == s]
                cells.append(f"{float(q[0]['gbs']):6.0f}({float(q[0]['gbs']) / s:4.0f})" if q else " " * 12)
            print(f"{m:>4} " + " ".join(cells))
