#!/usr/bin/env python3
"""Per-instruction stage cycles from a SimX --debug=3 run.log.
usage: stages.py run.log [first_uuid] [count]"""
import re, sys
st = {}
pat = re.compile(r"TRACE\s+(\d+): \S+?(?:-pipeline)? (schedule|icache-req|icache-rsp|decode|issue|commit):.*?PC=(0x[0-9a-f]+).*?(?:eop=(\d))?.*\(#(\d+)\)")
for l in open(sys.argv[1]):
    m = pat.search(l)
    if not m: continue
    cyc, stage, pc, eop, uid = int(m.group(1)), m.group(2), m.group(3), m.group(4), int(m.group(5))
    d = st.setdefault(uid, {"pc": pc})
    if stage == "commit":
        if eop == "1" or "commit" not in d: d["commit"] = cyc
    elif stage not in d:
        d[stage] = cyc
first = int(sys.argv[2]) if len(sys.argv) > 2 else min(st)
n = int(sys.argv[3]) if len(sys.argv) > 3 else 20
cols = ["schedule", "icache-req", "icache-rsp", "decode", "issue", "commit"]
print("uuid pc " + " ".join(cols) + "  | sched->sched")
prev = None
for u in range(first, first + n):
    d = st.get(u)
    if not d: continue
    s = d.get("schedule")
    print(u, d["pc"], " ".join(str(d.get(c, "-")) for c in cols), " |", (s - prev) if (s is not None and prev is not None) else "-")
    prev = s
