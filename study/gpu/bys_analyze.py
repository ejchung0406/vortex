#!/usr/bin/env python3
"""Tables from results_gpu/h200_bys_<phase>.txt (SUMG / SUMS / R lines of bys_lab run).
python3 bys_analyze.py [phase ...]  -> prints tables; also usable as a module (load())."""
import os, re, sys, statistics as st

RES = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'results_gpu')
num = r'(-?[\d.]+|nan)'
rng = num + r' \[' + num + '-' + num + r'\]'


def f(x):
    return float('nan') if x == 'nan' else float(x)


def load(phases):
    G, S, R = {}, {}, {}
    for ph in phases:
        p = os.path.join(RES, 'h200_bys_%s.txt' % ph)
        if not os.path.exists(p):
            continue
        cur = None
        for ln in open(p):
            if ln.startswith('SUMG '):
                d = dict(re.findall(r'(\w+)=(\S+)', ln))
                rec = {'n': int(d['n']), 'trials': int(d['trials'])}
                for k in ['med_gbs', 'sum_gbs', 'mhz', 'bpc', 'on', 'off', 'lat0_p50', 'lat1_p50']:
                    m = re.search(r'\b' + k + '=' + rng, ln)
                    if m:
                        rec[k] = tuple(f(x) for x in m.groups())
                G[(d['s'], d['g'])] = rec
            elif ln.startswith('SUMS '):
                d = dict(re.findall(r'(\w+)=(\S+)', ln))
                rec = {'trials_ok': int(d['trials_ok']), 'reasons': d['reasons']}
                for k in ['nv_sm_mhz', 'nv_pw']:
                    m = re.search(k + '=' + rng, ln)
                    rec[k] = tuple(f(x) for x in m.groups()) if m else (float('nan'),) * 3
                S[d['s']] = rec
            elif ln.startswith('R '):
                d = dict(re.findall(r'(\w+)=(\S+)', ln))
                cur = (d['s'], int(d['t']))
                R.setdefault(d['s'], []).append({'hdr': d, 'g': {}})
            elif ln.startswith('  g=') and cur:
                d = dict(re.findall(r'(\w+)=(\S+)', ln))
                R[cur[0]][-1]['g'][d['g']] = d
    return G, S, R


def v(G, s, g, k='med_gbs', i=0):
    r = G.get((s, g))
    return r[k][i] if r and k in r else float('nan')


def fmt(x, w=7, p=1):
    return ('%*.*f' % (w, p, x)) if x == x else ' ' * (w - 3) + 'n/a'


def row(G, S, s, groups, base=None):
    out = ['%-28s' % s]
    for g in groups:
        m, lo, hi = (G[(s, g)]['med_gbs'] if (s, g) in G else (float('nan'),) * 3)
        r = ''
        if base and (base, g) in G:
            r = ' (%4.2f)' % (m / G[(base, g)]['med_gbs'][0])
        out.append('%s=%s [%s-%s]%s bpc=%s' % (g, fmt(m), fmt(lo, 1), fmt(hi, 1), r, fmt(v(G, s, g, 'bpc'), 5, 3)))
    sm = S.get(s)
    if sm:
        out.append('nvclk=%s pw=%s' % (fmt(sm['nv_sm_mhz'][0], 5, 0), fmt(sm['nv_pw'][0], 5, 0)))
    return ' '.join(out)


if __name__ == '__main__':
    phases = sys.argv[1:] or ['sanity', 'q1', 'q2', 'q3', 'q4', 'q5', 'q6', 'q7']
    G, S, R = load(phases)
    scen = []
    for (s, g) in G:
        if s not in scen:
            scen.append(s)
    for s in scen:
        gs = [g for (s2, g) in G if s2 == s]
        print(row(G, S, s, gs))
