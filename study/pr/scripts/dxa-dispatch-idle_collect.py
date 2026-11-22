import glob, os, re, sys
R = "/storage/project/r-hkim358-0/echung67/vortex-exp/results"
pat = sys.argv[1] if len(sys.argv) > 1 else "pr_ddi_*"
rows = {}
for d in sorted(glob.glob(os.path.join(R, pat))):
    n = os.path.basename(d)
    m = re.match(r"pr_ddi_(.+)_w(\w+)_(base|br)_(simx|rtlsim)$", n)
    if not m:
        continue
    meta = open(os.path.join(d, "meta.txt")).read() if os.path.exists(os.path.join(d, "meta.txt")) else ""
    rc = re.search(r"rc=(\S+)", meta)
    log = os.path.join(d, "run.log")
    txt = open(log, errors="replace").read() if os.path.exists(log) else ""
    perf = re.findall(r"PERF: instrs=(\d+), cycles=(\d+)", txt)
    passed = "PASSED" in txt
    val = (rc.group(1) if rc else "run", perf[-1] if perf else None, passed)
    rows.setdefault((m.group(1), m.group(2)), {})[(m.group(3), m.group(4))] = val
def c(v): return int(v[1][1]) if v and v[1] else None
print("%-8s %-3s | %9s %9s %6s | %9s %9s %6s | %s" % ("test","w","sx base","rtl base","gap","sx br","rtl br","gap","rc/instr notes"))
for (t, w), cell in sorted(rows.items()):
    sb, rb, sbr, rbr = (cell.get(k) for k in [("base","simx"),("base","rtlsim"),("br","simx"),("br","rtlsim")])
    def g(s, r):
        s, r = c(s), c(r)
        return "%.2f%%" % (100.0*abs(r-s)/r) if s and r else "-"
    notes = " ".join("%s/%s:rc=%s,i=%s,%s" % (k[0],k[1],v[0],v[1][0] if v[1] else "-", "P" if v[2] else "noPASS") for k,v in sorted(cell.items()))
    print("%-8s %-3s | %9s %9s %6s | %9s %9s %6s | %s" % (t, w, c(sb), c(rb), g(sb,rb), c(sbr), c(rbr), g(sbr,rbr), notes))
