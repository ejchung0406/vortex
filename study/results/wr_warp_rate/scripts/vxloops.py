#!/usr/bin/env python3
"""Loops of warp_rate's kernel_main: static and executed instructions per iteration.
usage: vxloops.py <llvm-objdump -d kernel.elf>"""
import re, sys, collections
ins = []; on = False
for l in open(sys.argv[1]):
    if re.match(r"^[0-9a-f]+ <kernel_main>:", l): on = True; continue
    if on and re.match(r"^[0-9a-f]+ <", l): break
    m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]{8}\s+(\S+)\s*(.*)", l)
    if on and m: ins.append((int(m.group(1), 16), m.group(2), m.group(3)))
addr = [a for a, _, _ in ins]
for i, (a, op, args) in enumerate(ins):
    if op[0] == "b" or op == "j":
        m = re.search(r"0x([0-9a-f]+)", args)
        if not m: continue
        t = int(m.group(1), 16)
        if t < a and t in addr:
            s = addr.index(t)
            body = ins[s:i + 1]
            ops = collections.Counter(o for _, o, _ in body)
            # forward bnez over one addi inside the body: the skipped addi is not executed
            skipped = sum(1 for j, (_, o, ar) in enumerate(body[:-1]) if o == "bnez" and "0x" in ar)
            print(f"loop @{t:x}-{a:x}: static {len(body)} executed {len(body) - skipped} :: " + " ".join(f"{o}:{n}" for o, n in ops.most_common(8)))
