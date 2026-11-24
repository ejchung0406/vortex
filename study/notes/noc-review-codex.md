# Independent review of feature/noc

**Verdict: merge after fixes.** The default-off configuration still changes SimX launch behavior, including a completion regression, and a valid grouped configuration aborts at construction. Reviewed `2910aa91090aec196d8701dc9dda09c446a6516a`, against `f55faaa2d`. The base revision's classifier reports **R3** for the KMU DCR encodings and new configuration knobs, including the changed L2 bank derivation. The range has 33 commits, 39 files, +2,756/−270. File references below are to the reviewed head unless marked otherwise.

## Evidence

- The clean `feature/noc` worktree resolved to the requested head. The base classifier found no changes to golden data or existing CI cases.
- The author’s `notes/nocb/fin5_res.txt` records exact off-path base/head matches for small demo, sgemm, dxa_copy and bwstream on both simulators, and for h200s bwstream, bwstream_dxa and gemm_block on SimX. These are sampled comparisons. The table gives no per-run source hashes. `fin5.sh` stages `HEAD` and the named base over copies of `src_all`.
- The same table records the four-core DXA plus ingress limiter pair at 18,281 SimX vs. 17,256 RTL cycles, or +5.94% relative to RTL, with 28,384 instructions each. The DXA plus groups and response gate pair is 17,562 vs. 17,694, or −0.75%, also with 28,384 instructions each. I could not tie the reported +4.0% and −2.6% to this exact revision and configuration. The table gives workload results for both configurations, but no per-run source hashes.

## Findings

### F3 · high · static proof and SimX KMU run — standalone SimX can finish before it dispatches a CTA

**Location** `sim/simx/processor.cpp:448`, `:540`, `sim/simx/kmu/kmu.cpp:213`, `:299`, `sim/simx/main.cpp:228`.

A newly queued CTA in the KMU fan-out tree lives in a local deque. The normal `run()` path was updated to check `kmu_->busy()`, but the `cycle()` path still checks only core activity and the platform channel count. In a cold standalone run with two two-core sockets, one CTA, and the NoC and mask off, tick one puts the CTA in an internal link. No core is active and no launch beat is in a channel. With idle memory and other channels, `cycle()` returns false before the next tick can send that CTA. The isolated KMU reproduced an empty visible channel on tick 1 at head and the first CTA send on tick 2. Base sent the CTA on tick 1. The standalone premature exit follows from the caller’s completion condition. The standalone binary and the SST and gem5 integrations use this path. Use the same KMU liveness check in all completion paths and test a one-CTA cold launch through `cycle()`.

### F3 · high · confirmed by a SimX constructor run — a valid default lane count exceeds the SimX switch limit

**Location** `VX_config.toml:443`, `sim/simx/mem/noc.h:281`, `:296`, `hw/rtl/libs/VX_stream_arb.sv:47`, `:101`.

SimX represents one registered slice level and aborts on 13 or more slices. RTL recursively slices its join arbiter. An 18-socket group with four L1 memory ports per socket gives 72 request ports and 128 default uplink lanes. The 128-input response arbiter requires 16 slices and SimX fails at construction, even though the documented divisibility and lane constraints are satisfied. I instantiated the switch in the XLEN32 head copy. The 72 × 64 control constructed successfully. The 72 × 128 switch aborted at the one-slice-level assertion with exit 134. The same limit can be reached on the request side, for example 32 sockets × four ports. Match the recursive RTL topology and its pending capacities, or agree on a common explicitly documented and enforced limit with viable lane defaults.

### F2 · high · confirmed by a SimX KMU run — the zero-mask SimX launch path changes

**Location** `sim/simx/kmu/kmu.cpp:83`, `:218`, `:295`, `sim/simx/processor.cpp:405`. Base `f55faaa2d:sim/simx/kmu/kmu.cpp:56`, `:140`.

The prior SimX model reset a flat round-robin pointer on each launch. The reviewed model uses registered links per hierarchy level and preserves its pointers at launch reset, regardless of the core mask or memory knobs. With two sockets of two ready cores, four single-CTA messages previously left the KMU for cores 0, 1, 2, 3. The hierarchy adds the first socket-link cycle and routes to 0, 2, 1, 3. With two one-core sockets and two successive one-CTA launches the second launch previously went to core 0, and at head goes to core 1. Core ID and cache locality can therefore change even when every NoC control is off. If this is an intentional parity correction, separate it from the NoC identity claim and validate it against the old and new cycle/placement cases. Otherwise preserve the existing off path.

### F2 · high · static proof — extra DXA port changes the offered beat during a stall

**Location** `hw/rtl/VX_socket.sv:1049`, `hw/rtl/mem/VX_mem_bus_arb.sv:22`, `hw/rtl/libs/VX_stream_arb.sv:126`.

The second DXA port and the dcache share an unbuffered, non-sticky priority arbiter. If port 1 offers a DXA request while its downstream gate is full or out of credit, and a dcache request appears before ready returns, the arbiter switches the offered address, attributes and tag while valid stays high. Persistent dcache traffic can also starve this DXA port. Give the arbitrating producer a registered, stable request boundary and match its capacity and arbitration in SimX. Add a stalled overlap test with two distinct producer data streams. See RTL coding guidelines §4 and the RTL review handshake rules.

### F2 · high · confirmed by compiling the generated SimX config — grouping changes the L2 bank count under VM at a fixed lane count

**Location** `VX_config.toml:463`, `:272`, `hw/rtl/VX_gpu_pkg.sv:2396`, `sim/simx/constants.h:131`.

The off formula sizes banks from all demand inputs, including a PTW using the L2. The on formula counts socket ports only. In a single cluster with four one-port sockets, VM and L2 on, L3 off, the PTW is a fifth client in either topology. The bank count falls from eight to four when enabling two groups of two lanes, even though the total L2 inputs remain five. Compiling and printing the generated SimX config with each of those settings confirmed both results. This changes cache bandwidth/geometry and can change the derived reservation-station and MSHR sizing, confounding an on/off comparison. Keep dedicated pre-existing clients in the flat-geometry expression and test group/lane sweeps with and without a PTW.

### F2 · high · missing test — the failing DXA configuration is outside parity CI

**Location** `ci/testcase.py:113`, `ci/testcases/dxa.yaml:534`, `tests/regression/bwstream_dxa/kernel.cpp:45`, `tests/regression/bwstream_dxa/main.cpp:258`.

The reviewed runner forces parity and perf gates to XLEN32. Commit `dd94ad142`, which honors explicit `xlen`, is outside the head's ancestry, and the added cases supply no explicit XLEN. The named DXA case uses one small core and 112 B/cycle. Its kernel consumes shared tiles and writes a final result, so it does not provide the sustained kernel global reads described by its catalog comment. The uniform source can also hide a worker or address permutation. The author's four-core, 32-thread case with the two DXA ports and a 32 B/cycle ingress limit exceeds 5%, and there is no DXA-plus-groups-and-gate parity case. Land the explicit-XLEN runner behavior, test at XLEN64, and exercise both combinations with address- and worker-distinct data plus concurrent global load traffic.

### F2 · high · static proof — the SimX links pool buffering across registered RTL boundaries

**Location** `sim/simx/cluster.cpp:198`, `:234`, `sim/simx/mem/noc.h:329`, `sim/simx/delay_buffer.h:35`, `hw/rtl/libs/VX_stream_xbar.sv:92`, `hw/rtl/libs/VX_delay_buffer.sv:61`.

The RTL instantiates two-entry output storage for each xbar and byte gate plus the delay-link FIFO and output stage. For example on a nonzero-latency uplink the SimX xbar sends with delay into the DelayBuffer input’s pooled capacity. It has no separate xbar output slots at that edge, and its input ready accounting can reuse a just-freed slot. A lane blocked by a full L2 port produces different acceptance timing and head-of-line stalls after the downstream port resumes. Model each registered ready boundary and compare the first acceptance divergence in a stalled-lane test.

### F2 · high · missing test — required timing evidence is open

**Location** `hw/rtl/libs/VX_rate_gate.sv:68`, `hw/rtl/mem/VX_noc_switch.sv:126`, `hw/rtl/core/VX_kmu_bus_arb.sv:184`.

Port rotation, credit popcounts, hashing, flight bookkeeping and weighted launch selection add logic to RTL that the reviewed evidence does not cover with Beacon. A concrete timing exposure is the dependency from lane rotation and prefix counting to the registered gate grants as the lane count grows. A similar exposure exists in the launch selector even with the runtime mask zero. The actual critical path and area delta are unmeasured. Record Beacon comparisons against the base on the affected units and GPU, with justification for any accepted depth/area increases. No baseline changes were found in this range.

### F1 · high · static proof of reset behavior, medium confidence in cycle impact — switch and gate state differ on relaunch

**Location** `sim/simx/processor.cpp:405`, `sim/common/simobject.h:1442`, `sim/simx/cluster.cpp:192`, `sim/simx/mem/noc.h:60`, `:183`, `hw/rtl/VX_cluster.sv:381`, `:406`, `hw/rtl/libs/VX_rate_gate.sv:125`.

The SimX launch reset clears the new xbar and rate gate arbitration state. The RTL retains it between launches. A launch that ends with the request xbar pointer advanced, followed by another launch with two ready socket inputs hashed to the same lane, can pick a different first winner. Gate credits may refill during idle on both sides while the arbitration pointer still differs. No repeated grouped or gated launch parity run establishes the effect. Keep the new network arbitration state across launches if it models device state, and extend the repeat-launch case to the grouped and gated shape.

### F1 · high · static proof — changelog advertises a DXA test argument its parser rejects

**Location** `CHANGELOG.md:26`, `tests/regression/bwstream_dxa/main.cpp:77`, `tests/regression/bwstream/main.cpp:88`.

The shared entry advertises `-R` for the two streaming tests. Invoking `bwstream_dxa -R 3` follows the unknown-option path. The regular `bwstream` implements repeat launches.

## Claims

| Claim | Result | Evidence |
| --- | --- | --- |
| 1. New network knobs are off by default and every existing default-off configuration is cycle-identical to base | **Does not hold** | Grouping, byte caps and the runtime core mask default off. Ingress and grouping use explicit bypasses. The DXA port count still defaults to one, but an existing explicit count above one now affects RTL wiring. The unconditional SimX launch tree adds a tick and changes placement in a two-socket shape, with a separate liveness failure. |
| 2. The L2 bank count is independent of group uplink lanes | **Holds as stated for a fixed topology** | At head the grouped bank formula uses flat socket-port count, capped and rounded as before. Uplink count still sizes demand inputs and affects arbitration and tag width. Enabling grouping can change the L2 bank count with VM even at unchanged input count, as the 8-to-4 PTW reproduction shows. |
| 3. SimX and RTL agree on the new timing paths | **Does not hold as a general claim** | The final author table includes a +5.94% DXA ingress case with equal retired instructions. SimX also aborts at a fan-in supported by the RTL arbiter implementation. Buffering and relaunch state differ by inspection. Grouped DXA with a response gate is within 5% in the final recorded case. A thread-count sweep is still open. |

### Default-off evidence

The off groups branch preserves the per-port L2 link. Zero ingress cap bypasses the rate gate. A one-port DXA still uses the original merge path, and the added RTL mask propagation resets to zero. The launch hierarchy changes independently of those controls. In my isolated base/head SimX KMU runs at XLEN32, with two sockets of two ready cores and four CTAs, base issued on ticks 1–4 to cores `0,1,2,3`, and head issued on ticks 2–5 to `0,2,1,3`. With two one-core sockets and two one-CTA launches, base selected `0,0` and head selected `0,1`. The tests compile the repository KMU source and set the launch DCRs, including a block size of one. The tick counts are at the KMU interface.

The author’s small off-path pairs are equal between base and head for each simulator. SimX/RTL cycles were 6,410/6,423 for demo, 148,367/148,022 for sgemm, 6,466/6,470 for dxa_copy and 17,483/16,609 for bwstream. The bwstream pair already differs by +5.26% at base. On the existing XLEN64 h200s profile, the SimX-only base/head samples agree at 136,324 cycles for bwstream, 54,582 for bwstream_dxa and 562,462 for gemm_block. The results cover the recorded workload shapes. [Final author table](nocb/fin5_res.txt) · [Staging and workload commands](nocb/fin5.sh)

### DXA and grouped paths in the author table

Differences below are `(SimX − RTL) / RTL`, the runner's cycle denominator. All listed pairs agree on retired instructions. The author’s runner configures XLEN64. The staged source is identified in the launch script by symbolic `HEAD` over `src_all`, with the abbreviated base for comparison runs. The evidence does not identify an exact run commit for these samples. [Final author table](nocb/fin5_res.txt) · [Workload commands](nocb/fin5.sh) · [Launcher](../ice/run_point_ice.sbatch)

| Four-core case | SimX cycles | RTL cycles | Difference |
| --- | ---: | ---: | ---: |
| Load streaming, NoC off | 17,954 | 17,763 | +1.08% |
| Load streaming, groups | 17,975 | 17,986 | −0.06% |
| Load streaming, ingress cap | 18,190 | 18,247 | −0.31% |
| Load streaming, groups and response gate | 19,176 | 19,132 | +0.23% |
| Load streaming, groups and request gate | 18,792 | 18,742 | +0.27% |
| DXA, one port | 17,714 | 17,378 | +1.93% |
| DXA, two ports | 17,755 | 17,379 | +2.16% |
| DXA, two ports and 32 B/cycle ingress cap | 18,281 | 17,256 | **+5.94%** |
| DXA, two ports, groups and 32 B/cycle response gate | 17,562 | 17,694 | −0.75% |

In the two-port comparison, adding the ingress cap adds 526 cycles in SimX and removes 123 in RTL. Trace the same XLEN64 shape with and without the cap, starting at the DXA worker-to-port arbiter, dcache/DXA merge and each input to the socket ingress gate. Compare line IDs, request accept cycles on both ports, retained arbitration winners, credit and registered ready under stalls. This separates a DXA dispatch difference from a gate/backpressure difference before looking at L2 MSHRs or kernel phases. The results establish a parity failure and do not identify which model has the first incorrect event.

For grouped DXA, compare the pre-switch port trace with the hashed lane, the L2-accepted request and the response-gate grant. Inspect the xbar/delay buffers and return-gate credit recovery if divergence starts after a stall. The final −0.75% sample is within the project threshold. Reproduce any −2.6% result with the exact sources, build options and command before attributing it to the gate. An earlier table records +4.41% and +9.30% DXA samples and does not establish either of the originally quoted percentages for the final head. [Earlier author table](nocb/lane2_res.txt)

## Test coverage and model boundaries

| Control or behavior | Directed and parity coverage in the range | Remaining verification |
| --- | --- | --- |
| `NOC_GROUP_SIZE`, `NOC_UPLINK_PORTS` | Chase covers switch latency with two lanes. A four-lane case exercises a 16-port switch and one arbiter slice level. RTL regressions include a one-input switch. | Default lane count for a large group, another tree level, a narrow saturated uplink, VM/PTW and direct CP-on-L2 coexistence. |
| `NOC_SM_LATENCY`, `NOC_UPLINK_LATENCY` and existing `L2_LINK_LATENCY` | The chase, gate and slice parity cases use derived delays with a 20-cycle L2 link. RTL fence stress explicitly uses zero SM delay and 48 uplink cycles. | Paired parity with independent explicit delays, including zero on either side, and a fence under saturated link queues. |
| `NOC_UPLINK_BYTES`, `NOC_UPLINK_GATE_REQ` | Load parity covers both gate placements at 8 B/cycle and the response gate with a wider budget. RTL ordering tests exercise request-side atomics and response-side fence and flush behavior. | SimX/RTL ordering under the same controlled stalls, repeated grouped launches and grouped DXA. |
| `SM_INGRESS_BYTES`, `DXA_GMEM_PORTS` | Standalone ingress has a four-port streaming case. The DXA parity case sets both a positive cap and two ports. | Separate one-to-two-port and disabled-to-enabled deltas, XLEN64 overlap with sustained dcache loads, 32 B/cycle ingress and address-distinct data. |
| `CORE_DISABLE0..7` | Five parity cases use `-T` to check masked placement. They cover two within-cluster mask orientations, a cross-cluster mask and a repeated launch. | Default-zero multicore launch and standalone completion, a mask above the low DCR word, and skewed runnable cores to exercise the ready-conditioned weighting. |
| L2 bank derivation | The grouped cases exercise alternate lane counts in different topologies. | Compare fixed sockets while sweeping only lanes. Exercise PTW on L2 with and without grouping and assert the same geometry. |

The lineup comprises five NoC data path parity cases, five launch-mask cases and one DXA case, plus two L1-link parity cases in the same range. The RTL functional catalog also covers message passing, flush coherence, atomic ordering, WAR ordering and a blocked flush stress path. The parity cases introduced here are selected at XLEN32 by `ci/testcase.py:113–119`. The author's separately launched XLEN64 tests are a different evidence set. I ran catalog lint on the configured head copy. It accepted 1,107 cases in 28 categories. Lint validates the catalog schema. [Core cases](../wt_noc/ci/testcases/core.yaml#L223) · [DXA case](../wt_noc/ci/testcases/dxa.yaml#L534) · [Ordering regressions](../wt_noc/ci/testcases/regression.yaml#L667) · [XLEN selection](../wt_noc/ci/testcase.py#L113)

The SimX switch and gate correspond to the RTL group instances and socket ingress gate. Both use a power-of-two lane count and the same line-address XOR lane selection, carry the return-port tag through L2, charge reads and atomics on the request-side gate, and hold a flush token until prior requests from that port are delivered. The one-output case skips lane hash bits, and both models reject grouping with cluster-resident graphics caches. SimX observes delivery when the L2 endpoint pops. RTL samples valid/ready at the L2 port. Compare those boundaries when the L2 request skid is full to establish cycle equivalence. The switch and callback are in the uncore domain. Cross-domain socket traffic retains its register slices. I found no conclusive thread-specific failure by inspection, and thread-count equivalence remains unverified. [SimX switch](../wt_noc/sim/simx/mem/noc.h) · [RTL switch](../wt_noc/hw/rtl/mem/VX_noc_switch.sv) · [RTL group wiring](../wt_noc/hw/rtl/VX_cluster.sv#L325) · [SimX socket wiring](../wt_noc/sim/simx/socket.cpp#L231)

## H200 fidelity

This range adds a configurable socket ingress limiter and a single-level group switch in front of the existing L2. Existing profiles are unchanged. The author's h200s results cover the flat baseline. The design notes put partition routing, a cross-partition link, same-line merging and remote-read replication in later stages. The candidate rates and geometry below have different levels of empirical support. [Design parameters and implementation order](noc-model-design.md#L138) · [Prototype verification notes](noc-design-verification.md) · [Author source audit](gpu-noc-sources.md)

| Design quantity | Source and what it supports |
| --- | --- |
| **132 shipped SMs, eight GPCs, groups of 16 or eight in scaled profiles** | The local Nsight record gives 132 SMs and a 1.980 GHz maximum clock. NVIDIA's full GH100 figure describes up to 144 SMs and eight GPCs. The local GPC map has non-uniform groups and an unresolved split. Fan et al. also report non-uniform or heavily floor-swept configurations. A contiguous fixed-size group is a reduced model and cannot express the 132-SM map with eight equal groups. [Nsight record](../wt_noc/study/results_gpu/h200_l2s_ncu.txt#L12) · [Local GPC map](../wt_noc/study/results_gpu/h200_l2s_gpcmap.txt) · [NVIDIA Hopper architecture](https://developer.nvidia.com/blog/nvidia-hopper-architecture-in-depth/) · [Fan et al.](https://arxiv.org/html/2609.24270v1#S5.SS2.SSS1) |
| **113 B/cycle socket ingress, two 64-byte DXA ports, four L1 memory ports** | Local concurrent TMA and loads total 215–230 GB/s, about 109–116 B/cycle at 1.98 GHz, and 174–197 GB/s with 4 KiB tiles. TMA alone reaches 196–204 GB/s and loads about 206–223 GB/s, depending on placement. The 113 setting is a fit to one range. The experiments leave port count, queuing and in-flight limits unresolved. Two DXA ports and four L1 ports are Vortex implementation choices. With the ingress cap off, two DXA ports offer a theoretical 128 B/cycle before other bottlenecks. With the cap on, it governs read-line issue per socket and allows credit bursts. A multi-core socket shares that one gate. Use one core per socket when comparing the setting to per-SM measurements. [Local ingress measurements](../wt_noc/study/results_gpu/h200_l2s_part2.txt#L64) · [Proposed mapping](noc-model-design.md#L997) |
| **648 B/cycle GPC gate, 324 scaled, 32 lanes** | Several disjoint-load tests flatten near 1.28 TB/s with about eight active SMs per GPC. At 1.98 GHz this is roughly 648 B/cycle. Stores reach 935 GB/s in the tested range. Shared-buffer loads reach 2.753 TB/s at 256 KiB and 2.276 TB/s at 4 MiB. This supports a workload-specific cap on unique-line traffic, with merging as a hypothesis for shared data. The model charges each returned beat that reaches its gate. It has no cross-socket same-line merge ahead of that gate. At 32 lanes of 64 B, the 648 gate can admit up to 2,048 B in one cycle after idle, with credit bounded by 2,696 B. The hardware burst envelope is unmeasured. The 324 rate is scaled and the 32 lanes come from head-of-line blocking sweeps, with physical lane count unverified. [Local GPC results](../wt_noc/study/results_gpu/h200_l2s_e1.txt#L340) · [Store and shared-data sweeps](../wt_noc/study/results_gpu/h200_l2s_gpcsweep.txt#L436) · [Lane sweep and reduced settings](noc-model-design.md#L663) |
| **Cross-partition bandwidth and latency** | Local/remote tests give about 4.63 TB/s local and about 2.56 TB/s with both cross directions active. Roughly 1.28 TB/s per direction is an inference from the paired result. The 88-cycle one-way delay is half the roughly 177-cycle near/far difference. Physical links and their widths remain unresolved. The 40/59-cycle near path and 174-cycle fixed DRAM term are fitted decompositions. Fan et al. independently report near/far clustering, with different endpoints and timing values. The measured remote-read locality change is also outside the reviewed implementation. [Local/remote results](../wt_noc/study/results_gpu/h200_l2s_local.txt#L154) · [Local side assignment](../wt_noc/study/results_gpu/h200_l2s_sides.txt) · [Design latency fit](noc-model-design.md#L250) · [Fan et al.](https://arxiv.org/html/2609.24270v1#S6) |
| **L2 slices, hashing, cache capacity and merging** | The local profiler reports 96 slices, 12 FBPs and 60 MiB. 640 KiB per slice follows from a uniform division. The partition XOR probe used a limited address range and page-local variation. It does not establish a full physical hash. The proposed 256 B slice granule and bits 8, 10 and 11 come from a probe confounded by requests to the same line. GemAtlas describes a related H20 with slice remapping. Accel-Sim 2.0 reports merge fan-in up to four for its H100/B200 experiments. It does not establish the H200 table size. The proposed entry count, 20 ways, 256 MSHRs and later remapping policy are modeling choices. [Local profiler](../wt_noc/study/results_gpu/h200_l2s_ncu.txt#L12) · [Hash probe](../wt_noc/study/results_gpu/h200_l2s_lat_analysis2.txt#L5) · [GemAtlas](https://arxiv.org/html/2611.13860v1#S5) · [Accel-Sim 2.0](https://arxiv.org/html/2608.22602v1#S2.SS3) |
| **HBM3E channels, stack count and slice-to-channel mapping** | NVIDIA specifies 141 GB and 4.8 TB/s. The local profiler's 6,016 bits equal 94 units of 64 bits by arithmetic. The 16-channel, 6.25 Gb/s/pin and 800 GB/s stack used by h200s follows an inferred six-stack, 1,024-bit-per-stack model. Micron documents a 24 GB eight-high HBM3E part for H200. Neither source establishes the disabled-channel or per-slice wiring. The one-to-one slice/channel topology is a model choice. NVIDIA's patent gives examples with three or five slices per two channels and does not establish the retail H200 layout. [NVIDIA H200 specifications](https://www.nvidia.com/en-us/data-center/h200/) · [Micron announcement](https://investors.micron.com/node/46656/pdf) · [Local profiler](../wt_noc/study/results_gpu/h200_l2s_ncu.txt#L12) · [NVIDIA slice-addressing patent](https://patents.google.com/patent/US11429534B2/en#description) |

The existing h200s profile models 22 SMs, 16 L2 banks, 8.25 MiB and 800 GB/s. A sixth of the observed 60 MiB is 10 MiB. The proposed 32-SM two-stack shapes increase achieved memory bandwidth per SM to about 48 GB/s, or 1.375× the measured part's 35 GB/s, and increase L2 per SM to 640 KiB from about 465 KiB. The proposed 64-SM shape is closer on those ratios. The design’s scaled shapes are memory-limited before the proposed cross-link can be validated. For H200 placement or bandwidth claims, report these as scaled experiments and validate the actual workload envelope, including shared-line loads and bursts. The existing h200s off-path agreement supplies a baseline for that work. [Existing profile](../wt_noc/profiles/h200s.json#L31) · [Scaling calculations](noc-model-design.md#L663)

## Verification scope and remaining evidence

- **Ran independently** — exact-range `git diff --check`, base-revision impact classifier, catalog lint, isolated base/head XLEN32 KMU launch probes, isolated head switch construction at 64 and 128 lanes, and head-generated configuration compilation for the VM/PTW bank example. Temporary source copies were configured outside `wt_noc`. The 128-lane constructor failed as recorded above.
- **Inspected** — the head's RTL and SimX data paths, compile-time bypass and constraints, launch/reset and KMU DCR changes, new test declarations and harnesses, author-run tables and H200 evidence. The RTL configuration guards agree on graphics exclusion. The test catalog has no new relaxed tolerance or golden data edits.
- **Not run independently** — full SimX workloads, standalone binary reproduction, exact-head XLEN64 paired runs, thread-count sweep, RTL elaboration or simulation, XRT integration, Beacon, synthesis and full-chip H200 validation. I did not audit the later partition/router RTL because it is outside the specified range. The memory packaging and physical lane layout remain inferred. The restricted HBM document was neither accessed nor copied. No Slurm runs were started or modified.
- **Before landing** — clear the F3s, resolve the zero-mask behavior and other F2s under the review policy, and supply exact-head parity and timing evidence. Run small XLEN64 DXA overlap and fence cases, then compare grouped and ungrouped PTW geometries and test the switch at the documented maximum supported size. Capture Beacon and elaboration evidence for the changed RTL. Review exact landing revision and any overlap with its DXA destination branch. [Vortex review policy](../wt_noc/docs/review/code-review-policy.md)
