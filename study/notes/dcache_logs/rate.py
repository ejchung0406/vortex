#!/usr/bin/env python3
# in-run delivery rate from the DCDBG fill series: lines accepted by the dcache banks per 4096 cycles
import sys,re
def info(f):
    d={}
    for l in open(f,errors='replace'):
        if l.startswith('PERF: instrs'): d['cycles']=int(re.search(r'cycles=(\d+)',l).group(1))
        if 'load_lat' in l: d['load_lat']=float(re.search(r'load_lat=([\d.]+)',l).group(1))
        if l.startswith('PERF: memory: reqs'): d['mem_lat']=float(re.search(r'lat=([\d.]+)',l).group(1))
        if 'series4096 fills' in l: d['fills']=[int(x) for x in l.split(':')[1].split()]
        if 'series4096 mshr_sum' in l: d['mshr']=[int(x) for x in l.split(':')[1].split()]
        if 'TCU_CYCLES' in l: d['tcu']=l.strip()
    return d
for f in sys.argv[1:]:
    d=info(f)
    out=f"{f.split('/')[-1]:22s} cycles={d.get('cycles')}"
    if 'fills' in d:
        v=d['fills']; tot=v[-1]
        lo=next(i for i,x in enumerate(v) if x>=0.10*tot); hi=max(i for i,x in enumerate(v) if x<=0.75*tot)
        if hi>lo:
            r=(v[hi]-v[lo])*64/((hi-lo)*4096.0)
            m=d['mshr']; occ=(m[hi]-m[lo])/((hi-lo)*4096.0)
            out+=f"  lines={tot}  mid-run(10-75%)={r:5.1f} B/c = {r*1.98:5.1f} GB/s  mshr_avg={occ:5.0f}"
    if 'load_lat' in d: out+=f"  load_lat={d['load_lat']}"
    if 'mem_lat' in d: out+=f"  mem_lat={d['mem_lat']}"
    print(out)
