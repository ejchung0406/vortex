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
