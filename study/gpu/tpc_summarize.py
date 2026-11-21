#!/usr/bin/env python3
"""Summarise results_gpu/h200_tpc_*.txt (tpc_partc.py output): one row per
run with the description and the key fields of the tpc_lab result line.

  python3 tpc_summarize.py FILE [--fields a,b,c] [--grep TEXT]
"""
import re, sys

DEF = ['mem_gbs', 'mem_gbs_range', 'per_unit_med', 'slowest', 'fastest', 'cmp_gflops', 'cmp_solo_gflops', 'cmp_ratio', 'check', 'issuers_max', 'mem_cta_sms', 'cmp_cov_sms']


def parse(path):
    desc, out = None, []
    for ln in open(path):
        ln = ln.rstrip('\n')
        if ln.startswith('# ') and not ln.startswith('# /') and not ln.startswith('# TPC_'):
            desc = ln[2:]
        elif ln.startswith('tag='):
            kv = dict(re.findall(r'(\w+)=(\S+)', ln))
            out.append((desc, kv, ln))
        elif ln.startswith('  per_unit_gbs:') and out:
            out[-1][1]['_per'] = ln.split(':', 1)[1].strip()
        elif ln.startswith('SKIP') or ln.startswith('TIMEOUT') or ln.startswith('LAUNCH_ERROR'):
            out.append((desc, {'error': ln}, ln))
    return out


def main():
    path = sys.argv[1]
    fields = DEF
    g = None
    for i, a in enumerate(sys.argv):
        if a == '--fields':
            fields = sys.argv[i + 1].split(',')
        if a == '--grep':
            g = sys.argv[i + 1]
    for desc, kv, ln in parse(path):
        if g and g not in (desc or ''):
            continue
        if 'error' in kv:
            print('%-70s %s' % (desc, kv['error']))
            continue
        print('%-70s %s' % (desc, ' '.join('%s=%s' % (f, kv[f]) for f in fields if f in kv)))


if __name__ == '__main__':
    main()
