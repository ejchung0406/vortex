#!/usr/bin/env python3
"""Schedules for gng_corun (experiment A). usage: gng_sched.py <kind> <map> <out>
kinds: smoke, same (two streams in one process), mps (two MPS clients)
Trials are interleaved (trial loop outermost) so drifts spread over all configs."""
import sys

kind, mapf, out = sys.argv[1:4]
bit2sms = {}
for l in open(mapf):
    f = l.split()
    if f and f[0] == "bit":
        bit2sms[int(f[1])] = [int(x) for x in f[3:]]
gpc_bits = [[0, 1, 2, 3, 75, 78, 81, 83]] + [list(range(9 * g, 9 * g + 9)) for g in range(1, 8)]
gpc_bits = [[b for b in g if b in bit2sms] for g in gpc_bits]


def spread(n_tpc):  # TPCs round-robin over the 8 physical GPCs
    sel, k = [], 0
    while len(sel) < n_tpc:
        for g in gpc_bits:
            if len(sel) < n_tpc and k < len(g):
                sel.append(g[k])
        k += 1
    sms = sorted(s for b in sel for s in bit2sms[b])
    rest = sorted(s for b in bit2sms if b not in sel for s in bit2sms[b])
    return sms, rest


def lst(v):
    return ",".join(map(str, v))


LANE = {f"L{m}{p}": f"M={m}:r={8 if p == 'p' else 0}:t=16:d=2" for m in (1, 4, 16) for p in "pu"}
LANE["Lt"] = "M=16:r=0:t=16:d=2:c=0"  # TMA-only lane: no consumer
GEMMS = [f"{lib}_{dt}_{n}" for lib in ("cublas", "cutlass") for dt in ("bf16", "fp8") for n in (3840, 15360)]
CARVE = ["cutlass_bf16m1_15360", "cutlass_fp8m1_15360", "cutlass_bf16m1_3840", "cutlass_fp8m1_3840"]
OTHER = ["ew", "fma2", "fma4"]
slots = []


def add(lane, lender, first, trial, tag):
    slots.append((lane, lender, first, trial, tag))


if kind == "smoke":
    one = bit2sms[54][0]
    add(LANE["L16u"], "-", "lane", 0, "solo_lane|L16u")
    add(LANE["L16p"], "-", "lane", 0, "solo_lane|L16p")
    add(LANE["L16u"] + f":sms={one}", "-", "lane", 0, "solo_lane|L16u_1sm")
    add("M=16:r=0:t=32:d=6:sms=%d" % one, "-", "lane", 0, "solo_lane|D32x6_1sm")
    for g in ["cublas_bf16_15360", "cutlass_bf16_15360", "cutlass_bf16m1_15360", "cublas_fp8_15360", "cutlass_fp8_15360", "ew", "fma4"]:
        add("-", g, "lane", 0, f"solo_lender|{g}")
    add(LANE["L16u"], "cublas_bf16_15360", "lane", 0, "co|L16u|cublas_bf16_15360|lane")
    add(LANE["L16u"], "cutlass_bf16_15360", "lane", 0, "co|L16u|cutlass_bf16_15360|lane")
    add(LANE["L16u"], "cutlass_bf16m1_15360", "lender", 0, "co|L16u|cutlass_bf16m1_15360|lender")
    add(LANE["L16u"], "fma4", "lender", 0, "co|L16u|fma4|lender")
elif kind in ("same", "mps"):
    if kind == "same":
        lenders1 = ["cublas_bf16_15360", "cublas_fp8_15360", "cutlass_bf16_15360", "cutlass_fp8_15360",
                    "cutlass_bf16m1_15360", "cutlass_fp8m1_15360", "ew", "fma2", "fma4"]
        lenders_small = ["cublas_bf16_3840", "cublas_fp8_3840", "cutlass_bf16_3840", "cutlass_fp8_3840"]
        lanes1 = ["L16u", "L16p", "Lt"]
    else:
        lenders1 = ["cublas_bf16_15360", "cutlass_bf16m1_15360", "cutlass_fp8_15360", "ew", "fma4"]
        lenders_small = []
        lanes1 = ["L16u", "Lt"]
    for tr in range(5):
        lanes_solo = lanes1 + (["L1u", "L4u"] if kind == "same" else [])
        for ln in lanes_solo:
            add(LANE[ln], "-", "lane", tr, f"solo_lane|{ln}")
        for g in lenders1 + lenders_small:
            add("-", g, "lane", tr, f"solo_lender|{g}")
        for g in lenders1:
            for ln in lanes1:
                for first in ("lane", "lender"):
                    add(LANE[ln], g, first, tr, f"co|{ln}|{g}|{first}")
        for g in lenders_small:
            for first in ("lane", "lender"):
                add(LANE["L16u"], g, first, tr, f"co|L16u|{g}|{first}")
        if kind == "same":
            for g in ["cublas_bf16_15360", "cutlass_bf16m1_15360", "ew", "fma4"]:
                for ln in ("L1u", "L4u"):
                    for first in ("lane", "lender"):
                        add(LANE[ln], g, first, tr, f"co|{ln}|{g}|{first}")
            one = bit2sms[54][0]
            s16, _ = spread(8)
            add(LANE["L16u"] + f":sms={one}", "-", "lane", tr, "solo_lane|L16u_1sm")
            add(LANE["Lt"] + f":sms={one}", "-", "lane", tr, "solo_lane|Lt_1sm")
            add(LANE["L16u"] + f":sms={lst(s16)}", "-", "lane", tr, "solo_lane|L16u_16spread")
            for ntpc in (12, 16):
                ls, rest = spread(ntpc)
                lane = f"M=16:r=0:t=32:d=6:c=0:sms={lst(ls)}"  # disjoint lane: large ring, TMA-only, unpaced
                add(lane, "-", "lane", tr, f"disj{len(ls)}_lane")
                for g in ["cublas_bf16_15360", "cutlass_bf16_15360"]:
                    gm = f"{g}@sms={lst(rest)}"
                    add("-", gm, "lane", tr, f"disj{len(ls)}_lender|{g}")
                    add(lane, gm, "lender", tr, f"disj{len(ls)}_co|{g}")
with open(out, "w") as f:
    for k, (lane, lender, first, tr, tag) in enumerate(slots):
        f.write(f"slot {k} lane {lane} lender {lender} first {first} trial {tr} tag {tag}\n")
print(f"{len(slots)} slots, {len(slots) * 2.6 / 60:.1f} min at 2.6 s per slot")
