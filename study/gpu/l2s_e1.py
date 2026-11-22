#!/usr/bin/env python3
"""E-1 / E-2 / same-GPC / local-remote / Part 2 runs with the fixed-window tw kernel.
python3 l2s_e1.py <phase>; output appended to results_gpu/h200_l2s_<phase>.txt"""
import os, sys, itertools
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from l2s_run import run, load_map, gpcs, lst, LISTS
B = load_map(); G = gpcs()
SIDE = {'A': [0, 1, 5, 7], 'B': [2, 3, 4, 6]}
def sms(g, n=None, step=1):
    v = [x for b in G[g] for x in B[b]]
    v = v[::step]
    return v if n is None else v[:n]
TMA = ['--tma', 1, '--tile-kb', 32, '--depth', 6]
LD = ['--ld', 32, '--block', 4096]
MODES = [('TMA', TMA), ('LD16B', LD)]
def tw(ph, d, groups, mode, dur=1000, extra=()):
    run(ph, d, ['tw', '--groups', groups] + list(mode) + ['--dur-ms', dur] + list(extra))

def e1():
    ph = 'e1'
    # (a) bystanders: 2 SMs (first SM of the first two TPCs) in each of the 7 GPCs other than GPC 6
    by = [x for g in range(8) if g != 6 for x in sms(g, 2, 2)]
    for mn, m in MODES:
        tw(ph, 'bystanders alone: 2 SMs x 7 GPCs, %s' % mn, lst(by), m)
        for n in [16, 6]:
            tw(ph, 'bystanders + GPC6 %d SMs, %s' % (n, mn), '%s;%s' % (lst(by), lst(sms(6, n))), m)
        tw(ph, 'GPC6 16 SMs alone, %s' % mn, lst(sms(6, 16)), m)
        tw(ph, 'GPC6 6 SMs alone, %s' % mn, lst(sms(6, 6)), m)
    # (b) saturation: 12 SMs in each of g GPCs, balanced across halves and one half
    bal = {1: [1], 2: [1, 2], 3: [1, 2, 5], 4: [1, 2, 5, 3], 6: [1, 2, 5, 3, 7, 4], 8: [1, 2, 5, 3, 7, 4, 0, 6]}
    one = {2: [1, 5], 3: [1, 5, 7], 4: [1, 5, 7, 0]}
    for mn, m in MODES:
        for g, gl in bal.items():
            tw(ph, 'saturation 12 SMs x %d GPCs balanced %s, %s' % (g, gl, mn), ';'.join(lst(sms(x, 12)) for x in gl), m, extra=('--mb-per-unit', 32))
        for g, gl in one.items():
            tw(ph, 'saturation 12 SMs x %d GPCs one half %s, %s' % (g, gl, mn), ';'.join(lst(sms(x, 12)) for x in gl), m, extra=('--mb-per-unit', 32))
    # all-pairs GPC matrix, 12 SMs each, 16 B loads
    for a, b in itertools.combinations(range(8), 2):
        tw(ph, 'pair GPC%d+GPC%d 12 SMs each, LD16B' % (a, b), '%s;%s' % (lst(sms(a, 12)), lst(sms(b, 12))), LD, dur=500, extra=('--mb-per-unit', 32))
    for a in range(8):
        tw(ph, 'single GPC%d 12 SMs, LD16B' % a, lst(sms(a, 12)), LD, dur=500, extra=('--mb-per-unit', 32))

def e2():
    ph = 'e2'
    allsm = [x for g in range(8) for x in sms(g)]
    for mb in [0.125, 0.25]:
        for mn, m in MODES:
            tw(ph, 'all 132 SMs L2-resident disjoint %.3f MB/SM (%.1f MB total), %s' % (mb, mb * 132, mn), lst(allsm), m, extra=('--mb-per-unit', mb))
        tw(ph, 'all 132 SMs L2-resident shared %.3f MB, LD16B' % mb, lst(allsm), LD, extra=('--mb-per-unit', mb, '--shared', 1))
    for mn, m in MODES:
        tw(ph, 'all 132 SMs DRAM disjoint 32 MB/SM, %s' % mn, lst(allsm), m, extra=('--mb-per-unit', 32))
    # per GPC, L2-resident disjoint, all SMs of each GPC (one group per GPC)
    tw(ph, 'all 132 SMs L2-resident disjoint 0.125 MB/SM, one group per GPC, LD16B', ';'.join(lst(sms(g)) for g in range(8)), LD, extra=('--mb-per-unit', 0.125))

def gpcsweep():
    ph = 'gpcsweep'
    for g in [6, 1]:
        for n in [1, 2, 3, 4, 6, 8, 10, 12, 14, 16, 18]:
            s = sms(g, n)
            if len(s) < n: continue
            d = 'GPC%d n=%d' % (g, n)
            tw(ph, d + ' DRAM read disjoint, LD16B', lst(s), LD, dur=300)
            tw(ph, d + ' DRAM read disjoint, TMA', lst(s), TMA, dur=300)
            tw(ph, d + ' DRAM write disjoint, ST16B', lst(s), LD, dur=300, extra=('--write', 1))
            tw(ph, d + ' L2 read disjoint 256KB/SM, LD16B', lst(s), LD, dur=300, extra=('--mb-per-unit', 0.25))
            tw(ph, d + ' L2 read shared 256KB, LD16B', lst(s), LD, dur=300, extra=('--mb-per-unit', 0.25, '--shared', 1))
            tw(ph, d + ' L2 read shared 4MB, LD16B', lst(s), LD, dur=300, extra=('--mb-per-unit', 4, '--shared', 1))
            tw(ph, d + ' L2 write disjoint 256KB/SM, ST16B', lst(s), LD, dur=300, extra=('--mb-per-unit', 0.25, '--write', 1))

def local():
    ph = 'local'
    near = {sd: os.path.join(LISTS, 'near_side%s.u32' % sd) for sd in SIDE}
    far = {sd: os.path.join(LISTS, 'far_side%s.u32' % sd) for sd in SIDE}
    side_of = {g: sd for sd, gs in SIDE.items() for g in gs}
    def grp(by_side, kind, mb):
        parts = []
        for sd, v in by_side.items():
            if not v: continue
            parts.append('%s@%s' % (lst(v), {'local': near[sd], 'remote': far[sd], 'mixed': 'all'}[kind]))
        return ';'.join(parts)
    cases = [('one SM GPC%d' % g, {side_of[g]: sms(g, 1)}, 64) for g in [0, 1, 2, 6]]
    cases += [('GPC%d 6 SMs' % g, {side_of[g]: sms(g, 6)}, 64) for g in [1, 6]]
    cases += [('GPC%d all SMs' % g, {side_of[g]: sms(g)}, 32) for g in [1, 6]]
    cases += [('side %s all 66 SMs' % sd, {sd: [x for g in gs for x in sms(g)]}, 16) for sd, gs in SIDE.items()]
    for per in [2, 3, 4, 6]:
        cases.append(('spread %d SMs per GPC' % per, {sd: [x for g in gs for x in sms(g, per, 2)] for sd, gs in SIDE.items()}, 32))
    cases.append(('whole chip 132 SMs', {sd: [x for g in gs for x in sms(g)] for sd, gs in SIDE.items()}, 16))
    for name, bs, mb in cases:
        for kind in ['local', 'remote', 'mixed']:
            tw(ph, '%s reading %s pages, LD16B' % (name, kind), grp(bs, kind, mb), LD, dur=500, extra=('--mb-per-unit', mb))
    # TMA with 4 KB tiles (partition-pure) for one SM and one GPC
    T4 = ['--tma', 4, '--tile-kb', 4, '--depth', 12]
    for name, bs, mb in cases[:1] + cases[4:5] + cases[-1:]:
        for kind in ['local', 'remote', 'mixed']:
            tw(ph, '%s reading %s pages, TMA 4x(4KBx12)' % (name, kind), grp(bs, kind, mb), T4, dur=500, extra=('--mb-per-unit', mb))

def part2():
    ph = 'part2'
    one = '0'; pair = '0,1'
    near, far = os.path.join(LISTS, 'near_sideA.u32'), os.path.join(LISTS, 'far_sideA.u32')
    runs = [
        ('(i) TMA alone 32KBx6', TMA, ()), ('(i) TMA alone 2 issuers 32KBx3', ['--tma', 2, '--tile-kb', 32, '--depth', 3], ()),
        ('(ii) loads alone 32 warps', LD, ()), ('(ii) loads alone 24 warps', ['--ld', 24, '--block', 4096], ()),
        ('(ii) loads alone 32 warps, carveout 100 (small L1)', LD, ('--carveout', 100)),
        ('(iii) same CTA TMA 32KBx6 + 24 load warps', TMA + ['--ld', 24, '--block', 32768], ()),
        ('(iii) same CTA TMA 32KBx6 + 16 load warps', TMA + ['--ld', 16, '--block', 32768], ()),
        ('(iii) two CTAs: TMA kernel + 24-warp load kernel (carveout 100 both)', TMA, ('--second-ld', 24, '--carveout', 100)),
        ('(iii) two CTAs: TMA kernel + 30-warp load kernel (carveout 100 both)', TMA, ('--second-ld', 30, '--carveout', 100)),
        ('(iv) smem readers alone 2 warps', ['--smr', 2], ()), ('(iv) smem readers alone 4 warps', ['--smr', 4], ()),
        ('(iv) smem readers alone 8 warps', ['--smr', 8], ()),
        ('(iv) TMA 32KBx5 alone', ['--tma', 1, '--tile-kb', 32, '--depth', 5], ()),
        ('(iv) TMA 32KBx5 + 2 smem readers', ['--tma', 1, '--tile-kb', 32, '--depth', 5, '--smr', 2], ()),
        ('(iv) TMA 32KBx5 + 1 smem reader', ['--tma', 1, '--tile-kb', 32, '--depth', 5, '--smr', 1], ()),
        ('near pages: loads 32 warps', LD, ('--list-dummy', 0)),
    ]
    for d, m, ex in runs[:-1]:
        for s, lbl in [(one, '1 SM'), (pair, '2 SMs of a TPC')]:
            tw(ph, '%s %s' % (lbl, d), s, m, dur=500, extra=ex)
    for kind, L in [('near', near), ('far', far)]:
        tw(ph, '1 SM loads 32 warps, %s pages' % kind, '0@%s' % L, LD, dur=500)
        tw(ph, '1 SM TMA 4x(4KBx12), %s pages' % kind, '0@%s' % L, ['--tma', 4, '--tile-kb', 4, '--depth', 12], dur=500)
        tw(ph, '1 SM TMA 4x(4KBx12) + 24 load warps, %s pages' % kind, '0@%s' % L, ['--tma', 4, '--tile-kb', 4, '--depth', 12, '--ld', 24], dur=500)
    tw(ph, '1 SM TMA 4x(4KBx12), mixed pages', '0', ['--tma', 4, '--tile-kb', 4, '--depth', 12], dur=500)

if __name__ == '__main__':
    globals()[sys.argv[1]]()
