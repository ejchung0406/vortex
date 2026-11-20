#!/usr/bin/env python3
"""Summarize bwstream points under results/: achieved memory bandwidth per run.

usage: collect.py <name-prefix> [--mhz 1980] [--peak-gbs 3350]
"""
import argparse
import glob
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))


def grab(pattern, text, cast=int):
    m = re.search(pattern, text)
    return cast(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--mhz", type=float, default=1980.0)
    ap.add_argument("--peak-gbs", type=float, default=3350.0)
    args = ap.parse_args()

    rows = []
    for d in sorted(glob.glob(os.path.join(HERE, "results", args.prefix + "*"))):
        run = os.path.join(d, "run.log")
        meta = os.path.join(d, "meta.txt")
        if not os.path.exists(run):
            continue
        text = open(run, errors="replace").read()
        mt = open(meta).read() if os.path.exists(meta) else ""
        rows.append({
            "name": os.path.basename(d),
            "ok": "PASSED!" in text,
            "cores": grab(r"cores=(\d+)", text),
            "pattern": grab(r"pattern=(\w+)", text, str),
            "unroll": grab(r"unroll=(\d+)", text),
            "bytes": grab(r"bytes=(\d+)", text),
            "reads": grab(r"PERF: memory: reqs=\d+ \(r=(\d+)", text),
            "mem_lat": grab(r"PERF: memory:.*lat=([\d.]+)", text, float),
            "cycles": grab(r"PERF: instrs=\d+, cycles=(\d+)", text),
            "wall": grab(r"wall_s=(\d+)", mt),
        })

    rows.sort(key=lambda r: (r["pattern"] or "", r["unroll"] or 0, r["cores"] or 0))
    hdr = ("name", "ok", "cores", "pat", "u", "MiB", "reads/lines", "cycles", "B/cyc",
           "GB/s", "%peak", "GB/s/SM", "lat", "wall_s")
    print(" ".join(f"{h:>12}" for h in hdr))
    for r in rows:
        if not r["cycles"] or not r["bytes"]:
            print(f"{r['name']:>12} incomplete")
            continue
        lines = r["bytes"] / 64
        bpc = r["bytes"] / r["cycles"]
        gbs = bpc * args.mhz * 1e6 / 1e9
        vals = (r["name"][-12:], "ok" if r["ok"] else "FAIL", r["cores"], (r["pattern"] or "")[:5],
                r["unroll"], f"{r['bytes'] / 2**20:.1f}",
                f"{(r['reads'] or 0) / lines:.2f}", r["cycles"], f"{bpc:.1f}", f"{gbs:.1f}",
                f"{100 * gbs / args.peak_gbs:.1f}", f"{gbs / r['cores']:.1f}",
                r["mem_lat"], r["wall"])
        print(" ".join(f"{str(v):>12}" for v in vals))


if __name__ == "__main__":
    main()
