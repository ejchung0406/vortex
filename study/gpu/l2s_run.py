#!/usr/bin/env python3
"""L2-structure experiments on the H200 (run inside the l2struct_h200 holder
via tpc_srun.sh with TPC_JOB set). Phases:

  env        nvidia-smi, clock-lock attempt, ncu device attributes
  lat        latency scans (fine 8 MB window per GPC, conditions; sector scan;
             1 line per 4 KB over 7 GiB from two SMs)
  bw_*       bandwidth phases (need lists from l2s_analyze.py)
  mix        Part 2

Every tpc/l2s output line is appended to results_gpu/h200_l2s_<phase>.txt.
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, '..', 'results_gpu')
LAB = os.path.join(HERE, 'l2s_lab')
MAP = os.environ.get('L2S_MAP', os.path.join(RES, 'h200_l2s_map.txt'))
LISTS = os.path.join(HERE, 'l2s_lists')
LATDIR = os.path.join(HERE, 'l2s_lat')


def log(phase, text):
    with open(os.path.join(RES, 'h200_l2s_%s.txt' % phase), 'a') as f:
        f.write(text)
    sys.stdout.write(text); sys.stdout.flush()


def run(phase, desc, args, timeout=900):
    cmd = [LAB] + [str(a) for a in args]
    if args[0] != 'raw':
        cmd.insert(2, '--map'); cmd.insert(3, MAP)
    else:
        cmd = [str(a) for a in args[1:]]
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    out = '# %s\n# %s\n%s%sexit=%d secs=%.1f\n' % (desc, ' '.join(cmd), p.stdout,
                                                  ''.join('STDERR ' + l + '\n' for l in p.stderr.splitlines()[-5:]),
                                                  p.returncode, time.time() - t0)
    log(phase, out)
    return p.stdout


def load_map():
    b2s = {}
    for ln in open(MAP):
        f = ln.split()
        if f[:1] == ['bit']:
            b2s[int(f[1])] = [int(x) for x in f[3:]]
    return b2s


def gpcs():
    """GPC groups from h200_l2s_gpc.txt (physical: lone TPCs folded into GPC 0)"""
    g = []
    for ln in open(os.path.join(RES, 'h200_l2s_gpc.txt')):
        f = ln.split()
        if f[:1] == ['gpc']:
            g.append([int(x) for x in f[3:]])
    return g


def lst(v):
    return ','.join(str(x) for x in v)


def phase_env():
    for c in ['nvidia-smi -q -d CLOCK,POWER,PERFORMANCE', 'nvidia-smi -lgc 1980,1980', 'nvidia-smi -ac 3201,1980',
              'nvidia-smi --query-gpu=name,serial,uuid,pci.bus_id,driver_version,clocks.max.sm,clocks.max.mem --format=csv']:
        p = subprocess.run(c.split(), capture_output=True, text=True)
        log('env', '### %s (exit %d)\n%s%s\n' % (c, p.returncode, p.stdout[-4000:], p.stderr[-2000:]))
    attrs = ['device__attribute_l2s_count', 'device__attribute_num_l2s_per_fbp', 'device__attribute_fbp_count',
             'device__attribute_max_gpu_frequency_khz', 'device__attribute_max_mem_frequency_khz',
             'device__attribute_l2_cache_size', 'device__attribute_multiprocessor_count',
             'device__attribute_dram_bandwidth', 'device__attribute_fb_bus_width', 'device__attribute_limits_max_cta_per_sm',
             'device__attribute_num_tpc_per_gpc', 'device__attribute_num_gpcs', 'device__attribute_num_tpcs',
             'device__attribute_ltc_count', 'device__attribute_num_ltc', 'device__attribute_num_fbpas',
             'device__attribute_fbpa_count', 'device__attribute_display_name', 'device__attribute_chip_name']
    cmd = ['ncu', '--metrics', ','.join(attrs), '--print-units', 'base', LAB, 'lat', '--map', MAP, '--sm', '0', '--count', '64', '--gib', '2', '--flush', '0']
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    log('env', '### %s (exit %d)\n%s%s\n' % (' '.join(cmd), p.returncode, p.stdout[-8000:], p.stderr[-3000:]))
    # instance counts of L2 slices / FBPA via per-instance metric values
    cmd = ['ncu', '--metrics', 'lts__t_sectors.sum,fbpa__dram_read_sectors.sum,ltc__t_sectors.sum', '--print-metric-instances', 'values',
           LAB, 'lat', '--map', MAP, '--sm', '0', '--count', '64', '--gib', '2', '--flush', '0']
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    log('env', '### %s (exit %d)\n%s%s\n' % (' '.join(cmd), p.returncode, p.stdout[-8000:], p.stderr[-3000:]))


def phase_lat():
    os.makedirs(LATDIR, exist_ok=True)
    G = gpcs(); B = load_map()
    sms = [B[g[0]][0] for g in G] + [B[g[-1]][0] for g in G[:1]]  # first SM of each GPC + a lone-TPC SM of GPC 0
    win = 8 << 20
    warmers = [sms[0], sms[-2]]  # GPC 0 and GPC 7
    for s in sms:
        for w in ['none', 'self'] + [str(x) for x in warmers if x != s]:
            out = os.path.join(LATDIR, 'fine_sm%d_warm%s.u16' % (s, w))
            run('lat', 'fine sm=%d warm=%s window=8MB stride=128' % (s, w),
                ['lat', '--sm', s, '--warm', w, '--off0', 0, '--stride', 128, '--count', win // 128, '--gib', 8, '--out', out])
    # sector-level: 32 B and 64 B steps over 256 KB, cold and self-warm
    for st in [32, 64]:
        for w in ['none', 'self']:
            out = os.path.join(LATDIR, 'sect_sm%d_s%d_warm%s.u16' % (sms[0], st, w))
            run('lat', 'sector sm=%d stride=%d warm=%s' % (sms[0], st, w),
                ['lat', '--sm', sms[0], '--warm', w, '--off0', 16 << 20, '--stride', st, '--count', (256 << 10) // st, '--gib', 8, '--out', out])
    # one line per 4 KB over 7 GiB, cold, from GPC 0 and GPC 7 (the bandwidth lists come from this)
    for s in warmers:
        out = os.path.join(LATDIR, 'blk4k_sm%d.u16' % s)
        run('lat', 'blocks sm=%d one line per 4KB over 7GiB cold' % s,
            ['lat', '--sm', s, '--warm', 'none', '--off0', 0, '--stride', 4096, '--count', (7 << 30) // 4096, '--gib', 8, '--out', out], timeout=1200)


# ------------------------------------------------------------------ bandwidth phases
import numpy as np


def sides():
    """side -> list of GPC indices, from results_gpu/h200_l2s_sides.txt"""
    d = {}
    for ln in open(os.path.join(RES, 'h200_l2s_sides.txt')):
        f = ln.split()
        if f[:1] == ['side']:
            d[f[1]] = [int(x) for x in f[3:]]
    return d


def lpath(name):
    return os.path.join(LISTS, name)


def gpc_sms(g, n=None):
    B = load_map(); G = gpcs()
    s = [x for b in G[g] for x in B[b]]
    return s if n is None else s[:n]


def phase_bw_gpc():
    """same-GPC sweep: DRAM/L2, disjoint/shared, read/write (coordinator item 2)"""
    for g in [6, 1]:
        allsm = gpc_sms(g)
        for n in [1, 2, 3, 4, 5, 6, 8, 10, 12, 14, 16, 18]:
            if n > len(allsm):
                continue
            sms = lst(allsm[:n]); units = 2 * n
            d = 'GPC%d n=%d' % (g, n)
            run('bw_gpc', d + ' DRAM read disjoint 128MB/unit', ['bw', '--groups', '%s@all:%d' % (sms, units * 32768), '--per-sm', 2, '--gib', 24])
            run('bw_gpc', d + ' DRAM write disjoint 128MB/unit', ['bw', '--groups', '%s@all:%d' % (sms, units * 32768), '--per-sm', 2, '--write', 1, '--gib', 24])
            run('bw_gpc', d + ' L2 read disjoint 256KB/unit x400', ['bw', '--groups', '%s@all:%d' % (sms, units * 64), '--per-sm', 2, '--passes', 400, '--gib', 8])
            run('bw_gpc', d + ' L2 write disjoint 256KB/unit x400', ['bw', '--groups', '%s@all:%d' % (sms, units * 64), '--per-sm', 2, '--passes', 400, '--write', 1, '--gib', 8])
            run('bw_gpc', d + ' L2 read shared 256KB x400', ['bw', '--groups', '%s@all:64' % sms, '--per-sm', 2, '--passes', 400, '--shared', 1, '--gib', 8])
            run('bw_gpc', d + ' L2 read shared 4MB x25', ['bw', '--groups', '%s@all:1024' % sms, '--per-sm', 2, '--passes', 25, '--shared', 1, '--gib', 8])
    # one SM per GPC, 8 GPCs (different-GPC reference of Fan Fig. 2a)
    B = load_map(); G = gpcs()
    for k in [1, 2, 4, 8]:
        sms = lst([B[G[g][0]][0] for g in range(k)])
        run('bw_gpc', 'one SM in each of %d GPCs L2 read disjoint 256KB/unit x400' % k, ['bw', '--groups', '%s@all:%d' % (sms, 2 * k * 64), '--per-sm', 2, '--passes', 400, '--gib', 8])
        run('bw_gpc', 'one SM in each of %d GPCs L2 read shared 256KB x400' % k, ['bw', '--groups', '%s@all:64' % sms, '--per-sm', 2, '--passes', 400, '--shared', 1, '--gib', 8])


def phase_bw_side():
    """local-only / remote-only / mixed: per SM, per GPC, per side, chip (items e, 3)"""
    S = sides(); B = load_map(); G = gpcs()
    near = {sd: lpath('near_side%s.u32' % sd) for sd in S}
    far = {sd: lpath('far_side%s.u32' % sd) for sd in S}
    side_of = {g: sd for sd, gs in S.items() for g in gs}
    def grp(sms_by_side, kind):
        parts = []
        for sd, sms in sms_by_side.items():
            if not sms:
                continue
            l = {'local': near[sd], 'remote': far[sd]}.get(kind, 'all:%d' % ((7 << 30) // 4096))
            parts.append('%s@%s' % (lst(sms), l))
        return ';'.join(parts) if kind != 'all' else '%s@all:%d' % (lst([x for v in sms_by_side.values() for x in v]), (7 << 30) // 4096)
    cases = []
    for g in [0, 1, 6]:
        cases.append(('one SM gpc%d' % g, {side_of[g]: [B[G[g][0]][0]]}))
    for g in [1, 6]:
        cases.append(('GPC%d 6 SMs' % g, {side_of[g]: gpc_sms(g, 6)}))
        cases.append(('GPC%d all SMs' % g, {side_of[g]: gpc_sms(g)}))
    for sd, gs in S.items():
        cases.append(('side %s all SMs' % sd, {sd: [x for g in gs for x in gpc_sms(g)]}))
    for per in [2, 3, 4, 6]:
        cases.append(('spread %d SMs per GPC (8 GPCs)' % per, {sd: [x for g in gs for x in gpc_sms(g)[0:2 * per:2]] for sd, gs in S.items()}))
    cases.append(('whole chip', {sd: [x for g in gs for x in gpc_sms(g)] for sd, gs in S.items()}))
    for name, sbs in cases:
        for kind in ['local', 'remote', 'all']:
            run('bw_side', '%s reading %s' % (name, kind), ['bw', '--groups', grp(sbs, kind), '--per-sm', 2, '--gib', 8, '--block', 4096])


def phase_bw_slice():
    """slice / channel probes (item d, f)"""
    S = sides(); G = gpcs(); B = load_map()
    sdA = sorted(S)[0]
    nearA = np.fromfile(lpath('near_side%s.u32' % sdA), dtype=np.uint32)
    b0 = int(nearA[0])  # a 4 KB block that is near for side A
    one_per_gpc = [B[G[g][0]][0] for g in S[sdA]]
    four_per_gpc = [x for g in S[sdA] for x in gpc_sms(g)[0:8:2]]
    base_line = b0 * 32  # in 128 B lines
    def lines(bits):
        L = [base_line]
        for b in bits:
            L = L + [x ^ (1 << (b - 7)) for x in L]
        return L
    sets = [('1 line', []), ('bit7', [7]), ('bit8', [8]), ('bit9', [9]), ('bit10', [10]), ('bit11', [11]),
            ('bits8-9', [8, 9]), ('bits8-11', [8, 9, 10, 11]), ('bits7-11 (one 4KB block)', [7, 8, 9, 10, 11])]
    os.makedirs(LISTS, exist_ok=True)
    for name, bits in sets:
        L = np.array(sorted(lines(bits)), dtype=np.uint32)
        L = np.tile(L, 8192 // len(L))  # 8192 entries (1 MB per pass), footprint = the set
        f = lpath('lines_%s.u32' % name.split()[0]); L.tofile(f)
        for label, sms in [('1 SM/GPC side %s' % sdA, one_per_gpc), ('4 SMs/GPC side %s' % sdA, four_per_gpc)]:
            run('bw_slice', 'L2 hammer %s (%d distinct lines) from %s' % (name, 2 ** len(bits), label),
                ['bw', '--groups', '%s@%s' % (lst(sms), f), '--block', 128, '--per-sm', 1, '--passes', 100, '--shared', 1, '--gib', 2])
    # many near 4 KB blocks: 256 KB and 2 MB of near data, shared
    for nb in [64, 512]:
        f = lpath('nearA_%d.u32' % nb); nearA[:nb].tofile(f)
        for label, sms in [('1 SM/GPC side %s' % sdA, one_per_gpc), ('4 SMs/GPC side %s' % sdA, four_per_gpc)]:
            run('bw_slice', 'L2 hammer %d near 4KB blocks from %s' % (nb, label),
                ['bw', '--groups', '%s@%s' % (lst(sms), f), '--block', 4096, '--per-sm', 1, '--passes', 25600 // nb, '--shared', 1, '--gib', 2])
    # DRAM: 256 B granules with fixed address bits, all 132 SMs, disjoint
    allsm = [x for g in range(len(G)) for x in gpc_sms(g)]
    ng = (7 << 30) // 256
    gi = np.arange(ng, dtype=np.uint64)
    filters = [('all', None), ('bit8=0', (8, 1)), ('bits8-9=0', (8, 3)), ('bits8-11=0', (8, 15)), ('bits9-11=0', (9, 7)),
               ('bits10-11=0', (10, 3)), ('bits12-15=0', (12, 15)), ('bits16-19=0', (16, 15)), ('bits8-15=0', (8, 255))]
    for name, flt in filters:
        if flt is None:
            sel = gi
        else:
            lo, m = flt
            sel = gi[(((gi * 256) >> lo) & m) == 0]
        f = lpath('gran_%s.u32' % name); sel.astype(np.uint32).tofile(f)
        run('bw_slice', 'DRAM 256B granules %s (%d granules) all SMs disjoint' % (name, len(sel)),
            ['bw', '--groups', '%s@%s' % (lst(allsm), f), '--block', 256, '--per-sm', 2, '--gib', 8])


def phase_mix():
    """Part 2: shared memory vs SM ingress, SM 0 (GPC 0)"""
    S = sides(); sdA = sorted(S)[0]
    sm = 0
    nearA, farA = lpath('near_side%s.u32' % sdA), lpath('far_side%s.u32' % sdA)
    base = ['mix', '--sm', sm, '--dur-ms', 20, '--gib', 24]
    runs = [
        ('(i) TMA alone 32KBx6', ['--tma', 1, '--tile-kb', 32, '--depth', 6]),
        ('(i) TMA alone 2 issuers 32KBx3', ['--tma', 2, '--tile-kb', 32, '--depth', 3]),
        ('(ii) loads alone 32 warps default carveout', ['--ld', 32]),
        ('(ii) loads alone 24 warps default carveout', ['--ld', 24]),
        ('(ii) loads alone 32 warps carveout 100', ['--ld', 32, '--carveout', 100]),
        ('(ii) loads alone 24 warps carveout 100', ['--ld', 24, '--carveout', 100]),
        ('(iii) same CTA: TMA 32KBx6 + 24 load warps', ['--tma', 1, '--tile-kb', 32, '--depth', 6, '--ld', 24]),
        ('(iii) same CTA: TMA 32KBx6 + 16 load warps', ['--tma', 1, '--tile-kb', 32, '--depth', 6, '--ld', 16]),
        ('(iii) two CTAs: TMA 32KBx6 kernel + 24-warp load kernel', ['--tma', 1, '--tile-kb', 32, '--depth', 6, '--second-ld', 24]),
        ('(iii) two CTAs: TMA 32KBx6 kernel + 30-warp load kernel', ['--tma', 1, '--tile-kb', 32, '--depth', 6, '--second-ld', 30]),
        ('(iv) smem readers alone 4 warps', ['--smr', 4]),
        ('(iv) smem readers alone 8 warps', ['--smr', 8]),
        ('(iv) TMA 32KBx5 alone', ['--tma', 1, '--tile-kb', 32, '--depth', 5]),
        ('(iv) TMA 32KBx5 + 2 smem reader warps', ['--tma', 1, '--tile-kb', 32, '--depth', 5, '--smr', 2]),
        ('(iv) TMA 32KBx5 + 3 smem reader warps', ['--tma', 1, '--tile-kb', 32, '--depth', 5, '--smr', 3]),
        ('(iv) smem readers alone 2 warps', ['--smr', 2]),
        ('(iv) smem readers alone 3 warps', ['--smr', 3]),
        ('near: TMA 4 issuers 4KBx12', ['--tma', 4, '--tile-kb', 4, '--depth', 12, '--block', 4096, '--list', nearA]),
        ('far: TMA 4 issuers 4KBx12', ['--tma', 4, '--tile-kb', 4, '--depth', 12, '--block', 4096, '--list', farA]),
        ('all: TMA 4 issuers 4KBx12', ['--tma', 4, '--tile-kb', 4, '--depth', 12, '--block', 4096]),
        ('near: loads 32 warps', ['--ld', 32, '--block', 4096, '--list', nearA]),
        ('far: loads 32 warps', ['--ld', 32, '--block', 4096, '--list', farA]),
        ('all: loads 32 warps 4KB blocks', ['--ld', 32, '--block', 4096]),
    ]
    for d, a in runs:
        run('mix', 'Part2 sm=%d %s' % (sm, d), base + a)


if __name__ == '__main__':
    ph = sys.argv[1]
    globals()['phase_' + ph]()
