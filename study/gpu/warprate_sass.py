#!/usr/bin/env python3
"""Count the SASS instructions of the timed loop of every warprate kernel.

usage: warprate_sass.py <cuobjdump -sass output>
The timed loop is the outermost backward branch whose body holds the
clock reads' successor code; for the producer kernel the inner try_wait loop
is reported separately (it runs once per iteration when the barrier is ready).
"""
import re, sys, collections

KIND = ["int_dep", "int_ind4", "int_ind8", "fma_dep", "fma_ind4", "fma_ind8",
        "br_every1", "br_every4", "br_every16", "lds_own", "lds_chase", "sts_own",
        "tma_producer", "producer_alu_only", "lds_own16", "producer_arrive_only", "arrive_no_wait", "try_wait_only", "fma_int_mix"]

funcs = {}
cur = None
for line in open(sys.argv[1]):
    m = re.search(r"Function : _Z4kernILi(\d+)E", line)
    if m:
        cur = int(m.group(1)); funcs[cur] = []; continue
    m = re.match(r"\s*/\*([0-9a-f]{4,})\*/\s+(.*?);", line)
    if m and cur is not None:
        funcs[cur].append((int(m.group(1), 16), m.group(2).strip()))

for k in sorted(funcs):
    ins = funcs[k]
    addr = [a for a, _ in ins]
    # clock reads bracket the loop
    clk = [i for i, (_, t) in enumerate(ins) if "SR_CLOCKLO" in t]
    lo, hi = clk[0], clk[-1]
    loops = []
    for i, (a, t) in enumerate(ins[lo:hi], lo):
        m = re.search(r"BRA (?:`\(\.L_x_\d+\)|)?\s*(0x[0-9a-f]+)", t)
        if m and "BRA" in t:
            tgt = int(m.group(1), 16)
            if tgt <= a:
                loops.append((addr.index(tgt), i))
    if not loops:
        print(f"{KIND[k]}: no loop found"); continue
    s, e = max(loops, key=lambda x: x[1] - x[0])
    body = [t for _, t in ins[s:e + 1]]
    ops = collections.Counter(re.sub(r"^@!?U?P\w+\s+", "", t).split()[0] for t in body)
    inner = [(a, b) for a, b in loops if (a, b) != (s, e)]
    inner_n = sum(b - a + 1 for a, b in inner)
    # executed per iteration: branch kernels skip each inner loop together with
    # its BSSY/BSYNC; the producer's wait loop runs once when the slot is ready.
    execd = len(body) - (inner_n + 2 * len(inner) if KIND[k].startswith("br_") else 0)
    print(f"{KIND[k]}: loop {len(body)} instr (static) executed_per_iter={execd} inner_loops={len(inner)} inner_instr={inner_n} :: "
          + " ".join(f"{o}:{n}" for o, n in ops.most_common()))
