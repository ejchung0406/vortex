#!/usr/bin/env python3
"""Summarize sgemm_tcu points: weight bytes and DRAM traffic per cycle.

usage: tcu_collect.py <name-prefix> [--mhz 1980]
"""
import argparse, glob, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
ap = argparse.ArgumentParser()
ap.add_argument("prefix")
ap.add_argument("--mhz", type=float, default=1980.0)
ap.add_argument("--elem-bytes", type=int, default=2)
a = ap.parse_args()

def grab(p, t, cast=int):
    m = re.search(p, t)
    return cast(m.group(1)) if m else None

print(f"{'name':>22} {'ok':>4} {'SMs':>4} {'M':>4} {'N':>6} {'K':>6} {'B MiB':>7} {'cycles':>9} "
      f"{'DRAM GB/s':>10} {'/SM':>6} {'weight GB/s':>11} {'/SM':>6} {'mem lat':>8}")
for d in sorted(glob.glob(os.path.join(HERE, "results", a.prefix + "*"))):
    run = os.path.join(d, "run.log")
    if not os.path.exists(run):
        continue
    t = open(run, errors="replace").read()
    cyc = grab(r"PERF: instrs=\d+, cycles=(\d+)", t)
    if not cyc:
        print(f"{os.path.basename(d):>22} incomplete")
        continue
    sms = len(set(re.findall(r"PERF: core(\d+):", t))) or 1
    mA = re.search(r"matrix A: (\d+)x(\d+)", t)
    mB = re.search(r"matrix B: (\d+)x(\d+)", t)
    M, K = int(mA.group(1)), int(mA.group(2))
    N = int(mB.group(2))
    reads = grab(r"PERF: memory: reqs=\d+ \(r=(\d+)", t) or 0
    lat = grab(r"PERF: memory:.*lat=([\d.]+)", t, float)
    wbytes = N * K * a.elem_bytes
    f = a.mhz * 1e6 / 1e9
    dram = reads * 64 / cyc * f
    wt = wbytes / cyc * f
    print(f"{os.path.basename(d):>22} {'ok' if 'PASSED' in t else 'FAIL':>4} {sms:>4} {M:>4} {N:>6} {K:>6} "
          f"{wbytes / 2**20:>7.1f} {cyc:>9} {dram:>10.1f} {dram / sms:>6.1f} {wt:>11.1f} {wt / sms:>6.1f} {str(lat):>8}")
