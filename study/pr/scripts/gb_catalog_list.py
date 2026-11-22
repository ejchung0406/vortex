"""Print the CI catalog's cases of one app as gemm-block_batch.sbatch LIST lines.

usage: python3 -I gb_catalog_list.py <worktree> <app> <xlen> <name prefix>
A model_parity case gives one line per leg (simx, rtlsim).
"""
import os
import sys

tree, app, xlen, prefix = sys.argv[1:5]
sys.path.insert(0, os.path.join(tree, "ci"))
import testcase as tc  # noqa: E402

for c in tc.load_all(os.path.join(tree, "ci", "testcases")):
    if c.app != app or not c.applies_to_xlen(int(xlen)):
        continue
    drivers = ["simx", "rtlsim"] if c.check == "model_parity" else [c.driver]
    for d in drivers:
        argv, env = c.run_command(int(xlen), driver=d)
        flags = [a for a in argv[3:] if not a.startswith("--args=")]
        args = [a[len("--args="):] for a in argv[3:] if a.startswith("--args=")]
        name = "{}_{}".format(prefix, c.id.split(":")[1] + "_" + d)
        print("|".join([name, d, c.app, args[0] if args else "", " ".join(flags),
                        env.get("CONFIGS", "")]))
