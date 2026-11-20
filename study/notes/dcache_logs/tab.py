import re,sys,os
def cyc(f):
    if not os.path.exists(f): return None
    for l in open(f,errors='replace'):
        if l.startswith('PERF: instrs'): return int(re.search(r'cycles=(\d+)',l).group(1))
def tcu(f):
    if not os.path.exists(f): return None
    for l in open(f,errors='replace'):
        m=re.search(r'TCU_CYCLES: max=(\d+)',l)
        if m: return int(m.group(1))
print(f"{'config':8s} | {'own-u8 n256':>11s} {'n512':>7s} {'diff GB/s':>9s} | {'b32-u4 n256':>11s} {'n512':>7s} {'diff':>6s} | {'k8 n256':>8s} {'n512':>7s} {'diff':>6s} | gemm TCU max / cycles")
for c in sys.argv[1:]:
    row=f"{c:8s} |"
    for s in ['on','bn','k8n']:
        a=cyc(f'logs/z_{c}_{s}256.log'); b=cyc(f'logs/z_{c}_{s}512.log')
        d=f"{2097152/(b-a)*1.98:6.1f}" if a and b else '   -  '
        row+=f" {str(a):>11s} {str(b):>7s} {d:>9s} |" if s=='on' else f" {str(a):>11s} {str(b):>7s} {d:>6s} |" if s=='bn' else f" {str(a):>8s} {str(b):>7s} {d:>6s} |"
    row+=f" {tcu(f'logs/zg_{c}_gemm.log')} / {cyc(f'logs/zg_{c}_gemm.log')}"
    print(row)
