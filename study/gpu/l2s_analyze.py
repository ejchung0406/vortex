#!/usr/bin/env python3
"""Analysis of l2s_lab latency scans (gpu/l2s_lat/*.u16).

  python3 l2s_analyze.py lat        classes per SM/condition, GPC sides,
                                    interleave, GF(2) fit of the partition bit
  python3 l2s_analyze.py lists      block lists (4 KB blocks) local/remote per side
"""
import glob, os, re, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
LATDIR = os.path.join(HERE, 'l2s_lat')
LISTS = os.path.join(HERE, 'l2s_lists')


def gmm1d(x, k, iters=200):
    x = x.astype(float)
    q = np.quantile(x, np.linspace(0.05, 0.95, k))
    mu, sd, w = q.copy(), np.full(k, max(x.std() / k, 1.0)), np.full(k, 1.0 / k)
    for _ in range(iters):
        p = w * np.exp(-0.5 * ((x[:, None] - mu) / sd) ** 2) / sd
        r = p / np.maximum(p.sum(1, keepdims=True), 1e-300)
        n = r.sum(0) + 1e-9
        mu = (r * x[:, None]).sum(0) / n
        sd = np.sqrt((r * (x[:, None] - mu) ** 2).sum(0) / n) + 0.5
        w = n / n.sum()
    ll = np.log(np.maximum((w * np.exp(-0.5 * ((x[:, None] - mu) / sd) ** 2) / (sd * np.sqrt(2 * np.pi))).sum(1), 1e-300)).sum()
    bic = -2 * ll + (3 * k - 1) * np.log(len(x))
    o = np.argsort(mu)
    return mu[o], sd[o], w[o], bic


def best_gmm(x, kmax=4):
    clip = x[x < np.quantile(x, 0.999)]
    res = [gmm1d(clip, k) for k in range(1, kmax + 1)]
    b = int(np.argmin([r[3] for r in res]))
    return b + 1, res[b], res


def two_class(x):
    """threshold between the two largest modes (GMM k=2 on clipped data)"""
    clip = x[x < np.quantile(x, 0.999)]
    mu, sd, w, _ = gmm1d(clip, 2)
    thr = (mu[0] * sd[1] + mu[1] * sd[0]) / (sd[0] + sd[1])
    return (x > thr).astype(np.uint8), mu, w, thr


def load(name):
    return np.fromfile(os.path.join(LATDIR, name), dtype=np.uint16)


def runs(c):
    """lengths of runs of equal class"""
    d = np.flatnonzero(np.diff(c.astype(int))) + 1
    edges = np.concatenate([[0], d, [len(c)]])
    return np.diff(edges)


def gf2_fit(cls, line_bytes, lo, hi, page_bits=21):
    """find a set S of address bits (lo..hi-1) and per-page constants with
    cls(addr) = XOR_{b in S} addr_b XOR const(page). Least-error search by
    solving over GF(2) with Gaussian elimination on the full system."""
    n = len(cls)
    addr = np.arange(n, dtype=np.int64) * line_bytes
    bits = [b for b in range(lo, hi)]
    page = addr >> page_bits
    npages = int(page.max()) + 1
    cols = [((addr >> b) & 1).astype(np.uint8) for b in bits] + [(page == p).astype(np.uint8) for p in range(npages)]
    A = np.stack(cols, 1)
    y = cls.astype(np.uint8)
    M = np.concatenate([A, y[:, None]], 1).copy()
    r, piv = 0, []
    for c in range(A.shape[1]):
        rows = np.flatnonzero(M[r:, c]) + r
        if len(rows) == 0:
            continue
        M[[r, rows[0]]] = M[[rows[0], r]]
        others = np.flatnonzero(M[:, c]); others = others[others != r]
        M[others] ^= M[r]
        piv.append(c); r += 1
        if r == M.shape[0]:
            break
    sol = np.zeros(A.shape[1], np.uint8)
    for i, c in enumerate(piv):
        sol[c] = M[i, -1]
    pred = (A.astype(np.int64) @ sol.astype(np.int64)) % 2
    err = float((pred != y).mean())
    used = [bits[i] for i in range(len(bits)) if sol[i]]
    return used, err


def cmd_lat():
    files = sorted(glob.glob(os.path.join(LATDIR, 'fine_*.u16')))
    data = {}
    for f in files:
        m = re.match(r'fine_sm(\d+)_warm(\w+)\.u16', os.path.basename(f))
        data[(int(m.group(1)), m.group(2))] = np.fromfile(f, dtype=np.uint16).astype(np.int32)
    sms = sorted(set(k[0] for k in data))
    print('== latency classes per SM and condition (1-D GMM, best k by BIC; means in SM cycles, weights)')
    for s in sms:
        for (ss, w), x in sorted(data.items()):
            if ss != s:
                continue
            k, (mu, sd, wt, _), _ = best_gmm(x)
            q = np.percentile(x, [1, 5, 25, 50, 75, 95, 99])
            print('sm=%3d warm=%-5s n=%d k=%d means=%s sd=%s weights=%s pct1/5/25/50/75/95/99=%s' % (
                s, w, len(x), k, np.round(mu).astype(int).tolist(), np.round(sd).astype(int).tolist(),
                np.round(wt, 3).tolist(), q.astype(int).tolist()))
    # cold classes (home partition relative to each SM)
    print('\n== cold (DRAM miss) two-class split per SM')
    cold = {}
    for s in sms:
        if (s, 'none') not in data:
            continue
        c, mu, w, thr = two_class(data[(s, 'none')])
        cold[s] = c
        r = runs(c)
        print('sm=%3d near=%d far=%d (cycles) thr=%d far_fraction=%.3f run_len_lines min=%d median=%d max=%d (x128 B)' % (
            s, mu[0], mu[1], thr, c.mean(), r.min(), np.median(r), r.max()))
    ks = sorted(cold)
    print('\n== agreement of cold far-masks between SMs (fraction of lines with the same class)')
    print('      ' + ' '.join('%6d' % k for k in ks))
    for a in ks:
        print('%5d ' % a + ' '.join('%6.3f' % (cold[a] == cold[b]).mean() for b in ks))
    # interleave and GF(2) fit on the first SM
    s0 = ks[0]
    c = cold[s0]
    r = runs(c)
    hist = np.bincount(r)
    print('\n== run lengths of the class along addresses (sm %d), in 128 B lines: %s' % (
        s0, {int(i): int(v) for i, v in enumerate(hist) if v}))
    for lo, hi in [(7, 21), (7, 23)]:
        used, err = gf2_fit(c, 128, lo, hi)
        print('GF(2) fit bits %d..%d with per-2MB-page constant: bits=%s error=%.4f' % (lo, hi - 1, used, err))
    # hit latency after warm by another SM: classes
    print('\n== L2 hit after warm by another SM: per-line class vs that SM\'s cold class')
    for (s, w), x in sorted(data.items()):
        if w in ('none', 'self'):
            continue
        ch, mu, wt, thr = two_class(x)
        if s in cold:
            print('sm=%3d warm=%s hit near=%d far=%d far_fraction=%.3f agree_with_own_cold_class=%.3f' % (
                s, w, mu[0], mu[1], ch.mean(), (ch == cold[s]).mean()))
    np.save(os.path.join(LATDIR, 'cold_classes.npy'), np.stack([cold[k] for k in ks]))
    with open(os.path.join(LATDIR, 'cold_classes_sms.txt'), 'w') as f:
        f.write(' '.join(map(str, ks)) + '\n')
    # sectors
    for f in sorted(glob.glob(os.path.join(LATDIR, 'sect_*.u16'))):
        x = np.fromfile(f, dtype=np.uint16).astype(int)
        st = int(re.search(r'_s(\d+)_', f).group(1))
        per = 128 // st
        x = x[: len(x) // per * per].reshape(-1, per)
        print('%s: mean latency by position in the 128 B line: %s' % (os.path.basename(f), np.round(x.mean(0)).astype(int).tolist()))
    # block probe
    for f in sorted(glob.glob(os.path.join(LATDIR, 'blk4k_*.u16'))):
        x = np.fromfile(f, dtype=np.uint16).astype(int)
        c, mu, w, thr = two_class(x)
        np.save(f.replace('.u16', '_class.npy'), c)
        r = runs(c)
        print('%s: n=%d near=%d far=%d far_fraction=%.3f run lengths (4KB blocks) median=%d max=%d' % (
            os.path.basename(f), len(x), mu[0], mu[1], c.mean(), np.median(r), r.max()))
        used, err = gf2_fit(c[: 1 << 16], 4096, 12, 21)
        print('   GF(2) fit bits 12..20 + page constant over first 256 MB: bits=%s error=%.4f' % (used, err))
    bl = sorted(glob.glob(os.path.join(LATDIR, 'blk4k_*_class.npy')))
    if len(bl) == 2:
        a, b = np.load(bl[0]), np.load(bl[1])
        print('block classes %s vs %s: equal=%.4f complementary=%.4f' % (os.path.basename(bl[0]), os.path.basename(bl[1]), (a == b).mean(), (a != b).mean()))


def cmd_lists():
    os.makedirs(LISTS, exist_ok=True)
    bl = sorted(glob.glob(os.path.join(LATDIR, 'blk4k_*_class.npy')))
    for f in bl:
        sm = re.search(r'sm(\d+)', f).group(1)
        c = np.load(f)
        near = np.flatnonzero(c == 0).astype(np.uint32)
        far = np.flatnonzero(c == 1).astype(np.uint32)
        near.tofile(os.path.join(LISTS, 'near_sm%s.u32' % sm)); far.tofile(os.path.join(LISTS, 'far_sm%s.u32' % sm))
        print('sm %s: near blocks %d, far blocks %d' % (sm, len(near), len(far)))


if __name__ == '__main__':
    {'lat': cmd_lat, 'lists': cmd_lists}[sys.argv[1]]()
