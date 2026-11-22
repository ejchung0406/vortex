#!/usr/bin/env python3
"""Tabulate pr_fh_* results: one row per point, one column per variant suffix."""
import os
import re
import sys

R = "/storage/project/r-hkim358-0/echung67/vortex-exp/results"
prefix = sys.argv[1] if len(sys.argv) > 1 else "pr_fh_"
cols = sys.argv[2].split(",") if len(sys.argv) > 2 else ["base", "h0", "h1", "rtl"]
rows = {}
for d in sorted(os.listdir(R)):
    if not d.startswith(prefix) or d == "pr_fh_jobs":
        continue
    m = re.match(r"(.*)_([^_]+)$", d)
    if not m:
        continue
    key, var = m.group(1)[len(prefix):], m.group(2)
    meta = os.path.join(R, d, "meta.txt")
    if not os.path.exists(meta):
        continue
    t = open(meta).read()
    rc = re.search(r"^rc=(\d+)", t, re.M)
    p = re.search(r"instrs=(\d+), cycles=(\d+)", t)
    if rc is None:
        val = "run"
    elif p is None or rc.group(1) != "0":
        val = "rc" + (rc.group(1) if rc else "?")
    else:
        val = (int(p.group(1)), int(p.group(2)))
    rows.setdefault(key, {})[var] = val


def fmt(v):
    if isinstance(v, tuple):
        return "{:>10}".format(v[1])
    return "{:>10}".format(v or "-")


print("{:42}".format("point") + "".join("{:>10}".format(c) for c in cols) + "   instrs  notes")
for k, v in rows.items():
    line = "{:42}".format(k) + "".join(fmt(v.get(c)) for c in cols)
    ins = {c: v[c][0] for c in cols if isinstance(v.get(c), tuple)}
    notes = []
    if len(set(ins.values())) > 1:
        notes.append("INSTR MISMATCH " + str(ins))
    cy = {c: v[c][1] for c in cols if isinstance(v.get(c), tuple)}
    if "rtl" in cy:
        for c in cols:
            if c != "rtl" and c in cy:
                notes.append("{}:{:+.1f}%".format(c, 100.0 * (cy[c] - cy["rtl"]) / cy["rtl"]))
    elif "h0" in cy and "h1" in cy and cy["h0"]:
        notes.append("h1/h0:{:+.2f}%".format(100.0 * (cy["h1"] - cy["h0"]) / cy["h0"]))
    line += "   {}  {}".format(list(ins.values())[0] if ins else "-", " ".join(notes))
    print(line)
