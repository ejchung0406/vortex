#!/usr/bin/env python3
"""Question tables for the bystander study from results_gpu/h200_bys_<phase>.txt.
python3 bys_report.py [phase ...]   (default: every phase found)"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bys_analyze import load

PH = ['sanity', 'q1', 'q2', 'q3', 'q4', 'q5', 'q6', 'q7']
G, S, R = load(sys.argv[1:] or PH)
nan = float('nan')


def g(s, grp, k='med_gbs', i=0):
    r = G.get((s, grp))
    return r[k][i] if r and k in r else nan


def rng(s, grp, k='med_gbs'):
    r = G.get((s, grp))
    if not r or k not in r:
        return '   n/a          '
    m, lo, hi = r[k]
    return '%6.1f [%5.1f-%5.1f]' % (m, lo, hi)


def nv(s):
    r = S.get(s)
    return '%4.0f MHz %4.0f W %s' % (r['nv_sm_mhz'][0], r['nv_pw'][0], r['reasons']) if r else ''


def line(s, groups, base=None, extra=()):
    out = ['%-26s' % s]
    for grp in groups:
        if (s, grp) not in G:
            continue
        x = '%s=%s' % (grp, rng(s, grp))
        if base and (base, grp) in G:
            x += ' x%.2f' % (g(s, grp) / g(base, grp))
        x += ' bpc=%.3f' % g(s, grp, 'bpc')
        if grp.startswith('agg') or grp in ('tenant', 'lane', 'fma'):
            x += ' sum=%.0f' % g(s, grp, 'sum_gbs')
        out.append(x)
    for k in extra:
        out.append(k)
    out.append(nv(s))
    print('  '.join(out))


def sect(t):
    print('\n## ' + t)


scen = []
for (s, _) in G:
    if s not in scen:
        scen.append(s)
BY = ['byA', 'byB']
AG = ['agg', 'agg0', 'agg1', 'agg2', 'agg3', 'agg6', 'agg2', 'agg1', 'agg5']

if any(s.startswith('san_') for s in scen):
    sect('sanity')
    for s in scen:
        if s.startswith('san_'):
            line(s, BY + ['agg', 'sideA'] + ['gpc%d' % k for k in range(8)])
if any(s.startswith('q1_') for s in scen):
    sect('Q1 clock / power')
    for s in scen:
        if s.startswith('q1_'):
            base = 'q1_by_ld' if s.endswith('_ld') and 'duty' not in s else 'q1_by_tma'
            ex = []
            for grp in BY:
                r = G.get((s, grp))
                if r and 'on' in r:
                    ex.append('%s on=%.1f off=%.1f' % (grp, r['on'][0], r['off'][0]))
            line(s, BY + ['agg', 'fma'], base, ex)
            if (s, 'fma') in G:
                print('%30s fma tflops(sum)=%.1f mhz=%.0f' % ('', g(s, 'fma', 'sum_gbs'), g(s, 'fma', 'mhz')))
for q in ['q2', 'q3', 'q5', 'q6', 'q7']:
    ss = [s for s in scen if s.startswith(q + '_')]
    if not ss:
        continue
    sect(q)
    for s in ss:
        base = {'q2': 'q2_by', 'q3': None, 'q5': None, 'q6': 'q1_by_tma', 'q7': 'q7_lane'}[q]
        if q == 'q2' and s.startswith('q2_sat_') and not s.endswith('byonly'):
            base = s + '_byonly'
        if q == 'q5':
            if s.startswith('q5_rot') and not s.endswith('byonly'):
                base = s + '_byonly'
            elif s.startswith('q5_by_') and s.endswith('_agg'):
                base = s[:-4]
        if q == 'q3':
            if s.startswith('q3_pg_by') and '_agg' in s:
                base = s.split('_agg')[0]
            elif s.startswith('q3_sl_') and '_agg0_by' in s:
                base = 'q3_sl_%s_by%sonly' % (s.split('_')[2], s[-1])
            elif s.startswith('q3_byL2_') or s.startswith('q3_byLDL2_'):
                base = s.rsplit('_', 1)[0]
            elif s.startswith('q3_agg') and 'only' not in s:
                base = 'q1_by_tma' if s.endswith('tma') or 'aggw' in s or 'aggsh' in s else 'q1_by_ld'
        grps = sorted({grp for (s2, grp) in G if s2 == s}, key=lambda x: (not x.startswith('by'), x))
        line(s, grps, base)
if any(s.startswith('q4_') for s in scen):
    sect('Q4 latency (cycles; class 0 = near/own-side home, class 1 = far home for miss)')
    for s in scen:
        if s.startswith('q4_'):
            for grp in ['probeA', 'probeB']:
                r = G.get((s, grp))
                if r:
                    print('%-22s %s p50 c0=%s c1=%s mhz=%.0f  %s' % (s, grp, r.get('lat0_p50', (nan,))[0], r.get('lat1_p50', (nan,))[0], r['mhz'][0], nv(s)))
