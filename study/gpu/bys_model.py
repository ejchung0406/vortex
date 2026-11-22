#!/usr/bin/env python3
"""Fit of the bystander model to results_gpu/h200_bys_{q1,q2,q5}.txt (python3 bys_model.py).

Model (same half as the over-subscribed GPC):
    L_agg = n_agg * W_agg / C_del            (Little's law on the aggressor GPC; C_del = its delivered rate)
    r_by  = min(r_free, W_by / (L_agg - delta))
Other half (a fraction f of the bystander's requests is served by the congested half's slices):
    r_by  = W_by / (f * (L_agg - delta) + (1 - f) * W_by / r_free)
W = bytes in flight per SM (TMA 32 KB x depth, LD 32 warps x 32 lanes x 8 x 16 B = 128 KB)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bys_analyze import load

G, S, R = load(['q1', 'q2', 'q5'])
KB = 1024


def m(s, g, k='med_gbs'):
    return G[(s, g)][k][0]


rows = []  # (label, n_agg, W_agg, C_del, W_by, r_free, r_obs_same, r_obs_other)
for n in [8, 10, 12, 14, 16]:
    s = 'q2_n%d' % n
    rows.append(('TMA agg n=%d' % n, n, 192 * KB, m(s, 'agg', 'sum_gbs'), 192 * KB, m('q2_by', 'byA'), m(s, 'byA'), m(s, 'byB')))
for d in [3, 4, 6]:
    s = 'q2_d%d' % d
    rows.append(('TMA agg depth=%d' % d, 16, 32 * KB * d, m(s, 'agg', 'sum_gbs'), 192 * KB, m('q2_by', 'byA'), m(s, 'byA'), m(s, 'byB')))
rows.append(('LD agg, LD by', 16, 128 * KB, m('q1_by_agg_ld', 'agg', 'sum_gbs'), 128 * KB, m('q1_by_ld', 'byA'), None, None))
# the LD-by-LD-agg run is in sanity (san_by_agg_ld); fall back to it
try:
    G2, _, _ = load(['sanity'])
    rows[-1] = ('LD agg, LD by', 16, 128 * KB, G2[('san_by_agg_ld', 'agg')]['sum_gbs'][0], 128 * KB,
                G2[('san_by_ld', 'byA')]['med_gbs'][0], G2[('san_by_agg_ld', 'byA')]['med_gbs'][0], G2[('san_by_agg_ld', 'byB')]['med_gbs'][0])
except KeyError:
    rows.pop()
rows.append(('TMA agg, LD32 by', 16, 192 * KB, m('q5_by_ld32_agg', 'agg', 'sum_gbs'), 128 * KB, m('q5_by_ld32', 'byA'), m('q5_by_ld32_agg', 'byA'), m('q5_by_ld32_agg', 'byB')))
rows.append(('TMA agg, TMA d1 by', 16, 192 * KB, m('q5_by_tmad1_agg', 'agg', 'sum_gbs'), 32 * KB, m('q5_by_tmad1', 'byA'), m('q5_by_tmad1_agg', 'byA'), m('q5_by_tmad1_agg', 'byB')))

# delta from the deep TMA family (n >= 10, depth 4 and 6): mean of L_agg - L_by
fit = [r for r in rows if r[0].startswith('TMA agg n=') and r[1] >= 10] + [r for r in rows if r[0] in ('TMA agg depth=4', 'TMA agg depth=6')]
ds = []
for lab, n, wa, c, wb, rf, rs, ro in fit:
    La = n * wa / (c * 1e9) * 1e6
    Lb = wb / (rs * 1e9) * 1e6
    ds.append(La - Lb)
delta = sum(ds) / len(ds)
print('delta (us) = %.3f  (values %s)' % (delta, ' '.join('%.3f' % x for x in ds)))
print('%-20s %7s %7s | %7s %7s %6s | %7s' % ('case', 'L_agg', 'L_by', 'r_obs', 'r_mod', 'err%', 'f_other'))
for lab, n, wa, c, wb, rf, rs, ro in rows:
    La = n * wa / (c * 1e9) * 1e6
    rmod = min(rf, wb / ((La - delta) * 1e-6) / 1e9) if La > delta else rf
    Lb = wb / (rs * 1e9) * 1e6 if rs else float('nan')
    # f for the other half: solve W/r_o = f*(La-delta) + (1-f)*W/rf
    if ro:
        tfree = wb / (rf * 1e9) * 1e6
        f = (wb / (ro * 1e9) * 1e6 - tfree) / ((La - delta) - tfree) if (La - delta) > tfree else float('nan')
    else:
        f = float('nan')
    print('%-20s %7.3f %7.3f | %7.1f %7.1f %+6.1f | %7.2f' % (lab, La, Lb, rs or float('nan'), rmod, 100 * (rmod / rs - 1) if rs else float('nan'), f))
