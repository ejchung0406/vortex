import re,sys,os
for n in sys.argv[1:]:
    f=f"results/{n}/ramulator.stats.log"
    if not os.path.exists(f): print(f"{n:22s} no stats"); continue
    s=open(f).read()
    def col(k): return [float(x) for x in re.findall(rf"^\s*{k}_0: ([0-9.e+]+)",s,re.M)]
    cyc=[float(x) for x in re.findall(r"memory_system_cycles: (\d+)",s)][0]
    hit=col("read_row_hits"); mis=col("read_row_misses"); con=col("read_row_conflicts"); ql=col("read_queue_len_avg"); att=col("num_read_reqs")
    srv=[h+m+c for h,m,c in zip(hit,mis,con)]; R=sum(srv)
    print(f"{n:22s} ch={len(srv)} served/ch {min(srv):.0f}..{max(srv):.0f}  hit {sum(hit)/R:.2f} conflict {(sum(con)+sum(mis))/R:.2f}  B/act {32*R/max(1,sum(mis)+sum(con)):.0f}  rejected {1-R/sum(att):.2f}  queue {min(ql):.0f}..{max(ql):.0f}  bus util {R/len(srv)*4/cyc/2:.2f}")
