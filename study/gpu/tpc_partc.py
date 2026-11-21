#!/usr/bin/env python3
"""Part C driver: placement-dependent MEM/CMP configurations on the H200.

Reads the mask-bit -> SM map (results_gpu/h200_tpc_map.txt, from
`tpc_lab probe`) and the TPC -> GPC map (results_gpu/h200_tpc_gpc.txt, lines
"gpc <g> bits <b...>", inferred in Part B), builds SM sets for each strategy
and runs ./tpc_lab run. Every output line of tpc_lab is appended to the
result file together with the strategy description.

  python3 tpc_partc.py <experiment> [--out FILE] [--reps R] [--dry]
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'results_gpu')
LAB = os.path.join(HERE, 'tpc_lab')
MAP = os.environ.get('TPC_MAP', os.path.join(RES, 'h200_tpc_map.txt'))
GPC = os.environ.get('TPC_GPC', os.path.join(RES, 'h200_tpc_gpc.txt'))


def load():
    bit2sms = {}
    for ln in open(MAP):
        f = ln.split()
        if f[:1] == ['bit']:
            bit2sms[int(f[1])] = [int(x) for x in f[3:]]
    gpcs = []
    for ln in open(GPC):
        f = ln.split()
        if f[:1] == ['gpc']:
            gpcs.append([int(x) for x in f[3:]])
    return bit2sms, gpcs


B2S, GPCS = load()
ALL_SMS = sorted(s for v in B2S.values() for s in v)
# GPCs ordered by size (largest first) so that "one GPC" uses a big one
GPC_ORDER = sorted(range(len(GPCS)), key=lambda g: -len(GPCS[g]))


def sms_of_bits(bits):
    return [s for b in bits for s in B2S[b]]


def lst(v):
    return ','.join(str(x) for x in v)


class Runner:
    def __init__(self, out, reps, dry):
        self.out, self.reps, self.dry = out, reps, dry

    def run(self, desc, args):
        cmd = [LAB, 'run', '--map', MAP, '--reps', str(self.reps)] + [str(a) for a in args]
        line = '# ' + desc + '\n# ' + ' '.join(cmd) + '\n'
        if self.dry:
            sys.stdout.write(line)
            return
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        res = p.stdout + (('STDERR ' + p.stderr) if p.stderr.strip() else '') + 'exit=%d secs=%.1f\n' % (p.returncode, time.time() - t0)
        with open(self.out, 'a') as f:
            f.write(line + res)
        sys.stdout.write(line + res)
        sys.stdout.flush()


def mem_tpcs(b, placement):
    """b MEM TPC bits: 'packed' in the biggest GPCs (fill one, then next),
    'spread' round-robin one per GPC."""
    if placement == 'packed':
        out = []
        for g in GPC_ORDER:
            for t in GPCS[g]:
                if len(out) < b:
                    out.append(t)
        return out
    out, k = [], 0
    while len(out) < b:
        for g in GPC_ORDER:
            if k < len(GPCS[g]) and len(out) < b:
                out.append(GPCS[g][k])
        k += 1
    return out


def gpc_of_bit(b):
    for g, v in enumerate(GPCS):
        if b in v:
            return g
    raise KeyError(b)


# ------------------------------------------------------------------ C.1
def exp_baseline(r):
    # MEM alone; packed vs spread vs one-SM-per-TPC; three MEM kinds
    kinds = [('ld', ['--mem', 'ld', '--per-sm', 2]),
             ('tma_consume', ['--mem', 'tma', '--self', 1, '--consume', 1, '--depth', 4, '--cluster', 1]),
             ('tma_writeonly', ['--mem', 'tma', '--self', 1, '--consume', 0, '--depth', 4, '--cluster', 1])]
    for b in [1, 2, 4, 8, 16]:
        for pl in ['packed', 'spread']:
            bits = mem_tpcs(b, pl)
            sms = sms_of_bits(bits)
            gp = sorted(set(gpc_of_bit(x) for x in bits))
            for name, a in kinds:
                if name == 'ld':
                    r.run('C1 %s b=%d %s sms=%d gpcs=%s' % (name, b, pl, len(sms), gp), a + ['--mem-sms', lst(sms)])
                else:
                    r.run('C1 %s b=%d %s sms=%d gpcs=%s' % (name, b, pl, len(sms), gp), a + ['--roles', ';'.join(map(str, sms))])
        # (c) same SM count as one SM per TPC over 2b TPCs (siblings idle)
        bits = mem_tpcs(2 * b, 'spread')
        sms = [B2S[t][0] for t in bits]
        gp = sorted(set(gpc_of_bit(x) for x in bits))
        for name, a in kinds:
            if name == 'ld':
                r.run('C1 %s b=%d one_sm_per_tpc sms=%d gpcs=%s' % (name, b, len(sms), gp), a + ['--mem-sms', lst(sms)])
            else:
                r.run('C1 %s b=%d one_sm_per_tpc sms=%d gpcs=%s' % (name, b, len(sms), gp), a + ['--roles', ';'.join(map(str, sms))])


def exp_gpcfill(r):
    # whole GPCs: one full GPC streaming vs the same SM count spread over all GPCs
    for g in GPC_ORDER[:2] + GPC_ORDER[-1:]:
        sms = sms_of_bits(GPCS[g])
        n = len(GPCS[g])
        bits = mem_tpcs(n, 'spread')
        sp = sms_of_bits(bits)
        for name, a in [('ld', ['--mem', 'ld', '--per-sm', 2])]:
            r.run('GPCFILL %s gpc=%d tpcs=%d packed' % (name, g, n), a + ['--mem-sms', lst(sms)])
            r.run('GPCFILL %s tpcs=%d spread' % (name, n), a + ['--mem-sms', lst(sp)])
        r.run('GPCFILL tma_consume gpc=%d tpcs=%d packed' % (g, n),
              ['--mem', 'tma', '--self', 1, '--consume', 1, '--depth', 4, '--roles', ';'.join(map(str, sms))])
        r.run('GPCFILL tma_consume tpcs=%d spread' % n,
              ['--mem', 'tma', '--self', 1, '--consume', 1, '--depth', 4, '--roles', ';'.join(map(str, sp))])


# ------------------------------------------------------------------ C.2
def cmp_args(exclude):
    ex = set(exclude)
    return ['--cmp-sms', lst([s for s in ALL_SMS if s not in ex])]


def roles_str(groups):
    return ';'.join('%d:%s' % (c, lst(h)) if h else '%d' % c for c, h in groups)


def borrow_groups(cons_bits, k, lender_pool):
    """consumers = both SMs of each bit in cons_bits; each gets k helper SMs
    from lender_pool[gpc] (list of SMs), same GPC. Returns groups, used SMs,
    cluster size (uniform) or None if it does not fit."""
    by_g = {}
    for b in cons_bits:
        by_g.setdefault(gpc_of_bit(b), []).append(b)
    groups, sizes = [], set()
    for g, bits in by_g.items():
        pool = list(lender_pool[g])
        cons = sms_of_bits(bits)
        if len(pool) < k * len(cons):
            return None, None
        for c in cons:
            groups.append((c, pool[:k]))
            pool = pool[k:]
        sizes.add(len(cons) * (1 + k))
    if len(sizes) != 1:
        return None, None
    C = sizes.pop()
    return groups, C


def lender_pool_excluding(excl_bits):
    pool = {}
    for g, bits in enumerate(GPCS):
        # whole TPCs so that cluster size = SM count of the masked TPCs
        pool[g] = sms_of_bits([b for b in bits if b not in excl_bits])
    return pool


def exp_borrow(r, with_cmp=True, bs=(1, 2, 4, 8), ks=(1, 3, 7), tag='C2'):
    base = ['--mem', 'tma', '--consume', 1]
    for b in bs:
        for pl in ['spread', 'packed']:
            bits = mem_tpcs(b, pl)
            cons = sms_of_bits(bits)
            cm = cmp_args(cons) if with_cmp else []
            ctag = 'cmp' if with_cmp else 'nocmp'
            # S0: no borrowing (self-issue, D=4), plain-load reference
            if pl == 'spread' or True:
                r.run('%s S0 b=%d %s %s tma self only' % (tag, b, pl, ctag),
                      base + ['--self', 1, '--depth', 4, '--cluster', 1, '--roles', ';'.join(map(str, cons))] + cm)
                r.run('%s S0ld b=%d %s %s plain loads' % (tag, b, pl, ctag),
                      ['--mem', 'ld', '--per-sm', 2, '--mem-sms', lst(cons)] + cm)
            # S2/S3/S4: k helpers per consumer in the same GPC, from CMP SMs
            pool = lender_pool_excluding(set(bits))
            for k in ks:
                groups, C = borrow_groups(bits, k, pool)
                if groups is None or C > 16:
                    with open(r.out, 'a') as f:
                        f.write('# %s S2 b=%d %s k=%d %s: does not fit (cluster > 16 or too few lenders in the GPC)\n' % (tag, b, pl, k, ctag))
                    continue
                D = max(1, min(4, 13 // (1 + k)))
                used = [c for c, _ in groups] + [h for _, hs in groups for h in hs]
                r.run('%s S%s b=%d %s k=%d %s cluster=%d depth=%d' % (tag, '3' if pl == 'spread' else '4', b, pl, k, ctag, C, D),
                      base + ['--self', 1, '--depth', D, '--cluster', C, '--roles', roles_str(groups)] + cm)
        # S1: one consumer per TPC, its sibling (running CMP) is the helper
        bits = mem_tpcs(2 * b, 'spread')
        groups = [(B2S[t][0], [B2S[t][1]]) for t in bits]
        cons = [c for c, _ in groups]
        cm = cmp_args(cons) if with_cmp else []
        r.run('%s S1 b=%d same-TPC helper %s cluster=2 depth=4' % (tag, b, 'cmp' if with_cmp else 'nocmp'),
              base + ['--self', 1, '--depth', 4, '--cluster', 2, '--roles', roles_str(groups)] + cm)
        # S1 reference: same consumers, helper in another TPC of the same GPC (k=1)
        # (built in exp_distance)


def exp_corun(r, bs=(1, 2, 4, 8)):
    # S5: MEM's own CTAs on every SM (its 2b SMs + co-resident on CMP's SMs)
    for b in bs:
        bits = mem_tpcs(b, 'spread')
        cons = sms_of_bits(bits)
        cm = cmp_args(cons)
        r.run('C2 S5 b=%d tma self on all SMs, cmp on the rest' % b,
              ['--mem', 'tma', '--consume', 1, '--self', 1, '--depth', 4, '--cluster', 1, '--roles', ';'.join(map(str, ALL_SMS))] + cm)
        r.run('C2 S5w b=%d tma write-only on all SMs, cmp on the rest' % b,
              ['--mem', 'tma', '--consume', 0, '--self', 1, '--depth', 4, '--cluster', 1, '--roles', ';'.join(map(str, ALL_SMS))] + cm)
        r.run('C2 S5ld b=%d plain loads 1 CTA/SM on all SMs, cmp on the rest' % b,
              ['--mem', 'ld', '--per-sm', 1, '--mem-sms', lst(ALL_SMS)] + cm)


def exp_distance(r):
    # 1 consumer + 1 helper: same TPC, other TPC of same GPC (several), no CMP
    for g in GPC_ORDER[:2]:
        bits = GPCS[g]
        c = B2S[bits[0]][0]
        r.run('DIST gpc=%d consumer=%d helper=%d same_tpc' % (g, c, B2S[bits[0]][1]),
              ['--mem', 'tma', '--consume', 1, '--self', 1, '--depth', 4, '--cluster', 2,
               '--roles', '%d:%d' % (c, B2S[bits[0]][1])])
        for t in bits[1:]:
            h = B2S[t][0]
            r.run('DIST gpc=%d consumer=%d helper=%d tpc_bit=%d' % (g, c, h, t),
                  ['--mem', 'tma', '--consume', 1, '--self', 1, '--depth', 4, '--cluster', 4,
                   '--roles', '%d:%d' % (c, h)])
        # no self-issue: ingest from the remote helper only
        for t in bits[:1] + bits[-1:]:
            h = B2S[t][1] if t == bits[0] else B2S[t][0]
            r.run('DIST noself gpc=%d consumer=%d helper=%d' % (g, c, h),
                  ['--mem', 'tma', '--consume', 1, '--self', 0, '--depth', 8, '--cluster', 2 if t == bits[0] else 4,
                   '--roles', '%d:%d' % (c, h)])


# ------------------------------------------------------------------ network (coordinator's priority 1)
REAL = [g for g in GPC_ORDER if len(GPCS[g]) > 1]
SINGLE = [g for g in GPC_ORDER if len(GPCS[g]) == 1]


def tpc_order(kind):
    """TPC bits in the order a placement takes them."""
    if kind == 'packed':
        return [t for g in REAL + SINGLE for t in GPCS[g]]
    out, k = [], 0
    groups = REAL + SINGLE
    while len(out) < len(B2S):
        for g in groups:
            if k < len(GPCS[g]):
                out.append(GPCS[g][k])
        k += 1
    return out


def placement(name, n):
    """returns (sms, bits) for n SMs"""
    if name.startswith('pair'):
        bits = tpc_order(name.split('_')[1])[:(n + 1) // 2]
        sms = sms_of_bits(bits)[:n]
    else:
        bits = tpc_order(name.split('_')[1])[:n]
        sms = [B2S[t][0] for t in bits]
    return sms, bits


def gpc_hist(bits, sms):
    h = {}
    for b in bits:
        g = gpc_of_bit(b)
        h[g] = h.get(g, 0) + sum(1 for s in B2S[b] if s in sms)
    return ','.join('g%d:%d' % (g, h[g]) for g in sorted(h))


W = ['--mem', 'tma', '--fused', 1, '--tile-kb', 32, '--depth', 6, '--cluster', 1]
# W with a 100 ns __nanosleep between mbarrier polls: builds after 04:10 got
# 139 GB/s per SM from a tight poll loop and 203 with this backoff
WB = W + ['--spin', 100]
S = ['--mem', 'tma', '--local-issuers', 3, '--tile-kb', 16, '--depth', 4, '--consume', 1, '--nw', 12, '--cluster', 1]


def runp(r, desc, bits, nsm):
    """bwsweep (the plain-load kernel of the MPS sweep) under an LD_PRELOAD mask"""
    cmd = [os.path.join(HERE, 'tpc_bwsweep'), '--gib', '8', '--blocks', str(2 * nsm), '--threads', '1024', '--unroll', '4', '--reps', '5']
    env = dict(os.environ, LD_PRELOAD=os.path.join(HERE, 'tpc_preload.so'), TPC_ENABLE_BITS=lst(bits))
    line = '# ' + desc + '\n# TPC_ENABLE_BITS=' + lst(bits) + ' ' + ' '.join(cmd) + '\n'
    if r.dry:
        sys.stdout.write(line); return
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600, env=env)
    res = p.stdout + ''.join('STDERR ' + l + '\n' for l in p.stderr.splitlines()) + 'exit=%d\n' % p.returncode
    with open(r.out, 'a') as f:
        f.write(line + res)
    sys.stdout.write(line + res); sys.stdout.flush()


def exp_network(r):
    # single SMs: one per group (per-SM ceiling everywhere)
    for g in REAL + SINGLE:
        s = B2S[GPCS[g][0]][0]
        r.run('NET W n=1 gpc=%d sm=%d' % (g, s), W + ['--roles', s])
        r.run('NET L16 n=1 gpc=%d sm=%d' % (g, s), ['--mem', 'ld', '--per-sm', 2, '--mem-sms', s])
    for n in [2, 4, 8, 16, 21, 32, 48, 64, 96, 132]:
        for pl in ['pair_packed', 'pair_spread', 'single_packed', 'single_spread']:
            if pl.startswith('single') and n > len(B2S):
                continue
            sms, bits = placement(pl, n)
            d = 'n=%d %s gpcs=%s' % (n, pl, gpc_hist(bits, sms))
            r.run('NET W ' + d, W + ['--roles', ';'.join(map(str, sms)), '--verbose', 1])
            r.run('NET S ' + d, S + ['--roles', ';'.join(map(str, sms))])
            r.run('NET L16 ' + d, ['--mem', 'ld', '--per-sm', 2, '--mem-sms', lst(sms), '--verbose', 1])
            r.run('NET L4 ' + d, ['--mem', 'ld32', '--per-sm', 2, '--mem-sms', lst(sms)])
    # GPC0 (4 TPCs) vs the four single TPCs (SMs 124-131): do they share an uplink?
    g0 = sms_of_bits(GPCS[0]); sg = sms_of_bits([GPCS[g][0] for g in SINGLE])
    for name, sms in [('gpc0', g0), ('singles', sg), ('gpc0+singles', g0 + sg)]:
        r.run('NET W %s n=%d' % (name, len(sms)), W + ['--roles', ';'.join(map(str, sms)), '--verbose', 1])
        r.run('NET L16 %s n=%d' % (name, len(sms)), ['--mem', 'ld', '--per-sm', 2, '--mem-sms', lst(sms), '--verbose', 1])


# ------------------------------------------------------------------ co-residency (coordinator's priority 2)
def exp_coresid(r):
    """MEM and CMP on the same 16 SMs (GPC1, 8 TPCs) vs disjoint splits."""
    sms = sms_of_bits(GPCS[1])
    roles = ';'.join(map(str, sms))
    mems = [
        ('W1 fused 1 thread 32KB D6', W + ['--roles', roles]),
        ('W1spin fused 1 thread 32KB D6 test_wait', W + ['--roles', roles, '--spin', 1]),
        ('W4 4 issuing warps 16KB D3 write-only', ['--mem', 'tma', '--local-issuers', 4, '--tile-kb', 16, '--depth', 3, '--consume', 0, '--nw', 4, '--roles', roles]),
        ('S12 staged 3 issuers 16KB D4 12 consumer warps', S + ['--roles', roles]),
        ('S4 staged 3 issuers 16KB D4 4 consumer warps', ['--mem', 'tma', '--local-issuers', 3, '--tile-kb', 16, '--depth', 4, '--consume', 1, '--nw', 4, '--roles', roles]),
        ('L16 plain uint4 loads 1x1024 thr', ['--mem', 'ld', '--per-sm', 1, '--mem-sms', lst(sms)]),
    ]
    for wt in [0, 256, 512, 768, 1024]:
        for name, a in mems:
            cm = [] if wt == 0 else ['--cmp-sms', lst(sms), '--cmp-threads', wt]
            if name.startswith('L16') and wt:
                cm += ['--cmp-carveout', -1]
            r.run('CORES fma_warps=%d mem=%s sms=16 gpc1' % (wt // 32, name), a + cm + ['--mb-per-consumer', 512, '--mb-per-sm', 512])
    # CMP alone with each warp count (solo rate per SM)
    for wt in [256, 512, 768, 1024]:
        r.run('CORES cmp_only fma_warps=%d sms=16' % (wt // 32), ['--cmp-sms', lst(sms), '--cmp-threads', wt, '--cmp-solo-ms', 60])
    # disjoint space sharing inside the same 16 SMs: x MEM SMs (both SMs of x/2 TPCs), 16-x CMP SMs
    for x in [2, 4, 8, 12]:
        ms = sms[:x]; cs = sms[x:]
        r.run('DISJ x=%d mem=W1 cmp=%d SMs fma_warps=32' % (x, 16 - x), W + ['--roles', ';'.join(map(str, ms)), '--cmp-sms', lst(cs)])
        r.run('DISJ x=%d mem=L16 cmp=%d SMs fma_warps=32' % (x, 16 - x), ['--mem', 'ld', '--per-sm', 2, '--mem-sms', lst(ms), '--cmp-sms', lst(cs), '--cmp-carveout', -1])


def exp_coresid2(r):
    """Co-residency with MEM launched first and FMA at ILP 2 (MEM is not starved
    there), vs FMA warps; then the whole chip: shared SMs vs disjoint splits."""
    sms = sms_of_bits(GPCS[1])
    roles = ';'.join(map(str, sms))
    big = ['--mb-per-consumer', 2048, '--mb-per-sm', 2048]
    for wt in [256, 512, 768, 1024]:
        for mf, ilp in [(1, 2), (1, 4), (0, 2)]:
            cm = ['--cmp-sms', lst(sms), '--cmp-threads', wt, '--cmp-ilp', ilp, '--mem-first', mf]
            r.run('CORES2 gpc1 fma_warps=%d ilp=%d mem_first=%d mem=W1' % (wt // 32, ilp, mf), W + ['--roles', roles] + cm + big)
            if mf == 1 and ilp == 2:
                r.run('CORES2 gpc1 fma_warps=%d ilp=%d mem_first=%d mem=W1 backoff500' % (wt // 32, ilp, mf), W + ['--roles', roles, '--spin', 500] + cm + big)
                r.run('CORES2 gpc1 fma_warps=%d ilp=%d mem_first=%d mem=S12' % (wt // 32, ilp, mf), S + ['--roles', roles] + cm + big)
                r.run('CORES2 gpc1 fma_warps=%d ilp=%d mem_first=%d mem=L16' % (wt // 32, ilp, mf),
                      ['--mem', 'ld', '--per-sm', 1, '--mem-sms', lst(sms), '--cmp-carveout', -1] + cm + big)
    # whole chip. CMP alone on all 132 SMs is the reference for every ratio below.
    for ilp in [2, 4]:
        r.run('CHIP cmp_only all132 ilp=%d' % ilp, ['--cmp-sms', lst(ALL_SMS), '--cmp-ilp', ilp, '--cmp-solo-ms', 60])
    allr = ';'.join(map(str, ALL_SMS))
    for ilp in [2, 4]:
        r.run('CHIP shared all132 mem=W1 mem_first=1 ilp=%d' % ilp,
              W + ['--roles', allr, '--cmp-sms', lst(ALL_SMS), '--cmp-ilp', ilp, '--mem-first', 1, '--mb-per-consumer', 256])
    for n in [8, 16, 24, 32, 48]:
        msms, _ = placement('single_spread', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        for ilp in [2, 4]:
            r.run('CHIP disjoint mem=W1 n=%d single_spread cmp=%d ilp=%d' % (n, len(rest), ilp),
                  W + ['--roles', ';'.join(map(str, msms)), '--cmp-sms', lst(rest), '--cmp-ilp', ilp, '--mb-per-consumer', 1024])
        msms, _ = placement('pair_spread', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        r.run('CHIP disjoint mem=W1 n=%d pair_spread cmp=%d ilp=4' % (n, len(rest)),
              W + ['--roles', ';'.join(map(str, msms)), '--cmp-sms', lst(rest), '--cmp-ilp', 4, '--mb-per-consumer', 1024])
        msms, _ = placement('pair_packed', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        r.run('CHIP disjoint mem=W1 n=%d pair_packed cmp=%d ilp=4' % (n, len(rest)),
              W + ['--roles', ';'.join(map(str, msms)), '--cmp-sms', lst(rest), '--cmp-ilp', 4, '--mb-per-consumer', 1024])


def exp_chip(r):
    W = WB
    for n in [1, 8, 16, 32]:  # recheck of the network sweep's W numbers with this build
        msms, _ = placement('single_spread', n)
        r.run('CHIP check nocmp mem=WB n=%d single_spread' % n, W + ['--roles', ';'.join(map(str, msms))])
    for n in [16, 24, 32]:
        msms, _ = placement('single_spread', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        r.run('CHIP disjoint mem=L16 n=%d single_spread cmp=%d ilp=4' % (n, len(rest)),
              ['--mem', 'ld', '--per-sm', 2, '--mem-sms', lst(msms), '--cmp-sms', lst(rest), '--cmp-carveout', -1])
    # whole chip (rerun with WB). CMP alone on all 132 SMs is the reference for every ratio below.
    for ilp in [2, 4]:
        r.run('CHIP cmp_only all132 ilp=%d' % ilp, ['--cmp-sms', lst(ALL_SMS), '--cmp-ilp', ilp, '--cmp-solo-ms', 60])
    allr = ';'.join(map(str, ALL_SMS))
    for ilp in [2, 4]:
        r.run('CHIP shared all132 mem=W1 mem_first=1 ilp=%d' % ilp,
              W + ['--roles', allr, '--cmp-sms', lst(ALL_SMS), '--cmp-ilp', ilp, '--mem-first', 1, '--mb-per-consumer', 256])
    for n in [8, 16, 24, 32, 48]:
        msms, _ = placement('single_spread', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        for ilp in [2, 4]:
            r.run('CHIP disjoint mem=W1 n=%d single_spread cmp=%d ilp=%d' % (n, len(rest), ilp),
                  W + ['--roles', ';'.join(map(str, msms)), '--cmp-sms', lst(rest), '--cmp-ilp', ilp, '--mb-per-consumer', 1024])
        msms, _ = placement('pair_spread', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        r.run('CHIP disjoint mem=W1 n=%d pair_spread cmp=%d ilp=4' % (n, len(rest)),
              W + ['--roles', ';'.join(map(str, msms)), '--cmp-sms', lst(rest), '--cmp-ilp', 4, '--mb-per-consumer', 1024])
        msms, _ = placement('pair_packed', n)
        rest = [x for x in ALL_SMS if x not in set(msms)]
        r.run('CHIP disjoint mem=W1 n=%d pair_packed cmp=%d ilp=4' % (n, len(rest)),
              W + ['--roles', ';'.join(map(str, msms)), '--cmp-sms', lst(rest), '--cmp-ilp', 4, '--mb-per-consumer', 1024])


def exp_borrow2(r):
    """Short borrowing check (coordinator's priority 3): consumer SM 12 (GPC6,
    first TPC) fed by helper SMs via cp.async.bulk.shared::cluster; helper in
    the same TPC vs in each other TPC of the GPC; k = 1, 3, 7 helpers."""
    g = GPCS[6]
    c = B2S[g[0]][0]
    base = ['--mem', 'tma', '--tile-kb', 16, '--spin', 100]
    for consume in [0, 1]:
        r.run('BOR k=1 same_tpc helper=%d self=0 consume=%d' % (B2S[g[0]][1], consume),
              base + ['--self', 0, '--depth', 12, '--consume', consume, '--cluster', 2, '--roles', '%d:%d' % (c, B2S[g[0]][1])])
        for t in g[1:]:
            h = B2S[t][0]
            r.run('BOR k=1 other_tpc bit=%d helper=%d self=0 consume=%d' % (t, h, consume),
                  base + ['--self', 0, '--depth', 12, '--consume', consume, '--cluster', 4, '--roles', '%d:%d' % (c, h)])
    for k, D, C in [(1, 6, 2), (3, 3, 4), (7, 1, 8)]:
        # helpers: the sibling SM, then SMs of the following TPCs; mask = TPCs used
        hs = [B2S[g[0]][1]] + [x for t in g[1:] for x in B2S[t]]
        hs = hs[:k]
        ntpc = (1 + k + 1) // 2
        C = 2 * ntpc
        for self_ in [0, 1]:
            Dk = max(1, 13 // (k + self_))
            r.run('BOR k=%d self=%d depth=%d nearest helpers %s' % (k, self_, Dk, hs),
                  base + ['--self', self_, '--depth', Dk, '--consume', 0, '--cluster', C, '--roles', '%d:%s' % (c, lst(hs))])
    # k=3 with CMP (FMA) on the helper SMs
    hs = [B2S[g[0]][1]] + B2S[g[1]]
    for ilp, mf in [(4, 0), (2, 1)]:
        r.run('BOR k=3 self=1 with CMP on helpers ilp=%d mem_first=%d' % (ilp, mf),
              base + ['--self', 1, '--depth', 3, '--consume', 0, '--cluster', 4, '--roles', '%d:%s' % (c, lst(hs)),
                      '--cmp-sms', lst(hs), '--cmp-ilp', ilp, '--mem-first', mf])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exp')
    ap.add_argument('--out')
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--dry', action='store_true')
    a = ap.parse_args()
    out = a.out or os.path.join(RES, 'h200_tpc_%s.txt' % a.exp)
    r = Runner(out, a.reps, a.dry)
    if a.exp == 'baseline':
        exp_baseline(r)
    elif a.exp == 'gpcfill':
        exp_gpcfill(r)
    elif a.exp == 'borrow':
        exp_borrow(r, True)
    elif a.exp == 'borrow_nocmp':
        exp_borrow(r, False, tag='C2up')
    elif a.exp == 'corun':
        exp_corun(r)
    elif a.exp == 'borrow2':
        exp_borrow2(r)
    elif a.exp == 'chip':
        exp_chip(r)
    elif a.exp == 'coresid2':
        exp_coresid2(r)
    elif a.exp == 'coresid':
        exp_coresid(r)
    elif a.exp == 'network':
        exp_network(r)
    elif a.exp == 'distance':
        exp_distance(r)
    else:
        sys.exit('unknown experiment')


if __name__ == '__main__':
    main()
