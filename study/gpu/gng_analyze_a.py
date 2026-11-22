#!/usr/bin/env python3
"""Summarise experiment A raw files (gng_corun 'slot=' lines).
usage: gng_analyze_a.py <raw file> [<raw file> ...]   (an MPS run = .lane + .lender files)
Ratios: median(co-run) / median(solo of the same mode); range = min..max of the
co-run trials over the solo median."""
import statistics as st, sys
from collections import defaultdict


def parse(path):
    rows = []
    for l in open(path):
        if not l.startswith("slot="):
            continue
        d = {}
        for tok in l.split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                d.setdefault(k, v)
        rows.append(d)
    return rows


def merge(paths):
    by = defaultdict(dict)
    for p in paths:
        for d in parse(p):
            for k, v in d.items():
                if k not in by[d["slot"]] or by[d["slot"]][k] in ("-", "-1"):
                    by[d["slot"]][k] = v
    return list(by.values())


def f(d, k):
    try:
        return float(d[k])
    except (KeyError, ValueError):
        return None


def med(v):
    v = [x for x in v if x is not None]
    return st.median(v) if v else None


def fmt(x, p=2):
    return "-" if x is None else f"{x:.{p}f}"


paths = sys.argv[1:]
rows = merge(paths)
groups = defaultdict(list)
for d in rows:
    groups[d["tag"]].append(d)
solo_lane = {t.split("|")[1]: g for t, g in groups.items() if t.startswith("solo_lane|")}
solo_lend = {t.split("|")[1]: g for t, g in groups.items() if t.startswith("solo_lender|")}
print(f"# {', '.join(paths)}: {len(rows)} slots")
print("\n## Solo rates (median, range over trials; clock = median of per-slot NVML medians / min)")
print("| job | n | rate | range | per SM | workers | clk med/min MHz | reasons | check |")
print("|---|---|---|---|---|---|---|---|---|")
for k, g in sorted(solo_lane.items()):
    v = [f(d, "lane_gbs") for d in g]
    print(f"| lane {k} | {len(g)} | {fmt(med(v),1)} GB/s | {fmt(min(v),1)}-{fmt(max(v),1)} | {fmt(med([f(d,'lane_gbs_per_sm') for d in g]))} GB/s | "
          f"{fmt(med([f(d,'lane_workers') for d in g]),0)} | {fmt(med([f(d,'clk_med') for d in g]),0)}/{fmt(min(f(d,'clk_min') or 0 for d in g),0)} | "
          f"{'|'.join(sorted(set(d.get('reasons_or','?') for d in g)))} | {','.join(sorted(set(d.get('lane_check','?').split('(')[0] for d in g)))} |")
for k, g in sorted(solo_lend.items()):
    v = [f(d, "lender_rate") for d in g]
    print(f"| {k} | {len(g)} | {fmt(med(v),1)} {g[0].get('lender_rate_unit','')} | {fmt(min(v),1)}-{fmt(max(v),1)} | | | "
          f"{fmt(med([f(d,'clk_med') for d in g]),0)}/{fmt(min(f(d,'clk_min') or 0 for d in g),0)} | {'|'.join(sorted(set(d.get('reasons_or','?') for d in g)))} | "
          f"{','.join(sorted(set(d.get('lender_check','?') for d in g)))} |")

print("\n## Co-runs on the same SMs: ratios to solo")
print("| lane | lender | first | n | lane GB/s (chip) | lane ratio [range] | lender rate | lender ratio [range] | lane SMs | SMs shared (CUTLASS %smid) | lane late ms | clk med | check lane/lender |")
print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
for t, g in sorted(groups.items()):
    if not t.startswith("co|"):
        continue
    _, ln, ld, first = t.split("|")
    sl = med([f(d, "lane_gbs") for d in solo_lane.get(ln, [])])
    sd = med([f(d, "lender_rate") for d in solo_lend.get(ld, [])])
    lv = [f(d, "lane_gbs") for d in g]; dv = [f(d, "lender_rate") for d in g]
    lr = fmt(med(lv) / sl, 3) if sl and med(lv) is not None else "-"
    dr = fmt(med(dv) / sd, 3) if sd and med(dv) is not None else "-"
    lrr = f"{min(lv)/sl:.3f}-{max(lv)/sl:.3f}" if sl and None not in lv else "-"
    drr = f"{min(dv)/sd:.3f}-{max(dv)/sd:.3f}" if sd and None not in dv else "-"
    print(f"| {ln} | {ld} | {first} | {len(g)} | {fmt(med(lv),1)} | {lr} [{lrr}] | {fmt(med(dv),1)} | {dr} [{drr}] | "
          f"{fmt(med([f(d,'lane_workers') for d in g]),0)} | {fmt(med([f(d,'sms_shared_with_lane') for d in g]),0)} | "
          f"{fmt(med([f(d,'lane_late_ms') for d in g]),1)} | {fmt(med([f(d,'clk_med') for d in g]),0)} | "
          f"{','.join(sorted(set(d.get('lane_check','?').split('(')[0] for d in g)))}/{','.join(sorted(set(d.get('lender_check','?') for d in g)))} |")

dis = [t for t in groups if t.startswith("disj")]
if dis:
    print("\n## Disjoint baseline (lane TMA-only 32 KB x 6 on spread SMs, GEMM on the other TPCs)")
    print("| lane SMs | GEMM | lane GB/s co (solo) | GEMM co | GEMM masked solo | GEMM co / all-132 solo [range] | GEMM co / masked solo |")
    print("|---|---|---|---|---|---|---|")
    for n in sorted({t.split("_")[0] for t in dis}):
        lsolo = med([f(d, "lane_gbs") for d in groups.get(f"{n}_lane", [])])
        for t in sorted(x for x in dis if x.startswith(f"{n}_co|")):
            gname = t.split("|")[1]
            g = groups[t]
            ms = med([f(d, "lender_rate") for d in groups.get(f"{n}_lender|{gname}", [])])
            full = med([f(d, "lender_rate") for d in solo_lend.get(gname, [])])
            dv = [f(d, "lender_rate") for d in g]
            print(f"| {n[4:]} | {gname} | {fmt(med([f(d,'lane_gbs') for d in g]),1)} ({fmt(lsolo,1)}) | {fmt(med(dv),1)} | {fmt(ms,1)} | "
                  f"{fmt(med(dv)/full,3) if full else '-'} [{min(dv)/full:.3f}-{max(dv)/full:.3f}] | {fmt(med(dv)/ms,3) if ms else '-'} |")
