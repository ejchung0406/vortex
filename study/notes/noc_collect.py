import re,os,sys
R='/storage/project/r-hkim358-0/echung67/vortex-exp/results'
def load(n):
    d=R+'/'+n
    try: log=open(d+'/run.log').read(); m=open(d+'/meta.txt').read()
    except: return None
    if 'PASSED' not in log: return 'FAIL' if re.search(r'^rc=',m,re.M) else None
    c=int(re.search(r'PERF: instrs=\d+, cycles=(\d+)',log).group(1)); r=int(re.search(r'PERF: memory: reqs=\d+ \(r=(\d+)',log).group(1))
    w=re.search(r'wall_s=(\d+)',m); cr={int(a):float(b) for a,b in re.findall(r'core_rate: core=(\d+) .*B_per_cycle=([\d.]+)',log)}
    return dict(bpc=r*64/c, cyc=c, wall=int(w.group(1)) if w else 0, cores=cr)
roof={'g2':1600/1.98,'g4':1600/1.98,'g64':2400/1.98,'off':1600/1.98}
def row(pref,sh,wl,bs):
    out=[]
    for b in bs:
        p=load(f'{pref}_{sh}_{wl}_pk{b}'); s=load(f'{pref}_{sh}_{wl}_sp{b}') if b>1 else p
        def f(x):
            if x is None: return 'pend'
            if x=='FAIL': return 'FAIL'
            return f"{x['bpc']:.0f} ({x['bpc']/b:.1f}/SM, {100*x['bpc']/roof[sh]:.0f}%)"
        rt=f"{s['bpc']/p['bpc']:.2f}" if isinstance(p,dict) and isinstance(s,dict) else '-'
        out.append(f"| {b} | {f(p)} | {f(s)} | {rt} |")
    return out
for pref in ['noc3','noc4','noc5']:
  for sh in ['g2','g4','g64']:
    for wl in ['dxa','bw']:
        rows=row(pref,sh,wl,[1,2,4,8,12,16,24,32])
        if all('pend | pend' in r for r in rows): continue
        print(f"\n### {pref} {sh} {wl} (B/clk total; roof {roof[sh]:.0f} B/clk)\n| b | packed | spread | sp/pk |\n|---|---|---|---|"); print('\n'.join(r for r in rows if 'pend | pend' not in r))
print('\n### bystanders (per-core B/clk: bystander mean | saturated-group mean | total B/clk)')
for sh,sat in [('g4',range(0,8)),('g64',range(0,16)),('g4c150',range(0,8))]:
  for side in [0,1]:
    for wl in ['dxa','bw']:
      res=[]
      for case in ['alone','sat','uns','satonly']:
        x=load(f'noc4_by_{sh}r{side}_{wl}_{case}') or load(f'noc5_by_{sh}r{side}_{wl}_{case}')
        if not isinstance(x,dict): res.append(f'{case}: {x}'); continue
        by=[v for c,v in x['cores'].items() if c not in sat]; st=[v for c,v in x['cores'].items() if c in sat]
        res.append(f"{case}: by={sum(by)/len(by):.1f} " if by else f"{case}: ")
        if st: res[-1]+=f"sat={sum(st)/len(st):.1f} "
        res[-1]+=f"tot={x['bpc']:.0f}"
      print(f"{sh} gate_req={side} {wl}: "+' | '.join(res))
print('\n### cost')
for n in ['noc3_cost_s22','noc3_cost_g2','noc3_cost_g64']:
    x=load(n); print(n, x if not isinstance(x,dict) else f"wall={x['wall']} cyc={x['cyc']} ms/kcyc={1000*x['wall']/x['cyc']:.2f} B/clk={x['bpc']:.0f}")
