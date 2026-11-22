#!/usr/bin/env python3
"""Print the run commands of catalog cases as shell lines: '<tag>\t<env>\t<argv>'.

usage: tests-bwstream_cases.py <src_root> <xlen> <id-prefix>...
Run from a build tree; the commands are what ci/test_runner.py executes.
"""
import shlex
import sys

src, xlen, prefixes = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
sys.path.insert(0, src + "/ci")
import testcase as tc  # noqa: E402

for c in tc.load_all(src + "/ci/testcases"):
    cat, cid, drv = c.id.split(":")
    if not any(cid.startswith(p) for p in prefixes) or not c.applies_to_xlen(xlen):
        continue
    argv, env = c.run_command(xlen)
    envs = " ".join("{}={}".format(k, shlex.quote(v)) for k, v in env.items()) or "-"
    print("{}-{}\t{}\t{}".format(cid, drv, envs, " ".join(shlex.quote(a) for a in argv)))
