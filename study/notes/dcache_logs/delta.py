#!/usr/bin/env python3
# usage: delta.py logA(short) logB(long) -> steady-state deltas
import sys,re
def parse(f):
    d={}
    for l in open(f,errors='replace'):
        l=l.strip()
        if l.startswith('PERF: instrs'):
            d['cycles']=int(re.search(r'cycles=(\d+)',l).group(1))
        if l.startswith('PERF: core0: dcache'):
            for k,v in re.findall(r'(\w+)=(\d+)',l): d['perf.'+k]=int(v)
        if l.startswith('DCDBG'):
            t=l.split()
            key=t[1]+('.'+t[2] if len(t)>2 and '=' not in t[2] and ':' not in t[2] else '')
            for k,v in re.findall(r'([\w:]+)=(\d+)',l): d[key+'.'+k]=int(v)
            if 'hist' in l:
                nums=[int(x) for x in l.split(':')[-1].split()]
                for i,x in enumerate(nums): d[t[1]+'.'+t[2].split('(')[0]+'.%d'%i]=x
    return d
a=parse(sys.argv[1]); b=parse(sys.argv[2])
bytes_=int(sys.argv[3]) if len(sys.argv)>3 else 2097152
dc=b['cycles']-a['cycles']
print(f"cycles {a['cycles']} -> {b['cycles']}  delta={dc}  B/cycle={bytes_/dc:.2f}  GB/s={bytes_/dc*1.98:.1f}")
for k in sorted(b):
    if k=='cycles' or k not in a: continue
    if 'max_mshr' in k: print(f"{k:40s} {a[k]} {b[k]}"); continue
    print(f"{k:40s} {b[k]-a[k]:9d}  {100.0*(b[k]-a[k])/dc:6.1f}% of cycles")
