import re,os
R='results/'
PT=r'TCU_CYCLES: max=(\d+)'; PI=r'IPC=([0-9.]+)'
def g(n,pat):
    try: s=open(R+n+'/run.log').read()
    except: return None
    m=re.findall(pat,s); return m[-1] if m else None
def cyc(n):
    v=g(n,r'cycles=(\d+)'); return int(v) if v else None
def reads(n):
    v=g(n,r'\(r=(\d+)'); return int(v) if v else None
def ok(n): return 'P' if g(n,r'(PASSED)') else ('F' if g(n,r'(FAILED)') else '-')
print("config:",sorted(set(re.findall(r'VX_CFG_(?:NUM_DXA_CORES|DXA_MAX_INFLIGHT|DCACHE_MSHR_SIZE|PLATFORM_MEMORY_SCHED_WINDOW|PLATFORM_MEMORY_BANK_GROUPS|PLATFORM_MEMORY_INTERLEAVE_SIZE|DCACHE_NUM_BANKS)=\d+',open(R+'fn_tma_k1_n8/run.log').read()))))
print("\n== streaming steady state GB/s (two lengths)")
for shp in ('blk','own'):
    for k in (1,4,11,22):
        a,b='fn_bw_%s_k%d_n256'%(shp,k),'fn_bw_%s_k%d_n512'%(shp,k)
        if cyc(a) and cyc(b):
            db=(reads(b)-reads(a))*64; dc=cyc(b)-cyc(a)
            print("  %s k%d: %7.1f GB/s (%5.1f/SM) [%d %d] %s%s"%(shp,k,db/dc*1.98,db/dc*1.98/k,cyc(a),cyc(b),ok(a),ok(b)))
print("== TMA copy steady state")
for k in (1,4,11,22):
    a,b='fn_tma_k%d_n8'%k,'fn_tma_k%d_n32'%k
    if cyc(a) and cyc(b):
        ta,tb=int(g(a,r'transfers=(\d+)')),int(g(b,r'transfers=(\d+)'))
        v=(tb-ta)*16384/(cyc(b)-cyc(a))*1.98
        print("  k%d: %7.1f GB/s (%5.1f/SM) %s%s"%(k,v,v/k,ok(a),ok(b)))
print("== GEMM kernel cycles (max) / GB/s per SM / vs 1 SM")
base={}
for lab,pat in [('gdec','fn_gdec_k%d'),('load','fn_gb_load_k%d'),('dxa d1','fn_gb_dxa_d1_k%d'),('dxa d2','fn_gb_dxa_k%d'),('dxa d3','fn_gb_dxa_d3_k%d'),('dxa d4','fn_gb_dxa_c16d4_k%d')]:
    row=[]
    for k in (1,11,22):
        n=pat%k; t=g(n,PT)
        if t:
            t=int(t)
            if k==1: base[lab]=t
            row.append("k%d: %d (%4.1f GB/s/SM, %+.0f%%) %s"%(k,t,8388608*1.98/t,100*(t/base.get(lab,t)-1),ok(n)))
        else: row.append("k%d: -"%k)
    print("  %-7s "%lab+" | ".join(row))
print("== compute / other")
for n in ('fn_cp_k1','fn_m64_s0','fn_m64_s1','fn_m64_s0_k11','fn_m64_s1_k11','fn_m64_s0_k22','fn_m64_s1_k22','fn_m256_s0','fn_m256_s1','fn_l1s_s0_k1','fn_l1s_s1_k1','fn_l1s_s0_k11','fn_l1s_s1_k11'):
    print("  %s: cycles=%s TCU=%s IPC=%s %s"%(n,cyc(n),g(n,PT),g(n,PI),ok(n)))
