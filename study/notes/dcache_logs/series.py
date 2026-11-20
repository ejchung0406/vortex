import sys
for t in sys.argv[1:]:
    for l in open(f'logs/{t}.log'):
        if 'series4096 fills' in l:
            v=[int(x) for x in l.split(':')[1].split()]
            d=[ (v[i]-v[i-1]) for i in range(1,len(v))]
            print(t,'B/cycle per 4096-cycle bucket:', ' '.join(f'{x*64/4096:.0f}' for x in d))
        if 'mshr_sum' in l:
            v=[int(x) for x in l.split(':')[1].split()]
            d=[ (v[i]-v[i-1]) for i in range(1,len(v))]
            print(t,'avg MSHR entries:', ' '.join(f'{x/4096:.0f}' for x in d))
        if l.startswith('PERF: instrs'): print(t, l.strip())
