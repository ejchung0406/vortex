#!/usr/bin/env python3
"""Summarise experiment B (gng_swapab) output: usage gng_analyze_b.py results_gpu/h200_gng_b.txt"""
import re, statistics as st, sys

cur, rows = None, []
for l in open(sys.argv[1]):
    if l.startswith("### "):
        cur = {"args": l[4:].strip(), "trials": [], "clk": []}
        rows.append(cur)
    elif cur is not None and ("trial=" in l):
        d = dict(t.split("=", 1) for t in l.split() if "=" in t)
        cur["trials"].append(float(d.get("gbs_per_sm_med", d.get("gbs_per_sm", "nan"))))
        if "clk_med" in d:
            cur["clk"].append(float(d["clk_med"]))
        cur["last"] = d
    elif cur is not None and l.startswith("summary"):
        cur["summary"] = dict(t.split("=", 1) for t in l.split()[1:] if "=" in t)
        cur["check"] = re.search(r"check=(\S+)", l).group(1) if "check=" in l else "-"
print("| SMs | mode | config | GB/s per SM med [min-max] | B/clk at median SM clock | TFLOP/s per SM | clk MHz | check |")
print("|---|---|---|---|---|---|---|---|")
for r in rows:
    if not r["trials"]:
        print(f"| {r['args']} | FAILED | | | | | | |"); continue
    a = r["args"]
    sms = re.search(r"--sms (\S+)", a).group(1)
    mode = re.search(r"--mode (\S+)", a).group(1)
    cfg = " ".join(x for x in re.findall(r"--(nwg|stages|stage-kb|pf|M) (\S+)", a) and [f"{k}={v}" for k, v in re.findall(r"--(nwg|stages|stage-kb|pf|M) (\S+)", a)])
    g = st.median(r["trials"]); clk = st.median(r["clk"]) if r["clk"] else float("nan")
    tf = r["last"].get("tflops_per_sm", r["last"].get("tflops_useful", "-"))
    print(f"| {sms} | {mode} | {cfg} | {g:.1f} [{min(r['trials']):.1f}-{max(r['trials']):.1f}] | {g / (clk / 1000):.1f} | {tf} | {clk:.0f} | {r.get('check', '-')} |")
