import sys,re
# whole-run bank0 slot shares (percent of bank0 ticks) and a few queue stats
for f in sys.argv[1:]:
    d={}
    for l in open(f,errors='replace'):
        if l.startswith('DCDBG bank0 '):
            d=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
        if l.startswith('DCDBG coalescer'): co=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
        if l.startswith('DCDBG arb port0'): p0=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
        if l.startswith('DCDBG arb ticks'): ar=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',l))
        if l.startswith('PERF: stalls'): st=l.strip()
    t=d['ticks']; fills=d['in_fill']
    keys=['in_core','in_coal','in_fill','in_replay','no_stall_rsp','no_stall_mem','no_fillblk_fwd','no_mshrfull','no_idle']
    print(f.split('/')[-1])
    print('   bank0 % of ticks: '+' '.join(f"{k}={100.0*d[k]/t:.1f}" for k in keys))
    print(f"   per fill: fwd_ok={d['fwd_ok']/fills:.2f} fwd_blk={d['fwd_blk']/fills:.2f} replay={d['in_replay']/fills:.2f} fill_wait={d['sum_mrsp']/fills:.1f}cyc  core_req_wait={d['sum_creq']/(d['in_core']+d['in_coal']):.1f}cyc  mshr_avg={d['sum_mshr']/t:.0f} max={d['max_mshr']}")
    print(f"   coalescers(4): ticks={co['ticks']} build={co['build']} drain_blocked={co['drain_blocked']} reqin_empty={co['reqin_empty']} rsp_blocked={co['rsp_blocked']}; port0 nonempty={100.0*p0['ne']/ar['ticks']:.0f}% of arb ticks")
    print('   '+st)
