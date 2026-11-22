#!/usr/bin/env python3
"""Plans for bys_lab (bystander study). python3 bys_plan.py <outdir> [phase ...]
Writes <outdir>/bys_<phase>.plan. SM sets come from results_gpu/h200_l2s_map.txt
(TPC bit -> SMs) and results_gpu/h200_l2s_gpc.txt (GPC -> TPC bits), sides from
results_gpu/h200_l2s_sides.txt (override with BYS_SIDES=<file>)."""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'results_gpu')
MAP = os.environ.get('BYS_MAP', os.path.join(RES, 'h200_l2s_map.txt'))
GPCF = os.environ.get('BYS_GPC', os.path.join(RES, 'h200_l2s_gpc.txt'))
SIDEF = os.environ.get('BYS_SIDES', os.path.join(RES, 'h200_l2s_sides.txt'))


def load():
    b2s = {}
    for ln in open(MAP):
        f = ln.split()
        if f[:1] == ['bit']:
            b2s[int(f[1])] = [int(x) for x in f[3:]]
    G = []
    for ln in open(GPCF):
        f = ln.split()
        if f[:1] == ['gpc']:
            G.append([x for b in f[3:] for x in b2s[int(b)]])
    S = {}
    for ln in open(SIDEF):
        f = ln.split()
        if f[:1] == ['side']:
            S[f[1]] = [int(x) for x in f[3:]]
    return G, S


G, SIDES = load()
SIDE_OF = {g: s for s, gs in SIDES.items() for g in gs}
AGG = 6  # aggressor GPC (side B), as in E-1


def sms(g, n=None, step=1, skip=0):
    v = G[g][skip:][::step]
    return v if n is None else v[:n]


def L(v):
    return ','.join(str(x) for x in v)


def by_sms(exclude):
    """bystanders: first SM of the first two TPCs of every GPC not in exclude, split by side"""
    out = {'A': [], 'B': []}
    for g in range(len(G)):
        if g in exclude:
            continue
        out[SIDE_OF[g]] += sms(g, 2, 2)
    return out


TMA = 'mode=tma depth=6 tile=32'
LD = 'mode=ld warps=32'


class Plan:
    def __init__(self):
        self.lines = []

    def S(self, name, trials=5, dur=1000, bin=1000):
        self.lines.append('S name=%s trials=%d dur=%d bin=%d' % (name, trials, dur, bin))

    def G(self, name, smlist, spec):
        if smlist:
            self.lines.append('G name=%s sms=%s %s' % (name, L(smlist), spec))

    def bys(self, exclude=(AGG,), spec=TMA, pfx='by', data=None):
        b = by_sms(exclude)
        for sd in 'AB':
            d = '' if data is None else ' data=' + (data(sd) if callable(data) else data)
            self.G('%s%s' % (pfx, sd), b[sd], spec + d)

    def write(self, path):
        with open(path, 'w') as f:
            f.write('\n'.join(self.lines) + '\n')


def agg16(g=AGG):
    return sms(g, 16)


# ------------------------------------------------------------------ phases
def sanity(P):
    """E-1 reproduction, classification check (local/remote), each GPC's cap"""
    for mn, m in [('tma', TMA), ('ld', LD)]:
        P.S('san_by_%s' % mn, trials=3); P.bys(spec=m)
        P.S('san_by_agg_%s' % mn, trials=3); P.bys(spec=m); P.G('agg', agg16(), m)
        P.S('san_agg_%s' % mn, trials=3); P.G('agg', agg16(), m)
    sideA = [x for g in SIDES['A'] for x in sms(g)]
    for h in 'AB':
        P.S('san_sideA_home%s' % h, trials=3); P.G('sideA', sideA, LD + ' mb=16 data=home:%s' % h)
    for g in range(len(G)):
        P.S('san_gpc%d_16' % g, trials=3); P.G('gpc%d' % g, sms(g, 16), TMA)


def q1(P):
    """clock / power artefact?"""
    by = by_sms([AGG]); allby = by['A'] + by['B']
    others = [x for x in range(132) if x not in allby and x not in agg16()]
    for mn, m in [('tma', TMA), ('ld', LD)]:
        P.S('q1_by_%s' % mn); P.bys(spec=m)
        P.S('q1_by_agg_%s' % mn); P.bys(spec=m); P.G('agg', agg16(), TMA)
    P.S('q1_by_fma16'); P.bys(); P.G('fma', agg16(), 'mode=fma warps=32')
    P.S('q1_fma16'); P.G('fma', agg16(), 'mode=fma warps=32')
    P.S('q1_agg'); P.G('agg', agg16(), TMA)
    # force the power cap without a memory aggressor: tensor FMA on every other SM
    P.S('q1_by_fmaall'); P.bys(); P.G('fma', agg16() + others, 'mode=fma warps=32')
    P.S('q1_by_agg_fmaoth'); P.bys(); P.G('agg', agg16(), TMA); P.G('fma', others, 'mode=fma warps=32')
    # low duty aggressor: bystander rate folded on the aggressor's on / off phases (50 us bins)
    for on in [100, 250, 500]:
        P.S('q1_by_agg_duty%d' % on, bin=50); P.bys(); P.G('agg', agg16(), TMA + ' duty=%d/1000' % on)
        P.S('q1_by_agg_duty%d_ld' % on, bin=50); P.bys(spec=LD); P.G('agg', agg16(), TMA + ' duty=%d/1000' % on)


def q2(P):
    """dose-response"""
    P.S('q2_by'); P.bys()
    for n in [1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16]:
        P.S('q2_n%d' % n); P.bys(); P.G('agg', sms(AGG, n), TMA)
    for n in [4, 6, 7, 8, 12, 16]:
        P.S('q2_aggonly_n%d' % n); P.G('agg', sms(AGG, n), TMA)
    for d in [1, 2, 3, 4, 6]:
        P.S('q2_d%d' % d); P.bys(); P.G('agg', agg16(), 'mode=tma tile=32 depth=%d' % d)
        P.S('q2_aggonly_d%d' % d); P.G('agg', agg16(), 'mode=tma tile=32 depth=%d' % d)
    # demand throttled: per-SM pace as % of the GPC limit / 16 (80 GB/s per SM = 100%)
    for pct in [25, 50, 75, 85, 90, 95, 100, 105, 110, 125, 150]:
        P.S('q2_p%d' % pct); P.bys(); P.G('agg', agg16(), TMA + ' pace=%.2f' % (0.8 * pct))
    for pct in [90, 95, 100, 150]:
        P.S('q2_aggonly_p%d' % pct); P.G('agg', agg16(), TMA + ' pace=%.2f' % (0.8 * pct))
    # 8 SMs: at the knee (unpaced) and paced to the same delivered bytes
    for pct in [90, 95, 100]:
        P.S('q2_n8_p%d' % pct); P.bys(); P.G('agg', sms(AGG, 8), TMA + ' pace=%.2f' % (1.6 * pct))
    # number of saturated GPCs (16 SMs each); bystanders in the other GPCs
    sets = {'g6': [6], 'g6g2': [6, 2], 'g6g1': [6, 1], 'g6g2g3': [6, 2, 3], 'g6g1g5': [6, 1, 5], 'g6g2g1': [6, 2, 1], 'g6g1g2g5': [6, 1, 2, 5]}
    for nm, gs in sets.items():
        P.S('q2_sat_%s_byonly' % nm); P.bys(exclude=gs)
        P.S('q2_sat_%s' % nm); P.bys(exclude=gs)
        for g in gs:
            P.G('agg%d' % g, agg16(g), TMA)


def q3(P):
    """where is the shared queue"""
    by = {'tma': TMA, 'ld': LD}
    # aggressor L2-resident (disjoint 256 KB per SM)
    for mn, m in by.items():
        P.S('q3_aggL2_%s' % mn); P.bys(spec=m); P.G('agg', agg16(), m + ' mb=0.25')
        P.S('q3_aggL2only_%s' % mn); P.G('agg', agg16(), m + ' mb=0.25')
    # bystanders L2-resident
    P.S('q3_byL2'); P.bys(spec=TMA + ' mb=0.25')
    P.S('q3_byL2_agg'); P.bys(spec=TMA + ' mb=0.25'); P.G('agg', agg16(), TMA)
    P.S('q3_byL2_aggL2'); P.bys(spec=TMA + ' mb=0.25'); P.G('agg', agg16(), TMA + ' mb=0.25')
    P.S('q3_byLDL2'); P.bys(spec=LD + ' mb=0.25')
    P.S('q3_byLDL2_agg'); P.bys(spec=LD + ' mb=0.25'); P.G('agg', agg16(), TMA)
    # aggressor on a shared buffer (same lines for all 16 SMs)
    for mb in [0.25, 4]:
        P.S('q3_aggsh%g' % mb); P.bys(); P.G('agg', agg16(), LD + ' data=shared mb=%g' % mb)
        P.S('q3_aggsh%gonly' % mb); P.G('agg', agg16(), LD + ' data=shared mb=%g' % mb)
    # aggressor writing
    for nm, m in [('st', 'mode=st warps=32'), ('tmast', 'mode=tmast tile=32 depth=4')]:
        P.S('q3_aggw_%s' % nm); P.bys(); P.G('agg', agg16(), m)
        P.S('q3_aggw_%sonly' % nm); P.G('agg', agg16(), m)
    # page locality matrix (16 B loads, 4 KB-block lists); aggressor GPC6 is side B
    own = lambda sd: 'home:%s' % sd
    oth = lambda sd: 'home:%s' % ('B' if sd == 'A' else 'A')
    bk = {'loc': own, 'rem': oth, 'mix': 'contig'}
    ak = {'loc': 'data=home:B', 'rem': 'data=home:A', 'mix': 'data=contig'}
    for b, bd in bk.items():
        P.S('q3_pg_by%s' % b); P.bys(spec=LD, data=bd)
        for a, ad in ak.items():
            P.S('q3_pg_by%s_agg%s' % (b, a)); P.bys(spec=LD, data=bd); P.G('agg', agg16(), LD + ' ' + ad)
    for a, ad in ak.items():
        P.S('q3_pg_agg%sonly' % a); P.G('agg', agg16(), LD + ' ' + ad)
    # disjoint slice / FBP sets inside one 2 MB page (L2-resident, 32 KB per SM = 8 warps x 4 KB, 256 B granules).
    # The aggressor takes granules with fn = 0 and PA9 = 0; bystanders fn = 0 / 1 and PA9 = 1. PA9 is in no
    # routing term (GemAtlas; our bit-9 probe), so "same" = same slices, different lines.
    F0, S0, B9 = 0x151100, 0x030100, 0x200   # GemAtlas f0 = PA8^12^16^18^20, s0 = PA8^16^17 (offsets within the page)
    L8 = 'mode=ld warps=8 mb=0.03125'
    for fn, mask in [('f0', F0), ('s0', S0)]:
        agg = L8 + ' data=gran:0:%x:0:%x:0' % (mask, B9)
        for bv in [0, 1]:
            byd = 'gran:0:%x:%d:%x:1' % (mask, bv, B9)
            P.S('q3_sl_%s_agg0_by%d' % (fn, bv)); P.bys(spec=L8, data=byd); P.G('agg', agg16(), agg)
            P.S('q3_sl_%s_by%donly' % (fn, bv)); P.bys(spec=L8, data=byd)
        P.S('q3_sl_%s_agg0only' % fn); P.G('agg', agg16(), agg)


def q4(P):
    """latency of a one-load-at-a-time probe, near / far, hit / miss"""
    pa, pb = sms(0, 1), sms(2, 1)          # probe SMs: GPC0 (side A), GPC2 (side B)
    wa, wb = sms(1, 1), sms(3, 1)          # warmers for far lines: a side-A SM warms lines homed on A (far for pb)
    aggs = {'none': None, 'agg': TMA, 'aggL2': TMA + ' mb=0.25', 'aggp95': TMA + ' pace=76', 'aggp90': TMA + ' pace=72'}
    for an, am in aggs.items():
        for lt in ['hit', 'miss', 'far']:
            P.S('q4_%s_%s' % (lt, an))
            ex = {'hit': ' mb=0.25', 'miss': ' mb=1024', 'far': ''}[lt]
            P.G('probeA', pa, 'mode=lat lat=%s side=A%s%s' % (lt, ex, ' warm=%d' % wb[0] if lt == 'far' else ''))
            P.G('probeB', pb, 'mode=lat lat=%s side=B%s%s' % (lt, ex, ' warm=%d' % wa[0] if lt == 'far' else ''))
            if am:
                P.G('agg', agg16(), am)


def q5(P):
    """who is hurt most: deep vs shallow bystanders; fairness over aggressor GPC"""
    kinds = {'tmad6': TMA, 'tmad1': 'mode=tma tile=32 depth=1', 'ld32': LD, 'ld4': 'mode=ld warps=4', 'ld1': 'mode=ld warps=1'}
    for kn, km in kinds.items():
        P.S('q5_by_%s' % kn); P.bys(spec=km)
        P.S('q5_by_%s_agg' % kn); P.bys(spec=km); P.G('agg', agg16(), TMA)
    for g in range(len(G)):
        P.S('q5_rot%d_byonly' % g); P.bys(exclude=[g])
        P.S('q5_rot%d' % g); P.bys(exclude=[g]); P.G('agg', agg16(g), TMA)


def q6(P):
    """software avoidance: spread the aggressor at the same total (pace = per-SM GB/s)"""
    # 8 SMs of GPC6 + 8 SMs of GPC2 outside the bystander TPCs (TPCs 2-5 of GPC2)
    g2 = sms(2, 8, 1, 4)
    for nm, aggs in [('16in1_p80', [(agg16(), 80)]), ('16in1', [(agg16(), 0)]),
                     ('8in2_p80', [(sms(AGG, 8), 80), (g2, 80)]), ('8in2', [(sms(AGG, 8), 0), (g2, 0)]),
                     ('4in4_p80', [(sms(g, 4, 1, 4), 80) for g in [6, 2, 3, 4]]), ('4in4', [(sms(g, 4, 1, 4), 0) for g in [6, 2, 3, 4]])]:
        P.S('q6_%s' % nm); P.bys()
        for i, (s, pace) in enumerate(aggs):
            P.G('agg%d' % i, s, TMA + (' pace=%d' % pace if pace else ''))
        P.S('q6_%s_aggonly' % nm)
        for i, (s, pace) in enumerate(aggs):
            P.G('agg%d' % i, s, TMA + (' pace=%d' % pace if pace else ''))


def q7(P):
    """decode-like lane (2 SMs per GPC, 16 B loads) beside a packed memory-heavy tenant"""
    lane = [x for g in range(len(G)) for x in sms(g, 2, 2)]
    P.S('q7_lane'); P.G('lane', lane, LD)
    packs = {'k1': [6], 'k2': [6, 2], 'k4half': [6, 2, 3, 4], 'k4bal': [6, 1, 2, 5]}
    for nm, gs in packs.items():
        ten = [x for g in gs for x in G[g] if x not in lane]
        n = len(ten)
        # spread: same SM count, round-robin over all GPCs, outside the lane SMs
        pool = [[x for x in G[g] if x not in lane] for g in range(len(G))]
        spread = []
        i = 0
        while len(spread) < n:
            g = i % len(G)
            if pool[g]:
                spread.append(pool[g].pop(0))
            i += 1
        for kind, t in [('packed', ten), ('spread', spread)]:
            P.S('q7_%s_%s_only' % (nm, kind)); P.G('tenant', t, TMA + ' mb=32')
            P.S('q7_%s_%s' % (nm, kind)); P.G('lane', lane, LD); P.G('tenant', t, TMA + ' mb=32')


def ncu(P):
    """one trial each, 300 ms, for counter collection under ncu (run with --nomask 1 --only <name>)"""
    P.S('ncu_by', trials=1, dur=300); P.bys()
    P.S('ncu_agg', trials=1, dur=300); P.G('agg', agg16(), TMA)
    P.S('ncu_by_agg', trials=1, dur=300); P.bys(); P.G('agg', agg16(), TMA)
    P.S('ncu_by_aggp95', trials=1, dur=300); P.bys(); P.G('agg', agg16(), TMA + ' pace=76')
    P.S('ncu_by_aggL2', trials=1, dur=300); P.bys(); P.G('agg', agg16(), TMA + ' mb=0.25')


PHASES = ['sanity', 'q1', 'q2', 'q3', 'q4', 'q5', 'q6', 'q7', 'ncu']
if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for ph in (sys.argv[2:] or PHASES):
        P = Plan(); globals()[ph](P); P.write(os.path.join(out, 'bys_%s.plan' % ph))
        print(ph, sum(1 for l in P.lines if l.startswith('S ')), 'scenarios')
