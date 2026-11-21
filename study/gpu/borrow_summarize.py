#!/usr/bin/env python3
"""Tabulate the h200_borrow_* result files: borrow_summarize.py <file> [filter=value ...]"""
import re
import sys

KEYS = ["tag", "fused", "cluster", "helpers", "self", "issuers", "consume", "depth", "tile_kb", "nw", "clusters",
        "ctas", "distinct_sms", "cluster_on_distinct_sms", "window_mb", "check", "consumer_gbs_med",
        "consumer_gbs_range", "slowest_consumer", "aggregate_gbs", "event_aggregate_gbs",
        "test", "cg", "blocks", "unroll", "kb_in_flight_per_sm", "per_sm_gbs_med", "per_sm_gbs_range"]


def main():
    path = sys.argv[1]
    filt = dict(a.split("=", 1) for a in sys.argv[2:])
    rows = []
    for line in open(path):
        if not line.startswith("gpu=") or "SKIP" in line:
            continue
        d = dict(re.findall(r"(\w+)=([^ ]+)", line))
        if all(d.get(k) == v for k, v in filt.items()):
            rows.append(d)
    if not rows:
        return
    keys = [k for k in KEYS if any(k in r for r in rows) and k not in filt]
    w = {k: max(len(k), *(len(r.get(k, "")) for r in rows)) for k in keys}
    print("  ".join(k.ljust(w[k]) for k in keys))
    for r in rows:
        print("  ".join(r.get(k, "").ljust(w[k]) for k in keys))


if __name__ == "__main__":
    main()
