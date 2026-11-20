#!/usr/bin/env python3
# remove // DCDBG-BEGIN ... // DCDBG-END blocks from the given files, in place
import sys
for p in sys.argv[1:]:
    out=[]; skip=False; n=0
    for l in open(p):
        t=l.strip()
        if t=='// DCDBG-BEGIN': skip=True; n+=1; continue
        if t=='// DCDBG-END': assert skip; skip=False; continue
        if not skip: out.append(l)
    assert not skip
    open(p,'w').write(''.join(out))
    print(p, 'blocks removed:', n)
