# SimX wall-clock optimization

Branch `feature/simx-perf` (off `feature/dxa-borrowing` 540b3dad2), worktree
`vortex-exp/wt_perf`. Goal: simulated cycles per second of host time and
end-to-end time of the study runs, with cycle counts, perf counters and program
output bit-identical for the same build configuration.

Tools: `vortex-exp/perf/` (`cases.sh` the fixed case table, `bench.sh` build/run,
`identical.sh` the identity check, `vt.sh`/`vtrep.sh` VTune user-mode hot spots,
`BRIEF.md` the contract every agent worked under).

## 1. Benchmarks (fixed 2026-11-21)

| case | app | profile | SMs | CONFIGS | args |
|---|---|---|---|---|---|
| roof1 | tmem_roof | h200s | 1 | | `-m0 -n512 -i256` |
| gb1 | gemm_block | h200s | 1 | `-DMTILES=1 -DPROFILE_ENABLE` | `-m16 -n1024 -k4096 -s1` |
| bw1/4/11/22 | bwstream | h200s | 1/4/11/22 | | `-n512 -r1 -p0 -u4 -b32` |
| dxa4/11/22 | bwstream_dxa | h200s | 4/11/22 | | `-e4096 -n32 -g16 -d4 -c0` |
| gb22 | gemm_block | h200s | 22 | `-DMTILES=1 -DPROFILE_ENABLE` | `-m16 -n22528 -k4096 -s1 -d4` |
| d_sgemm | sgemm | default | 1 | | `-n64` |
| d_tcu | sgemm_tcu | default | 1 | | `-n64` |
| d_dogfood | dogfood | default | 1 | | |
| d_sgemm4 | sgemm | default | 4 | `-DVX_CFG_L2_ENABLE` | `-n128` |

All with `--perf=7` (`-DPERF_ENABLE`, `VORTEX_PROFILING=7`) and
`VORTEX_VM_MEGAPAGE_HEAP=1`. A case is built exactly as `ci/blackbox.sh` builds
it (`make -C tests/regression/<app> runtime-simx` with the same CONFIGS) and the
binaries are staged, so the timed command is the simulator process alone:
`LD_LIBRARY_PATH=<stage> VORTEX_DRIVER=simx VORTEX_PROFILING=7 ./<app> <args>`.

## 2. Profile of the unoptimized base (VTune user-mode sampling, CPU time over all threads)

Host: Xeon Gold 6226 (Cascade Lake), shared node, so percentages matter, not seconds.

### gb1 (1 SM, memory + tensor)
```
  8.3%    4.82s std::__atomic_futex_unsigned_base::_M_futex_wait_until_steady
  5.3%    3.07s TfrFedp::multiply_to_common
  5.2%    3.04s std::vector<unsigned int, std::allocator<unsigned int>>::_M_realloc_insert<unsigned int const&>
  5.1%    2.99s vortex::DxaCore::Impl::tick
  3.9%    2.28s Ramulator::HBM3::get_preq_command
  3.5%    2.05s TfrFedp::operator()
  2.8%    1.60s main
  2.7%    1.58s cfree
  2.7%    1.55s Ramulator::HBM3::check_ready
  2.2%    1.30s vortex::CommandProcessor::tick_engine
  2.2%    1.28s std::vector<int, std::allocator<int>>::_M_realloc_insert<int const&>
  1.8%    1.04s TfrFedp::alignment
  1.8%    1.03s std::vector<int, std::allocator<int>>::_M_realloc_insert<int>
  1.7%    1.00s std::_Function_handler<bool (void), vx_device::make_cp_hooks(void)::{lambda()#7}>::_M_invoke
  1.7%    0.99s vortex::Core::Impl::issue
  1.5%    0.86s operator new
  1.4%    0.81s vortex::RAM::get
  1.4%    0.81s vortex::SimObject<vortex::Dispatcher>::do_tick
  1.3%    0.77s vortex::SimPlatform::tick.constprop.0
  1.3%    0.73s std::_Sp_counted_base<(__gnu_cxx::_Lock_policy)2>::_M_release
  1.0%    0.61s vortex::DxaCore::Impl::tick_worker_smem_wr
  1.0%    0.56s Ramulator::WindowedDRAMController::tick
  0.9%    0.54s MSHR::lookup
  0.8%    0.49s vortex::TxCrossBar<vortex::MemReq>::on_tick
  0.8%    0.48s vortex::Core::Impl::commit
  0.8%    0.48s vortex::Scoreboard::get_uses
  0.8%    0.46s vortex::SimChannelEvent<vortex::MemRsp>::fire
  0.8%    0.46s vortex::Core::Impl::schedule
  0.8%    0.45s vortex::CommandProcessor::tick
  0.8%    0.45s vortex::Scheduler::schedule
  0.7%    0.43s vortex::CommandProcessor::tick_launch
  0.7%    0.39s vortex::Mmu::on_tick
  0.7%    0.39s rv_htof_s
  0.7%    0.39s vortex::TxRxCrossBar<vortex::MemReq, vortex::MemRsp>::on_tick
  0.7%    0.39s vortex::Cache::Impl::tick
  0.6%    0.37s Ramulator::DRAMNodeBase<Ramulator::HBM3>::update_timing
  0.6%    0.37s Ramulator::GenericDRAMSystem::tick
  0.6%    0.34s Ramulator::FRFCFS::get_best_request
  0.5%    0.31s set_t::tag_match
  0.5%    0.31s func@0x9680
  0.5%    0.30s std::_Function_handler<void (Ramulator::Request&), RamulatorDram::issue(RamulatorDram::channel_t&)::{lambda(Ramulator::Request&)#1}>::_M_manager
  0.5%    0.29s vortex::Scoreboard::in_use
  0.5%    0.28s vortex::CommandProcessor::enabled
  0.4%    0.26s vortex::SimObject<CacheBank>::do_tick
  0.4%    0.26s vx_dev_init::{lambda(void*unsigned int, unsigned int*)#4}::_FUN
total 58.1s
```

### roof1 (1 SM, tensor compute)
```
 11.3%    2.97s std::vector<unsigned int, std::allocator<unsigned int>>::_M_realloc_insert<unsigned int const&>
 10.2%    2.69s TfrFedp::multiply_to_common
  8.6%    2.27s vortex::TcuUnit::Impl::mma_lmem
  8.5%    2.24s vortex::TcuTbuf::read_b
  8.1%    2.13s std::__atomic_futex_unsigned_base::_M_futex_wait_until_steady
  7.8%    2.05s TfrFedp::operator()
  5.5%    1.44s TfrFedp::alignment
  5.1%    1.35s std::vector<int, std::allocator<int>>::_M_realloc_insert<int const&>
  4.9%    1.28s std::vector<int, std::allocator<int>>::_M_realloc_insert<int>
  4.5%    1.19s cfree
  2.5%    0.65s operator new
  2.2%    0.59s vortex::TcuUnit::Impl::gather_word<vortex::TcuUnit::Impl::load_lmem_word(vortex::TcuUnit::Impl::lmem_desc_t const&, unsigned int, unsigned int, unsigned int, bool, bool, bool) constconst::{lambda(unsigned long)#1}>
  2.1%    0.56s std::_Function_handler<bool (void), vx_device::make_cp_hooks(void)::{lambda()#7}>::_M_invoke
  2.0%    0.54s vortex::TcuTbuf::read_a
  1.9%    0.51s vortex::DxaCore::Impl::tick
  1.6%    0.43s vortex::CommandProcessor::tick_engine
  0.8%    0.21s vortex::CommandProcessor::tick_launch
  0.6%    0.17s std::vector<int, std::allocator<int>>::reserve
  0.5%    0.13s __memcpy_evex_unaligned_erms
  0.5%    0.13s vortex::CommandProcessor::tick
  0.5%    0.12s vortex::CommandProcessor::enabled
  0.4%    0.10s func@0x9680
  0.4%    0.10s vortex::TcuUnit::Impl::job_run
  0.3%    0.09s vortex::Scheduler::schedule
  0.3%    0.09s std::vector<TfrFedp::pterm_t, std::allocator<TfrFedp::pterm_t>>::reserve
  0.3%    0.08s vortex::Mmu::on_tick
  0.3%    0.07s Ramulator::WindowedDRAMController::tick
  0.3%    0.07s vortex::CommandProcessor::busy
  0.3%    0.07s vortex::Cache::Impl::tick
  0.3%    0.07s operator delete
total 26.3s
```

### bw1 (1 SM, plain-load stream)
```
  5.5%    0.55s vortex::Scoreboard::get_uses
  5.5%    0.55s std::__atomic_futex_unsigned_base::_M_futex_wait_until_steady
  4.3%    0.43s vortex::DxaCore::Impl::tick
  4.2%    0.42s Ramulator::HBM3::get_preq_command
  3.8%    0.38s vortex::SimObject<CacheBank>::do_tick
  2.6%    0.26s vortex::SimObject<vortex::Dispatcher>::do_tick
  2.5%    0.25s std::_Sp_counted_base<(__gnu_cxx::_Lock_policy)2>::_M_release
  2.2%    0.22s vortex::SimPlatform::tick.constprop.0
  2.2%    0.22s vortex::Core::Impl::issue
  1.9%    0.19s Ramulator::HBM3::check_ready
  1.7%    0.17s MSHR::has_pending_fill
  1.7%    0.17s CacheBank::processInputs
  1.7%    0.17s vortex::CommandProcessor::tick_engine
  1.7%    0.17s vortex::TxCrossBar<vortex::MemReq>::on_tick
  1.7%    0.17s vortex::Core::Impl::commit
  1.4%    0.14s MSHR::replay.isra.0
  1.3%    0.13s vortex::SimChannelEvent<vortex::MemRsp>::fire
  1.3%    0.13s Ramulator::DRAMNodeBase<Ramulator::HBM3>::update_timing
  1.3%    0.13s vortex::TxRxCrossBar<vortex::MemReq, vortex::MemRsp>::on_tick
  1.2%    0.12s vortex::Cache::Impl::tick
  1.2%    0.12s cfree
  1.2%    0.12s Ramulator::WindowedDRAMController::tick
  1.1%    0.11s vortex::Scoreboard::in_use
  1.1%    0.11s std::_Function_handler<bool (void), vx_device::make_cp_hooks(void)::{lambda()#7}>::_M_invoke
  1.1%    0.11s Ramulator::GenericDRAMSystem::tick
  1.0%    0.10s operator new
  1.0%    0.10s vortex::MemCoalescer::on_tick
  1.0%    0.10s vortex::Mmu::on_tick
  0.9%    0.09s Ramulator::FRFCFS::get_best_request
  0.9%    0.09s CacheBank::processFill
  0.9%    0.09s MSHR::lookup
  0.9%    0.09s vortex::LsuUnit::process_response_step
  0.8%    0.08s vortex::Core::Impl::schedule
  0.8%    0.08s set_t::tag_match
  0.8%    0.08s RamulatorDram::tick
  0.8%    0.08s CacheRspMcast::on_tick
  0.8%    0.08s std::_Function_handler<void (Ramulator::Request&), RamulatorDram::issue(RamulatorDram::channel_t&)::{lambda(Ramulator::Request&)#1}>::_M_manager
  0.8%    0.08s vortex::SimChannelEvent<vortex::MemReq>::fire
  0.7%    0.07s vortex::Scheduler::schedule
  0.7%    0.07s vortex::Operands::total_stalls
total 10.0s
```

### dxa4 (4 SMs, DXA copy engine)
```
 15.7%    4.45s std::__atomic_futex_unsigned_base::_M_futex_wait_until_steady
 12.9%    3.66s Ramulator::HBM3::get_preq_command
  6.5%    1.84s vortex::DxaCore::Impl::tick
  5.3%    1.52s Ramulator::HBM3::check_ready
  4.1%    1.18s vortex::CommandProcessor::tick_engine
  3.8%    1.07s std::_Function_handler<bool (void), vx_device::make_cp_hooks(void)::{lambda()#7}>::_M_invoke
  3.5%    0.98s vortex::DxaCore::Impl::tick_worker_smem_wr
  3.3%    0.93s Ramulator::FRFCFS::get_best_request
  2.5%    0.70s vortex::SimPlatform::tick.constprop.0
  1.5%    0.44s vortex::CommandProcessor::tick
  1.3%    0.37s vortex::CommandProcessor::tick_launch
  1.1%    0.31s vortex::Mmu::on_tick
  1.1%    0.30s MSHR::replay.isra.0
  1.0%    0.29s Ramulator::DRAMNodeBase<Ramulator::HBM3>::update_timing
  0.9%    0.26s vortex::SimObject<CacheBank>::do_tick
  0.9%    0.26s CacheBank::processInputs
  0.9%    0.26s MSHR::has_pending_fill
  0.9%    0.25s set_t::tag_match
  0.9%    0.25s std::_Function_handler<void (Ramulator::Request&), RamulatorDram::issue(RamulatorDram::channel_t&)::{lam
  0.8%    0.24s vortex::SimChannelEvent<vortex::MemRsp>::fire
  0.8%    0.23s vortex::Cache::Impl::tick
  0.8%    0.23s Ramulator::WindowedDRAMController::tick
  0.8%    0.23s vortex::Core::Impl::issue
  0.8%    0.22s CachePortArb::on_tick
  0.7%    0.19s vortex::TxRxCrossBar<vortex::MemReq, vortex::MemRsp>::on_tick
  0.6%    0.18s vortex::CommandProcessor::enabled
  0.6%    0.18s operator new
  0.6%    0.17s vx_dev_init::{lambda(void*unsigned int, unsigned int*)#4}::_FUN
  0.6%    0.17s vortex::Core::Impl::schedule
  0.6%    0.17s Ramulator::Lambdas::Preq::Bank::RequireRowOpen<Ramulator::HBM3>
total 28.4s
```

## 3. Baseline (unoptimized 540b3dad2, serial)

Dedicated 4-core Slurm allocations on Xeon Gold 6226 (2.7 GHz, Cascade Lake) nodes whose other cores were busy
(`results/perf_base_t/`, `perf/timing.sbatch`); min and median of the reps. Identity reference logs: `results/perf_base/`
(and `perf_base_t/` for bw22, gb22). The 22-SM case varied 564-732 s between reps on the same node, so 22-SM
before/after numbers are taken interleaved in one job.

| case | reps | wall min (s) | wall median (s) | cycles | cycles/s (min wall) | peak RSS (MB) |
|---|---|---|---|---|---|---|
| roof1 | 3 | 21.36 | 21.36 | 134416 | 6293 | 42 |
| gb1 | 3 | 41.18 | 41.37 | 647264 | 15718 | 82 |
| bw1 | 3 | 7.74 | 7.75 | 117390 | 15167 | 50 |
| bw4 | 3 | 42.40 | 42.58 | 132189 | 3118 | 77 |
| bw11 | 3 | 145.78 | 147.51 | 149134 | 1023 | 124 |
| bw22 | 2 | 563.71 | 648.00 | 261168 | 463 | 193 |
| dxa4 | 3 | 13.12 | 13.13 | 69883 | 5326 | 68 |
| dxa11 | 3 | 17.62 | 17.78 | 39833 | 2261 | 99 |
| dxa22 | 3 | 23.25 | 23.34 | 31103 | 1338 | 144 |
| gb22 | 1 | 2690.38 | 2690.38 | 994469 | 370 | 726 |
| d_sgemm | 3 | 2.39 | 2.39 | 792351 | 331528 | 12 |
| d_tcu | 3 | 0.35 | 0.35 | 83506 | 238589 | 13 |
| d_dogfood | 3 | 0.63 | 0.63 | 233631 | 370843 | 13 |
| d_sgemm4 | 3 | 18.31 | 18.34 | 1453274 | 79371 | 15 |

## Interim: trial integration 1 (perf/int-try 49b3204cf, serial, no build-flag or MT change)

Merge of the agents' branches as of 19:10 (kernel, cache, core, dram, tcu: 28 commits). One Slurm job
(`perf_ab_int1`, 8 cores, Xeon Gold 6226, node shared with other users), base and new alternating, 1 rep
(job ended at its limit; a second rep ran for the short cases), wall seconds, every new log IDENTICAL to
the base log (`perf/identical.sh`):

| case | base | new | speedup |
|---|---|---|---|
| roof1 | 21.39 | 5.13 | 4.17x |
| gb1 | 42.81 | 20.81 | 2.06x |
| bw1 | 8.47 | 4.29 | 1.97x |
| bw4 | 46.01 | 24.82 | 1.85x |
| bw11 | 193.37 | 93.48 | 2.07x |
| bw22 | 867.46 | 363.60 | 2.39x |
| dxa4 | 14.54 | 5.96 | 2.44x |
| dxa11 | 19.36 | 8.72 | 2.22x |
| dxa22 | 27.10 | 12.46 | 2.17x |
| d_sgemm | 2.40 | 2.14 | 1.12x |
| d_tcu | 0.35 | 0.28 | 1.25x |
| d_dogfood | 0.64 | 0.57 | 1.12x |
| d_sgemm4 | 18.37 | 16.42 | 1.12x |

Slurm accounts: every job of this work runs on `gts-hkim358` (the paid account is reserved for H200 jobs).

## 3. How the work is organized (2026-11-21)

One agent per area, each on its own branch `perf/<area>` and worktree
`vortex-exp/wt_perf_<area>` off `feature/simx-perf`, each with an independent
reviewer (static review against `perf/REVIEW.md`): `mt` (the multi-threaded
executor), `kernel` (SimObject kernel, host runtime, RAM), `cache` (sim/simx/mem),
`core` (pipeline), `dram` (Ramulator wrapper and the Windowed controller), `tcu`
(tensor core, DXA), `build` (flags, LTO, PGO, allocator). The lead merges
reviewed branches into `feature/simx-perf`.

Constraints met on the way:
- Billing: the charge account ran out during the work, so no Slurm jobs are
  used for development. Everything runs in the one 8-CPU interactive
  allocation, which is on a node at load 60-90: single timings there are
  useless, only interleaved A/B ratios are quoted, and the 22-SM before/after is
  taken once per integrated round in a single right-sized job.
- Baseline logs for the identity check: `results/perf_base/<case>/run1.log` for
  all cases but bw22 and gb22 (too slow on the loaded node; taken in the final
  job together with the optimized runs).

## 4. Round 1 integration (2026-11-21, late)

`feature/simx-perf` was moved to the new head of `feature/dxa-borrowing`
(18deea5da, 17 commits later than the first base) and the five areas that were
verified identical on their own were cherry-picked in this order, as staging
branches `int/<area>` (cumulative): tcu, dram, kernel, cache, core (49 commits).
The MT executor and build flags are round 2. `perf/core`'s HashTable commit was
dropped: `perf/cache` had made the same change (lowest free index from a bit
set, same allocation order).

Files touched per area (for the later merges with `feature/warp-rate` and `feature/noc`):

| area | commits | files |
|---|---|---|
| tcu | 4 | sim/simx/dxa/dxa_core.cpp, sim/simx/tcu/tcu_tbuf.{cpp,h}, sim/simx/tcu/tcu_unit.cpp, sw/common/fedp.h |
| dram | 5 | sim/common/dram_sim.cpp, third_party/Makefile, third_party/ramulator_vortex/windowed_controller.cpp |
| kernel | 8 | sim/common/simobject.h, sim/common/mem.cpp, sim/simx/regslice.h, sw/runtime/simx/vortex.cpp |
| cache | 17 | sim/simx/mem/{cache,local_mem_switch,lsu_mem_adapter,mem_coalescer,mmu,mmu_tlb,ptw,tlb_l2}.cpp, mmu.h, mmu_tlb.h, sim/simx/types.h (crossbars, HashTable) |
| core | 15 | sim/simx/{core,scheduler,scoreboard,dispatcher,opc_unit,lsu_unit,sfu_unit,alu_unit,csr_unit,wctl_unit}.cpp, core.h, scheduler.h, scoreboard.h, lsu_unit.h, cta_dispatcher.h, decompressor.h |

Overlap with `feature/warp-rate` (scheduler, fetch, decode, scoreboard, ibuffer
timing) is the core area: scheduler.cpp/.h, scoreboard.cpp/.h and the
frontend/issue parts of core.cpp. Overlap with `feature/noc` (socket-to-L2
path, DXA ports): dxa_core.cpp (worker slot bitmaps) and types.h crossbars.

Earlier evidence on the first base (540b3dad2), kernel+cache+dram+tcu merged,
serial, default flags: IDENTICAL on d_sgemm, d_tcu, d_dogfood, d_sgemm4, bw1,
roof1, gb1, dxa4, bw4. Interleaved A/B on the loaded node (3 rounds, wall s,
base/merged): roof1 70.7/15.0, 115.3/13.4, 54.1/8.9; dxa4 87.4/21.8, 68.5/15.8,
46.2/9.9; gb1 151.6/83.2, 169.7/92.7, 103.2/53.8; bw1 30.5/25.1, 47.3/26.2,
16.7/17.4; d_sgemm too short to read at that load. The measurement that counts
is the single job `perf_measure` (results/perf_measure_r1).

## 5. Round 1 before/after (job perf_measure 15988749, 2026-11-21 22:46 to 2026-11-22 00:03)

Taken on head 18deea5da of `feature/dxa-borrowing` (tag `perf-r1-on-18deea5da`
= e4859d934): base 18deea5da; optimized = base + tcu, dram, kernel, cache, core
(49 commits). Serial, default flags (-O3), no MT, no LTO/PGO. Both built in the
job (Xeon Gold 6226 @ 2.7 GHz, node atl1-1-02-004-3-2, 12 cpus of a shared node
at load 13-21), base and optimized alternating 5 times (gb22: one pair), four
streams in parallel; median wall (min), median user CPU.

| case | cycles | n | base wall med (min) s | opt wall med (min) s | wall speedup | base user s | opt user s | user speedup | base kcyc/s | opt kcyc/s | RSS MB b/o |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bw1 | 117794 | 5/5 | 8.0 (7.8) | 3.4 (3.4) | 2.36x | 8.4 | 3.3 | 2.53x | 14.74 | 34.75 | 50/51 |
| bw11 | 147413 | 5/5 | 158.6 (154.5) | 65.7 (62.0) | 2.41x | 168.3 | 65.2 | 2.58x | 0.93 | 2.24 | 123/130 |
| bw22 | 260089 | 5/5 | 598.3 (520.0) | 169.3 (165.7) | 3.53x | 634.5 | 168.2 | 3.77x | 0.43 | 1.54 | 192/205 |
| bw4 | 130808 | 5/5 | 46.2 (45.0) | 18.1 (17.7) | 2.55x | 49.0 | 18.0 | 2.73x | 2.83 | 7.22 | 77/80 |
| d_sgemm | 792351 | 5/5 | 2.4 (2.4) | 1.9 (1.9) | 1.27x | 2.5 | 1.9 | 1.35x | 326.07 | 414.84 | 12/12 |
| d_sgemm4 | 1453274 | 5/5 | 18.2 (18.1) | 14.1 (14.1) | 1.29x | 19.2 | 14.1 | 1.36x | 79.94 | 102.85 | 15/15 |
| d_tcu | 83506 | 5/5 | 0.3 (0.3) | 0.2 (0.2) | 1.46x | 0.4 | 0.2 | 1.57x | 238.59 | 347.94 | 12/12 |
| dxa11 | 39785 | 5/5 | 19.2 (18.5) | 5.1 (5.1) | 3.74x | 20.3 | 5.0 | 4.04x | 2.07 | 7.74 | 99/108 |
| dxa22 | 30880 | 5/5 | 25.8 (23.4) | 6.5 (6.1) | 3.96x | 27.2 | 6.4 | 4.27x | 1.20 | 4.74 | 144/162 |
| dxa4 | 70625 | 5/5 | 14.1 (13.7) | 3.7 (3.6) | 3.77x | 14.9 | 3.7 | 4.05x | 5.01 | 18.88 | 68/73 |
| gb1 | 633248 | 5/5 | 42.2 (42.1) | 16.1 (16.1) | 2.61x | 45.7 | 17.3 | 2.64x | 15.01 | 39.23 | 82/84 |
| gb22 | 1004023 | 1/1 | 2838.8 (2838.8) | 756.3 (756.3) | 3.75x | 2991.4 | 812.5 | 3.68x | 0.35 | 1.33 | 722/739 |
| roof1 | 134416 | 5/5 | 21.3 (21.3) | 3.1 (3.1) | 6.81x | 22.6 | 3.1 | 7.32x | 6.30 | 42.94 | 42/43 |

Identity (cycles, every PERF line, program output, first alternation):
IDENTICAL on all 14 cases (`results/perf_measure_r1/identity.txt`).

The 22-SM base times vary with the node (bw22 base 520-864 s, optimized
166-187 s): the first pair alone read 5.2x, the median of five is 3.5x.

## 6. Independent reviews (static, per branch, 2026-11-22)

| branch | verdict | findings acted on |
|---|---|---|
| cache (17 commits) | every commit behaviour-preserving, nothing blocks | F1: trace prints read moved-from packets (harmless today; print before the send); F0: missing asserts/braces. Follow-up commits on `perf/cache2` |
| dram (5) | 4 OK; cdb8f0466 NOT EQUIVALENT across reset | F2: `reset()` frees pool requests still inside Ramulator, a late callback then completes a reused slot (needs: Memory outside LaunchKeepScope, two launches, a read in flight at the end of a launch). Two latent F1 in the Windowed controller (state_changed scope for all-bank commands, dead future-actions guard), F0: the Makefile lost its drift guard against the pinned upstream controller. Fix commits on `perf/dram2` |
| tcu (4) | 3 OK; 3a7e7efee NOT EQUIVALENT for W <= 21 | F1: the fast alignment path of the TFR dot product is exact only for W >= 22 (SimX and CI use 25 or 53; hand runs of hw/unittest with a small --W differ in ~1.5% of results). Fix on `perf/tcu2` |
| core (16) | 15 OK; c3961105e "a quiescent core sleeps" can hang | F3: with NUM_LSU_LANES < NUM_THREADS an instruction is several packets; the EOP packet can retire before another packet's response returns, pending reaches 0, the core sleeps and the late LSU output is never read (run never ends). Default and h200s have one packet per instruction, so the benchmark set cannot hit it. F1: CTA arrival wakes on delivery, running() counts from reservation. Fixed in the re-derivation `perf/core2` |
| mt (7) | equal to serial on h200s by argument; no race found statically; evidence missing | F2: no TSan run and no multi-SM identity after the uncore started ticking concurrently with the SMs. F1: debug cross-access assert relaxed too far; `uncore_hoistable()` not conservative (fan-in, mailbox order, event links); device-side MPM CSR reads of foreign counters race in MT builds. Addressed on `perf/mt2` |
| kernel (8) | report pending | |

Pre-existing defects the reviews found in the base (not introduced here, not fixed here; for the user):
- `sw/common/fedp.h` lines 179-180, 786, 825 shift by the type width or more when Ww > 32 (W >= 30, or W = 25 with more than 32 terms); `reduce_f8` (line 705) shifts by a negative amount when a product is zero; `hw/unittest/tcu_fedp/fedp.py` constructs `FEDP`, which fedp.h does not define.
- `csr_unit.cpp:131`: a device-side read of an MPM CSR other than mcycle/minstret reads other domains' counters, a data race in any `-DSIMX_MT` build (no in-tree kernel does it).

## 7. Stack on the current head 811e78b15 (job perf_steps3 15992827, 2026-11-22)

`int3/four` = 811e78b15 + tcu, dram, kernel, cache (34 commits) + 7 review-fix
commits; `int3/all` = `int3/four` + core re-derived on the fetch-ahead code (17
commits incl. the test `lsu_tail`). `fin/four` (b61383b1d) and `fin/all`
(2b7a03287) are the same trees with the co-author trailers corrected.
h200s now runs with FETCH_INFLIGHT=4, FFLAGS_ACCUM=1, BRANCH_REG=0, so its cycle
counts differ from section 5 by design; default-config counts do not.

Identity against 811e78b15 built in the same job: both stacks IDENTICAL on all
14 cases (`results/perf_s3_identity.txt`). Single runs (three streams in
parallel in one job, not interleaved: indicative only), wall seconds:

| case | cycles | base | four areas | speedup | all five | speedup |
|---|---|---|---|---|---|---|
| roof1 | 132800 | 21.5 | 3.3 | 6.51x | 3.1 | 6.86x |
| gb1 | 665219 | 41.9 | 19.4 | 2.16x | 16.2 | 2.58x |
| bw1 | 156327 | 7.8 | 5.0 | 1.58x | 3.5 | 2.24x |
| bw4 | 135366 | 45.6 | 29.6 | 1.54x | 19.0 | 2.40x |
| bw11 | 149242 | 172.9 | 97.6 | 1.77x | 65.8 | 2.63x |
| bw22 | 257106 | 681.2 | 359.7 | 1.89x | 218.5 | 3.12x |
| dxa4 | 69365 | 13.7 | 4.7 | 2.94x | 3.8 | 3.58x |
| dxa11 | 40096 | 18.8 | 7.2 | 2.62x | 5.3 | 3.56x |
| dxa22 | 29515 | 27.4 | 9.8 | 2.78x | 6.9 | 3.99x |
| gb22 | 1007093 | 2503.5 | 1240.7 | 2.02x | 817.4 | 3.06x |
| d_sgemm | 792351 | 2.5 | 2.2 | 1.13x | 1.9 | 1.28x |
| d_sgemm4 | 1453274 | 18.1 | 16.3 | 1.11x | 14.4 | 1.25x |
| d_tcu | 83506 | 0.4 | 0.3 | 1.31x | 0.3 | 1.41x |

Earlier, against e211fbbfc (job perf_steps2): the four-area stack IDENTICAL on
all 14 cases and each cumulative step (tcu; +dram; +kernel) on 8
(`results/perf_s2_identity.txt`).

## 8. Second review round and the fedp.h width question (2026-11-22)

Kernel/host (8 commits) and the 7 review-fix commits, reviewed statically
(`rev3-kernel`): every commit OK; nothing blocks (the four wrong co-author
trailers are corrected in `fin/*`). Points worth keeping:
- Event FIFOs keep the old order for every delay (0, 1..4095, multiples of
  4096, events scheduled while a bucket fires); the dense tick flags are written
  only by the owning domain or by the host while workers are parked; a RegSlice
  without credits is woken by the credit return, which runs before the scan.
- No simulated cycle count depends on host polling, before or after the
  runtime change (every hook that touches the processor waits on the kernel
  future; the CP engine sits in WaitDone during a kernel).
- The DRAM reset fix reproduces the original shared_ptr behaviour exactly.
Follow-ups, not blocking: a delivery that throws leaks the rest of the detached
event chain (error path only); the Makefile drift guard runs only when
windowed_controller.o is rebuilt (add the hashed files as prerequisites);
`linked_list.h` include now unused in simobject.h.
Pre-existing (base too): a DRAM response completing after a reset sends a stale
MemRsp and wraps the uint32 `inflight` from 0, so with a nonzero queue size
`can_accept()` stays false; the CP `cycle_counter_` (MMIO 0x010/0x014) is
host-poll driven (only the aved driver reads it).

fedp.h at h200s (`fedp-width`, harness under UBSan on the base header and the
optimized one, same result): SimX always builds TfrFedp with W = 25 and every
path evaluates 8 words per dot product, so nt = 17 (fp16/bf16: 16 products;
fp8: 32 products paired to 16 lanes) or 9 (tf32), HR_ = 5 or 4, Ww = 32 or 31.
Ww never exceeds 32, so the shifts by the type width are NOT reached and the
accumulator matches the RTL's 32-bit tree (VX_tcu_fedp_tfr.sv, static assert
27..32); measured worst case needs 32 signed bits, no overflow. There is no
headroom: more than 32 products per evaluation or W >= 30 takes Ww past 32.
`reduce_f8` IS reached with a negative shift count for fp8/bf8 (0.5-10% of pair
reductions, when exactly one product of a pair is zero and has the larger
exponent field); the shifted value is always 0, g++ 12 emits sar + cmov, and
the output hash is identical across -O0/-O3/UBSan and base/optimized: results
are bit-correct on x86 but rest on undefined behaviour. Minimal fix, not
applied (base bug, for the user): `if (diff >= 32 || diff < 0)` in reduce_f8,
and optionally `assert(Wa + HR_ <= 32)` in accumulate().

## 9. Head named for merge (2026-11-22)

`feature/simx-perf` = a2341f5f7 = `feature/dxa-borrowing` 811e78b15 + 59 commits
(tcu 4, dram 5, kernel/host 8, cache 17, review fixes 7, core 17 incl. the test
`lsu_tail`, one CI case), every commit with the Fable 5.2 co-author trailer and
the session line. Not pushed. Tag `perf-r1-on-18deea5da` keeps the stack the
round-1 timing table was taken on.

Evidence on this head (trees identical to `int3/all` up to the CI yaml line):
- 14 benchmark cases IDENTICAL to 811e78b15 built in the same job, h200s with
  FETCH_INFLIGHT=4 / FFLAGS_ACCUM=1 / BRANCH_REG=0, default cases with the knobs
  off (`results/perf_s3_identity.txt`).
- CI sweep: CORRECTED, see section 11. At the time of the merge only 265 of the
  310 cases had run (the 45 with an empty CONFIGS field were silently skipped
  by the runner and counted identical). After the fix and the rerun: 310 of 310
  IDENTICAL in exit code and output, 289 with a cycle count, 301 PASSED; 9
  OpenCL host cases do not build on this cluster on either side.
- `lsu_tail` with a load split over four packets (16 threads, 4 LSU lanes): 618
  cycles on both; under h200s: 2160 cycles on both (`results/perf_lsutail`).
- Reviews: section 6 and 8; nothing blocking remains.

## 10. After the merge: what is left on disk, and round 2

`feature/dxa-borrowing` was fast-forwarded to a2341f5f7 (the head named in
section 9). Cleanup done on 2026-11-22; kept for round 2:
- worktrees `wt_perf` (feature/simx-perf), `wt_perf_mt` (perf/mt, 7 commits on
  the first base 540b3dad2), `wt_perf_build2` (perf/build2, 5 commits on
  e211fbbfc + the four areas);
- source copies `src_perf_base` (third_party built; seed of every copy),
  `src_perf_i_b4` (811e78b15) and `src_perf_i_c4all` (the merged stack without
  the CI yaml line), both hardlinked to src_perf_base: remove with `rm -rf`
  only, never rewrite a file in place;
- staged binaries `perf/stage_i_b4`, `perf/stage_i_c4all` (14 cases each, for A/B);
- `perf/` tools, `perf/alloc` (jemalloc/mimalloc builds of the build agent),
  all `results/perf_*`, `slurm/perf_*`;
- all branches (perf/*, perf/*2, int/*, int2/*, int3/*, fin/*, tag
  perf-r1-on-18deea5da): none deleted.

Round 2, not started:
- MT: rebase perf/mt onto the merged head (conflict with the kernel changes in
  sim/common/simobject.h), address the review (TSan on the tip, identity at 11
  and 22 SMs, conservative `uncore_hoistable()`, the relaxed debug assert, the
  MPM CSR race), then measure scaling. No MT speedup has been measured yet.
- Build flags: perf/build2 (opt-in knobs; system-header includes); the build
  agent's measurement table was never delivered; re-measure LTO/PGO/-march and
  the allocators against the merged serial build.
- Follow-ups from the reviews (section 8) and the base defects listed in
  sections 6 and 8 (fedp.h reduce_f8, DRAM response after reset, MPM CSR reads
  under MT) for the user's decision.
- Not done in round 1: an interleaved timing table on the merged head (the
  5-alternation table is from 18deea5da), "tried and did not help" lists (the
  authoring agents were lost before reporting), simulator build time.

## 11. The sweep defect (found 2026-11-22 by the base-fix agent, after the merge)

`perf/sweep/run.sh` read case rows with `IFS=$'\t' read -r id cfg cmd`. Tab is
IFS whitespace, so a row with an empty CONFIGS field collapsed, the command was
lost, `bash -c ""` returned 0 with an empty log, and `cmp.sh` counted two empty
logs as identical. 45 of the 310 cases (the default-configuration ones:
flush_coherence, fence_mp, diverge, gbarrier, occupancy, dogfood, module_reload,
lmem-access, single_cta, demo, vecadd, ...) never ran; the stack was merged on a
claim of "310 identical" that was really 265.

Fixed: run.sh splits rows with parameter expansion, reruns any case without a
valid result, and records a malformed row, an empty log or a log without the
run marker as rc 97/98/99; cmp.sh reports such cases INVALID, counts how many
identical cases print cycles and PASSED, and exits non-zero unless different =
invalid = missing = 0; `perf/identical.sh` had the same weakness (two empty logs
passed) and now returns INVALID unless both logs are non-empty with a cycle
count and PASSED.

Rerun (job perf_sweep45 15998095; 811e78b15 vs an exact copy of a2341f5f7): all
45 IDENTICAL (`results/perf_sweep_w2_45.txt`); 16 of them cross-checked on the
interactive node. Whole sweep: 310 identical, 289 with cycles, 301 PASSED.
regression_module_reload-1 and regression_fence_mp-1..4 print no cycle count
(identity on program output only). 9 cases do not build on this cluster on
either side (host OpenCL headers missing): sw_image-{copy,linear,int,get,ff,
ff-linear,ff-bgra,hwsw}, sw_lbm-1.

Audit of the other evidence, recomputed from raw logs with the hardened
identical.sh: perf_s3 28/28, perf_measure_r1 14/14, perf_s2 14/14 and 3 x 8/8,
each with a cycle count, PASSED and 6-1256 compared lines. Rule adopted for
every comparison: it must fail when nothing ran.

## 12. Build mode of every number in this file (added 2026-11-22)

All benchmark builds (`perf/bench.sh`) and all sweep builds (blackbox with
`--perf=1` added by `sweep/gen.py`) were made with the make variable `PERF`
NOT exported; `-DPERF_ENABLE` reaches the simulator compile once, through
CONFIGS (checked in `simx_config.stamp`). Both sides of every comparison were
built the same way. SimX cycle counts differ between "PERF exported" and "not
exported" builds of the same source (root cause open on fix/perf-timing), so:
none of the evidence above covers the PERF-exported mode, and the sweep ran CI
cases with `--perf=1` even where CI runs them without (needed to print cycle
counts): the simulator built WITHOUT -DPERF_ENABLE was never compared.

## 13. The kernel-library build mode (root cause from the coordinator, 2026-11-22)

Section 12's "PERF" framing is superseded. Nothing in SimX reads PERF_ENABLE,
and in fresh trees cycle counts are equal with PERF exported or not (job
perf_steps_perf1: 8 cases identical base vs stack and equal to the PERF-unset
counts). What differs between build flows is the device KERNEL LIBRARY
(sw/kernel/libvortex*.a): `make -s software` builds it under whatever
PROFILE/CONFIGS are in the environment then, and the test Makefiles never
rebuild it. run_point.sbatch exports PROFILE before `make software` (library
built under the profile); a blackbox or bench.sh call with `--profile` after a
default `make software` links h200s tests against a default-configuration
library (e.g. without the VX_CFG_VM_ENABLE satp write in `__vx_cta_entry`).
h200s gb1 on 811e78b15: 665,219 in the mixed mode, 665,514 in the study mode.

ALL round-1 h200s evidence in this file (sections 5, 7, 9, 11) was taken in the
mixed mode: identical within it, but not the mode the study numbers use.
Default-configuration cases are unaffected.

Tools changed: `perf/bench.sh build` rebuilds sw/kernel under the case's
PROFILE (CONFIGS unset, as run_point does) and records PERF_env, the count of
-DPERF_ENABLE in the simulator flags, klib_profile and klib_md5 in case.txt,
plus simx_config.stamp; `perf/sweep/run.sh` rebuilds it under each case's
--profile and CONFIGS (SWEEP_KLIB=case; =ci keeps plain-CI behaviour). Cases
bar1 (bar_vis), gtm1 (gemm_tmem), gtd1 (gemm_tmem_decode) added.

Consistent-mode evidence: job perf_measure3 16000477 (811e78b15 vs a2341f5f7,
17 cases, 5 alternations: identity + timing; results/perf_measure_r3, build
records under build_{base,opt}/<case>/).

Correction (same day): rebuilding only sw/kernel under the profile did not
reach the study mode (check: gb1 665,219 and bar_vis 165,633, the mixed-mode
values, instead of 665,514 / 154,182). `configure` reads PROFILE from the
environment, bakes it into config.mk and generates sw/VX_config.h under it, and
`make software` builds every once-built component under it. `perf/bench.sh`
now builds a profiled case in its own tree `<build_dir>__<profile>`, configured
and built with PROFILE exported; case.txt records tree, tree_profile,
vxconfig_md5, klib_md5, stub_md5. The sweep runner defaults to plain-CI
behaviour again. Consistent-mode job resubmitted as perf_measure3 16000666.

## 14. Merged stack in the study build mode (job perf_measure3 16000666, 2026-11-22)

811e78b15 vs a2341f5f7, fresh trees; every h200s case built in a tree configured
and `make software`'d with PROFILE=h200s exported (case.txt: tree_profile=h200s;
md5 of sw/VX_config.h and of the kernel library equal on both sides), default
cases in a plain tree; PERF not exported. Mode check: gb1 = 665,514 and bar_vis
= 154,182 on both (the run_point.sbatch numbers). Xeon Gold 6226, node
atl1-1-02-018-23-2, 12 cpus of a shared node at load 13-21; base and merged
alternating 5 times (gb22 one pair); median wall (min), median user CPU.

Identity (hardened identical.sh, first alternation): IDENTICAL on all 17 cases,
each with a cycle count, PASSED and PERF lines (`results/perf_measure_r3/identity.txt`;
build records under `build_{base,opt}/<case>/`).

| case | cycles | n | base wall med (min) s | opt wall med (min) s | wall speedup | base user s | opt user s | user speedup | base kcyc/s | opt kcyc/s | RSS MB b/o |
|---|---|---|---|---|---|---|---|---|---|---|---|
| bar1 | 154182 | 5/5 | 6.5 (6.5) | 3.2 (3.2) | 2.03x | 6.8 | 3.2 | 2.15x | 23.65 | 48.03 | 56/58 |
| bw1 | 107249 | 5/5 | 8.5 (8.3) | 3.7 (3.7) | 2.30x | 8.9 | 3.6 | 2.46x | 12.63 | 29.06 | 50/51 |
| bw11 | 132106 | 5/5 | 233.4 (172.3) | 80.1 (75.9) | 2.91x | 247.5 | 79.5 | 3.11x | 0.57 | 1.65 | 121/128 |
| bw22 | 259697 | 5/5 | 670.8 (480.0) | 217.9 (184.7) | 3.08x | 712.9 | 216.5 | 3.29x | 0.39 | 1.19 | 191/205 |
| bw4 | 109307 | 5/5 | 45.8 (43.7) | 19.9 (19.4) | 2.30x | 48.5 | 19.8 | 2.46x | 2.39 | 5.48 | 77/80 |
| d_sgemm | 792351 | 5/5 | 2.4 (2.4) | 1.9 (1.9) | 1.24x | 2.5 | 1.9 | 1.32x | 332.92 | 412.68 | 12/12 |
| d_sgemm4 | 1453274 | 5/5 | 18.2 (18.2) | 14.5 (14.4) | 1.26x | 19.3 | 14.5 | 1.34x | 79.68 | 100.09 | 15/15 |
| d_tcu | 83506 | 5/5 | 0.3 (0.3) | 0.2 (0.2) | 1.40x | 0.4 | 0.2 | 1.50x | 238.59 | 334.02 | 13/13 |
| dxa11 | 38916 | 5/5 | 20.9 (19.6) | 5.7 (5.7) | 3.66x | 21.9 | 5.6 | 3.92x | 1.86 | 6.80 | 99/109 |
| dxa22 | 29980 | 5/5 | 31.2 (25.9) | 7.3 (7.2) | 4.27x | 33.0 | 7.2 | 4.61x | 0.96 | 4.10 | 145/162 |
| dxa4 | 67926 | 5/5 | 14.6 (14.3) | 4.1 (4.0) | 3.60x | 15.4 | 4.0 | 3.87x | 4.65 | 16.73 | 68/73 |
| gb1 | 665514 | 5/5 | 43.0 (42.8) | 16.8 (16.8) | 2.56x | 46.6 | 18.0 | 2.59x | 15.49 | 39.61 | 83/84 |
| gb22 | 1002693 | 1/1 | 3293.1 (3293.1) | 835.9 (835.9) | 3.94x | 3468.3 | 879.6 | 3.94x | 0.30 | 1.20 | 720/739 |
| gtd1 | 974462 | 5/5 | 224.9 (224.2) | 44.2 (43.9) | 5.09x | 237.2 | 43.9 | 5.41x | 4.33 | 22.05 | 216/221 |
| gtm1 | 613318 | 5/5 | 177.4 (177.3) | 31.2 (31.0) | 5.69x | 187.8 | 31.0 | 6.05x | 3.46 | 19.65 | 104/109 |
| roof1 | 132608 | 5/5 | 21.5 (21.5) | 3.1 (3.1) | 6.95x | 22.9 | 3.1 | 7.45x | 6.16 | 42.78 | 42/43 |

This table supersedes section 5 and 7 as the before/after of the merged stack.

## 15. Multi-threaded SimX: parked (round 2, agent mt2, 2026-11-22)

Stopped at the user's decision (MT not needed). Kept: branch `perf/mt3`
(fe67a648e) = d1e537603 + 15 commits, not pushed, not merged, no worktree.
Deleted: perf/mt, perf/mt2, perf/mt2r and their worktrees, every
`src_perf_i_mt2*`/`mt3*` copy and `perf/stage_i_mt*` stage. Tools kept in
`perf/mt2/` (build.sh, run1.sh, matrix.sh, banner.sh, buildmode.sh, *.sbatch).

What the branch holds:
- The 7 commits of the first perf/mt re-applied: boundary audit, the
  uncore-first schedule, a futex barrier, VORTEX_SIMX_THREADS, and the split
  in which the uncore ends of the unregistered edges (the L2 TLB) tick alone
  before the rest of the uncore ticks next to the sockets. One commit was
  re-derived for the dense tick flags.
- A conservative hoist check (fan-in, scan order, delivery interleaving with
  tx callbacks, event links and credit returns, lead objects must tick). It
  falls back to "whole uncore first", or serial with one socket, and prints
  one `SIMX-MT:` line naming the schedule.
- Endpoint- and phase-precise access checks: debug builds or
  -DSIMX_MT_CHECKS.
- Endpoint pre-resolution.
- The MPM CSR race fix (processor statistics gathered only in the L3CACHE
  and MEM classes; a kernel's own csrr sees the base class).
- VORTEX_SIMX_THREADS notices, comments and docs, and the test `mpm_csr`
  with CI case simx_mt-2.
- A decimal mode line. MT is opt-in: SIMX_MT defaults to 0.

Identity (perf/mt2r = bb5d16d87 + 14 of these commits, bench.sh per-profile
trees, jobs 16000506/16000508/16000509):
- Serial tip = serial base on all 17 cases (mode check gb1 665,514, bar1
  154,182).
- MT matrix: 17 cases x 1/2/4/8/16 threads x 5 repeats = 425 runs, all
  IDENTICAL to the serial base log. Each of the 340 runs asking for 2 or
  more threads printed a parallel `SIMX-MT: <n> threads` banner; none fell
  back. The cap is one thread per socket plus the uncore.
- d_dogfood exits 1 in every run, base included (result errors, a base
  failure).
- mpm_csr (default 4 cores + L2, and h200s 4 cores): serial = MT at 4 and 16
  threads. Results: `results/perf_mt2_a`, `results/perf_mt2_mx/matrix.txt`.

Median wall s, threads 1/2/4/8/16 (job 16000508: one run at a time on 18
cpus; gb22 from 16000509: two streams at once on 24 cpus, so contended):

| case | 1 | 2 | 4 | 8 | 16 | best |
|---|---|---|---|---|---|---|
| bw4 | 17.9 | 11.3 | 5.1 | 3.5 | 3.5 | 5.1x |
| bw11 | 65.7 | 44.6 | 16.9 | 15.8 | 16.1 | 4.2x |
| bw22 | 193.9 | 127.5 | 59.9 | 55.9 | 58.7 | 3.5x |
| gb22 | 744 | 552 | 257 | 223 | 241 | 3.3x |
| dxa4 | 3.7 | 2.2 | 2.0 | 1.9 | 2.0 | 1.9x |
| dxa11 | 5.2 | 2.5 | 2.0 | 2.0 | 2.0 | 2.6x |
| dxa22 | 6.6 | 3.8 | 2.3 | 2.3 | 2.5 | 2.9x |
| d_sgemm4 | 14.2 | 16.0 | 10.7 | 8.3 | 8.2 | 1.7x |
| gb1 / gtm1 / gtd1 / bw1 (1 SM, 2 threads) | | | | | | 1.12 / 1.13 / 1.15 / 1.41x |

roof1 and bar1 gain nothing. d_sgemm, d_dogfood and d_tcu are slower at 2
threads (x0.6-0.7). User CPU rises steeply past 4-8 threads (barrier
spinning).

Never verified:
- ThreadSanitizer: the TSan build failed on atomic_thread_fence in
  sw/runtime/common/device.cpp under -Werror=tsan. -Wno-tsan was added but
  the rerun was cancelled. As a result the SIMX_MT_CHECKS asserts and the
  boundary audit never ran either.
- The 310-case CI sweep, base vs tip.
- An independent review of the 15 commits.
- The rerun on d1e537603 (perf/mt3 itself was never built).
- A 32-SM point, a serial-fraction profile, scaling on a quiet node.

Base defects found, not fixed:
- No SimX build without -DNDEBUG (DEBUG=1) compiles: sw/common/util.h
  defines a 2-argument `__assert` macro, and glibc's <assert.h>, re-included
  through <cassert> in sim/common/simobject.h after util.h, declares a
  3-argument `__assert`.
- sim/simx/gfx_doctrine.h:142, a once-only `static bool warned` in decode:
  under MT the warning can print twice (graphics builds only).
