# H200 bystander effect: cause and characterisation

Status: DRAFT. The method section is written; results are pending the
`bys_h200` holder job.

Tags: FACT = measured here; INFERENCE = my reasoning; UNVERIFIED = not checked.

## 0. Answer first

(pending data)

## 1. Method

- Tool: `gpu/bys_lab.cu` (built into `/tmp/$USER/bys` by `gpu/bys_build.sh`).
  Plans come from `gpu/bys_plan.py`, the runner is `gpu/bys_phase.sh`, and
  tables come from `gpu/bys_analyze.py`. Raw outputs are
  `results_gpu/h200_bys_<phase>.txt`.
- One process per phase:
  - allocates 24 GiB (a[i] = i) and classifies the L2 home side of every
    2 MB page in-process: SM 0 (side A) warms 16 lines per page, then
    SM 4 (side B) times dependent loads, and a far hit means "homed on A";
  - the within-page hash PA12⊕13⊕15⊕17⊕19 is applied and the page constant
    is voted over 16 lines;
  - it then runs every scenario of the plan trial-major, so the trials of
    different scenarios are interleaved in time.
- Scenario = one kernel launch:
  - every SM of every group runs one worker CTA in the group's role,
    placed by a TPC mask and checked by `%smid` (the SM of every unit is
    written back);
  - every unit starts at the same globaltimer instant (+300 µs), and the
    window is fixed at 1.0 s;
  - bytes count only if they complete inside the window;
  - disjoint buffers per SM (64 MB DRAM-resident, or 256 KB L2-resident),
    unless the scenario is a shared-buffer one.
- Roles:
  - TMA: cp.async.bulk 32 KB × depth 6, one issuer; with page lists, eight
    page-pure 4 KB sub-copies per 32 KB slot.
  - LD: 32 warps of 16 B ld.global.cg, 8 per lane in flight (128 KB per SM).
  - ST: 16 B st.global.cg.
  - TMAST: cp.async.bulk shared→global.
  - FMA: mma.sync f16→f32, no memory traffic.
  - LAT: one thread, dependent-address loads, latency histogram in 8-cycle
    bins.
  - Optional per-SM pacing in GB/s and a duty cycle; 50-1000 µs timeline
    bins per unit.
- Checks:
  - TMA checks the first and last word of each tile; LD checks one word
    per 16 B load; LAT checks every word;
  - a trial with a missing or misplaced unit, or any error, is retried and
    not counted.
- Clock and power:
  - NVML is sampled every 10 ms in-process (SM clock, instant power,
    clock-event reasons); the samples inside each window are summarised;
  - the in-kernel SM clock per unit comes from clock64 over globaltimer;
  - bytes per SM clock = bytes / (clock64 cycles of that SM in the window).
- Statistics: 5 trials per scenario (3 for sanity); the value is the median
  of the group's per-SM median over trials, with the range over trials.
