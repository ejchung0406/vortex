// E3: a memory-bound stream sharing SMs with a compute-bound kernel, in one
// process on two CUDA streams (no MPS).
//
//   borrow_corun --mem tma|load [--mem-blocks B] [--mem-ms M] --cp fma|gemm
//                [--fma-smem-kb S] [--gemm-n N] [--depth D] [--reps R]
//                [--carveout PCT] [--cp-mult X]
// --carveout: cudaFuncAttributePreferredSharedMemoryCarveout for our kernels
// (default 100 = max shared; -1 leaves the driver default, under which an FMA
// block with 140 KB left no room for a 65 KB TMA block beside it).
// --cp-mult: in (A) the compute job is sized to X times the memory job's solo time.
//
// Memory job:
//   tma : borrow_kernel with cluster size 1: one producer thread issuing 16 KB
//         cp.async.bulk copies at depth D into its own shared memory and 4
//         consumer warps reading every byte (the E2(b) consumer, no helpers).
//         160 threads, 1 KB + D x 16 KB dynamic shared memory.
//   tmafused: fused_kernel (one thread issues and checks, no consumer warps;
//         write-only, the first and last word of every tile are checked).
//   load: corun.cu's stream kernel (1024 threads, coalesced float loads,
//         4 outstanding per thread), no shared memory.
//   B blocks, each streaming its own part of a 16 GiB buffer (wrapping), sized
//   to run about M ms alone.
// Compute job:
//   fma : corun.cu's register-only multiply-add kernel, 1024 threads per block,
//         one block per SM (S KB of dynamic shared memory, default 140, so two
//         cannot share an SM), grid = number of SMs.
//   gemm: cublasGemmEx FP16 x FP16 -> FP16, FP32 accumulate, N x N x N.
// Protocol: each alone; then (A) compute started first and sized to outlast
// the memory job -> memory job's co-run rate; (B) memory job started first and
// sized to outlast the compute job -> compute job's co-run time. The SM of
// every memory-job block and every FMA block is read back.
#include "borrow_tma.cuh"
#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)
#define CB(x) do { cublasStatus_t s_ = (x); if (s_ != CUBLAS_STATUS_SUCCESS) { \
  fprintf(stderr, "%s:%d cublas %d\n", __FILE__, __LINE__, (int)s_); exit(1); } } while (0)

__global__ void init_buf(uint32_t* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
    a[i] = (uint32_t)i;
}
__global__ void init_half(__half* a, size_t n) {
  for (size_t i = blockIdx.x * (size_t)blockDim.x + threadIdx.x; i < n; i += (size_t)gridDim.x * blockDim.x)
    a[i] = __float2half((float)((i * 2654435761u) >> 24) / 256.f - 0.5f);
}

// corun.cu stream kernel (U = 4), plus SM id and wrap-around passes
__global__ void stream4(const float* __restrict__ a, size_t per_block, int passes, float* out, int* smid,
                        unsigned* started) {
  const int t = threadIdx.x, T = blockDim.x;
  if (t == 0) atomicAdd(started, 1u);
  const float* p = a + (size_t)blockIdx.x * per_block + t;
  const size_t n = per_block / T;
  float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
  for (int r = 0; r < passes; ++r)
    for (size_t i = 0; i + 4 <= n; i += 4) {
      s0 += p[i * T]; s1 += p[(i + 1) * T]; s2 += p[(i + 2) * T]; s3 += p[(i + 3) * T];
    }
  if (t == 0) { out[blockIdx.x] = s0 + s1 + s2 + s3; smid[blockIdx.x] = (int)sm_id(); }
}

// holds its stream until `target` CTAs of the other kernel have started (or
// until `min_ns` have passed when target == 0)
__global__ void gate(const volatile unsigned* counter, unsigned target, unsigned long long min_ns) {
  const unsigned long long t0 = gtimer();
  if (target) while (*counter < target) {}
  else while (gtimer() - t0 < min_ns) {}
}

// corun.cu compute kernel, plus SM id, start counter and per-block times
__global__ void muladd(unsigned long long iters, float mul, float add, float* out, int* smid, unsigned* started,
                       unsigned long long* bt) {
  unsigned long long t0 = 0;
  if (threadIdx.x == 0) { atomicAdd(started, 1u); t0 = gtimer(); }
  float a0 = 0.f, a1 = 0.f, a2 = 0.f, a3 = 0.f;
  for (unsigned long long i = 0; i < iters; i += 4) {
    a0 = a0 * mul + add; a1 = a1 * mul + add; a2 = a2 * mul + add; a3 = a3 * mul + add;
  }
  __syncthreads();
  if (threadIdx.x == 0) {
    out[blockIdx.x] = a0 + a1 + a2 + a3; smid[blockIdx.x] = (int)sm_id();
    bt[2 * blockIdx.x] = t0; bt[2 * blockIdx.x + 1] = gtimer();
  }
}

static float ms(cudaEvent_t a, cudaEvent_t b) { float t; CK(cudaEventElapsedTime(&t, a, b)); return t; }

int main(int argc, char** argv) {
  std::string mem = "tma", cp = "fma";
  int mem_blocks = 0, fma_smem_kb = 140, gemm_n = 8192, D = 4, reps = 3, carveout = 100, cp_mult = 15;
  double mem_ms = 40;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--mem")) mem = argv[++i];
    else if (is("--cp")) cp = argv[++i];
    else if (is("--mem-blocks")) mem_blocks = atoi(argv[++i]);
    else if (is("--mem-ms")) mem_ms = atof(argv[++i]);
    else if (is("--fma-smem-kb")) fma_smem_kb = atoi(argv[++i]);
    else if (is("--gemm-n")) gemm_n = atoi(argv[++i]);
    else if (is("--depth")) D = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--carveout")) carveout = atoi(argv[++i]);
    else if (is("--cp-mult")) cp_mult = atoi(argv[++i]);
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  cudaDeviceProp prop;
  CK(cudaGetDeviceProperties(&prop, 0));
  const int sms = prop.multiProcessorCount;
  if (mem_blocks == 0) mem_blocks = sms;
  const size_t gib16 = 16ull << 30, elems = gib16 / 4;
  uint32_t* buf;
  CK(cudaMalloc(&buf, gib16));
  init_buf<<<sms * 4, 1024>>>(buf, elems);
  CK(cudaDeviceSynchronize());
  cudaStream_t sm_, sc_;
  CK(cudaStreamCreateWithFlags(&sm_, cudaStreamNonBlocking));
  CK(cudaStreamCreateWithFlags(&sc_, cudaStreamNonBlocking));
  cudaEvent_t m0, m1, c0, c1;
  CK(cudaEventCreate(&m0)); CK(cudaEventCreate(&m1)); CK(cudaEventCreate(&c0)); CK(cudaEventCreate(&c1));
  int *mem_sm, *fma_sm;
  CK(cudaMallocManaged(&mem_sm, sms * 64 * sizeof(int)));
  CK(cudaMallocManaged(&fma_sm, sms * sizeof(int)));
  unsigned *mem_started, *fma_started;
  CK(cudaMalloc(&mem_started, 4)); CK(cudaMalloc(&fma_started, 4));
  unsigned long long* fbt;
  CK(cudaMallocManaged(&fbt, sms * 16));

  // ---------------- memory job
  BorrowParams p = {};
  const uint32_t tile = 16384;
  const size_t tma_smem = BORROW_BAR_BYTES + (size_t)D * tile;
  unsigned long long *sums, *times; unsigned* errs; long long* cyc;
  CK(cudaMallocManaged(&sums, mem_blocks * 8)); CK(cudaMallocManaged(&times, mem_blocks * 16));
  CK(cudaMallocManaged(&errs, mem_blocks * 4)); CK(cudaMallocManaged(&cyc, mem_blocks * 8));
  p.src = buf; p.window_tiles = gib16 / tile; p.tile_bytes = tile; p.D = D; p.self_issue = 1; p.nhelpers = 0;
  p.consume = 1; p.nw = D % 4 == 0 ? 4 : D % 3 == 0 ? 3 : D % 2 == 0 ? 2 : 1;  // nw must divide the D slots
  p.started = mem_started; p.sums = sums; p.times = times; p.errs = errs; p.cycles = cyc; p.smid = mem_sm;
  p.cluster_stride = p.window_tiles / mem_blocks;
  CK(cudaFuncSetAttribute(borrow_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)tma_smem));
  cudaLaunchConfig_t cfg = {};
  cudaLaunchAttribute at[1];
  at[0].id = cudaLaunchAttributeClusterDimension;
  at[0].val.clusterDim.x = 1; at[0].val.clusterDim.y = 1; at[0].val.clusterDim.z = 1;
  cfg.gridDim = dim3(mem_blocks); cfg.blockDim = dim3(32 * (1 + p.nw)); cfg.dynamicSmemBytes = tma_smem;
  cfg.attrs = at; cfg.numAttrs = 1; cfg.stream = sm_;
  float* out; CK(cudaMalloc(&out, sms * 64 * sizeof(float)));
  const size_t per_block = (elems / mem_blocks) & ~(size_t)4095;
  int passes = 1;
  double mem_bytes = 0;
  auto launch_mem = [&]() {
    if (mem == "tma") CK(cudaLaunchKernelEx(&cfg, borrow_kernel, p));
    else if (mem == "tmafused") fused_kernel<<<mem_blocks, 32, tma_smem, sm_>>>(p);
    else stream4<<<mem_blocks, 1024, 0, sm_>>>((const float*)buf, per_block, passes, out, mem_sm, mem_started);
    CK(cudaGetLastError());
  };
  auto set_mem_len = [&](double target_ms) {
    // calibrate with a short run, then size to target_ms
    if (mem != "load") { p.T = 2048; } else { passes = 1; }
    for (int k = 0; k < 2; ++k) {
      CK(cudaEventRecord(m0, sm_)); launch_mem(); CK(cudaEventRecord(m1, sm_)); CK(cudaStreamSynchronize(sm_));
    }
    const double t = ms(m0, m1);
    if (mem != "load") {
      p.T = (uint32_t)std::max(64.0, 2048 * target_ms / t);
      mem_bytes = (double)p.T * tile * mem_blocks;
    } else {
      passes = std::max(1, (int)(target_ms / t + 0.5));
      mem_bytes = (double)per_block * 4 * mem_blocks * passes;
    }
  };
  int mem_occ = 0;
  CK(cudaFuncSetAttribute(fused_kernel, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)tma_smem));
  if (mem == "tmafused") { p.consume = 0; CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&mem_occ, fused_kernel, 32, tma_smem)); }
  else if (mem == "tma") CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&mem_occ, borrow_kernel, 32 * (1 + p.nw), tma_smem));
  else CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&mem_occ, stream4, 1024, 0));

  // ---------------- compute job
  const size_t fma_smem = (size_t)fma_smem_kb * 1024;
  CK(cudaFuncSetAttribute(muladd, cudaFuncAttributeMaxDynamicSharedMemorySize, (int)fma_smem));
  // every kernel asks for the largest shared-memory carveout, so the SM's
  // carveout chosen for one kernel leaves room for the other's blocks
  if (carveout >= 0) {
    CK(cudaFuncSetAttribute(muladd, cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
    CK(cudaFuncSetAttribute(borrow_kernel, cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
    CK(cudaFuncSetAttribute(stream4, cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
    CK(cudaFuncSetAttribute(gate, cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
    CK(cudaFuncSetAttribute(fused_kernel, cudaFuncAttributePreferredSharedMemoryCarveout, carveout));
  }
  unsigned long long iters = 1 << 20;
  cublasHandle_t h = nullptr;
  __half *A = nullptr, *B = nullptr, *Cm = nullptr;
  int gemm_count = 1;
  if (cp == "gemm") {
    CB(cublasCreate(&h)); CB(cublasSetStream(h, sc_));
    const size_t nn = (size_t)gemm_n * gemm_n;
    CK(cudaMalloc(&A, nn * 2)); CK(cudaMalloc(&B, nn * 2)); CK(cudaMalloc(&Cm, nn * 2));
    init_half<<<sms * 4, 1024>>>(A, nn); init_half<<<sms * 4, 1024>>>(B, nn);
    CK(cudaDeviceSynchronize());
  }
  auto launch_cp = [&]() {
    if (cp == "fma") muladd<<<sms, 1024, fma_smem, sc_>>>(iters, 1.f, 1.f, out + sms, fma_sm, fma_started, fbt);
    else {
      const float alpha = 1.f, beta = 0.f;
      for (int g = 0; g < gemm_count; ++g)
        CB(cublasGemmEx(h, CUBLAS_OP_N, CUBLAS_OP_N, gemm_n, gemm_n, gemm_n, &alpha, A, CUDA_R_16F, gemm_n, B,
                        CUDA_R_16F, gemm_n, &beta, Cm, CUDA_R_16F, gemm_n, CUBLAS_COMPUTE_32F, CUBLAS_GEMM_DEFAULT));
    }
    CK(cudaGetLastError());
  };
  auto set_cp_len = [&](double target_ms) {
    if (cp == "fma") iters = 1 << 20; else gemm_count = 1;
    for (int k = 0; k < 2; ++k) {
      CK(cudaEventRecord(c0, sc_)); launch_cp(); CK(cudaEventRecord(c1, sc_)); CK(cudaStreamSynchronize(sc_));
    }
    const double t = ms(c0, c1);
    if (cp == "fma") { iters = (unsigned long long)((1 << 20) * target_ms / t); iters -= iters % 4; }
    else gemm_count = std::max(1, (int)(target_ms / t + 0.5));
  };
  auto cp_work = [&]() {  // FMA: block-iterations; GEMM: flop
    return cp == "fma" ? (double)iters * sms : 2.0 * gemm_n * (double)gemm_n * gemm_n * gemm_count;
  };
  int fma_occ = 0;
  CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&fma_occ, muladd, 1024, fma_smem));

  auto check_mem = [&]() {
    if (mem == "load") return std::string("n/a");
    int bad = 0;
    for (int c = 0; c < mem_blocks; ++c) bad += errs[c] || (uint32_t)sums[c] != borrow_expected(p, c);
    return std::string(bad ? "FAIL" : "ok");
  };
  auto reset_mem = [&]() { if (mem != "load") { memset(sums, 0, mem_blocks * 8); memset(errs, 0, mem_blocks * 4); } };

  // ---- solo runs
  set_mem_len(mem_ms);
  double mem_solo = 1e30;
  std::string chk_solo;
  for (int r = 0; r < reps; ++r) {
    reset_mem();
    CK(cudaEventRecord(m0, sm_)); launch_mem(); CK(cudaEventRecord(m1, sm_)); CK(cudaDeviceSynchronize());
    mem_solo = std::min(mem_solo, (double)ms(m0, m1));
    chk_solo = check_mem();
  }
  const int mem_distinct_solo = (int)std::set<int>(mem_sm, mem_sm + mem_blocks).size();
  set_cp_len(3 * mem_solo);
  double cp_solo = 1e30, fb_solo = 0;
  for (int r = 0; r < reps; ++r) {
    CK(cudaMemset(fma_started, 0, 4));
    CK(cudaEventRecord(c0, sc_)); launch_cp(); CK(cudaEventRecord(c1, sc_)); CK(cudaDeviceSynchronize());
    cp_solo = std::min(cp_solo, (double)ms(c0, c1));
    if (cp == "fma") {
      std::vector<double> v;
      for (int b = 0; b < sms; ++b) v.push_back((fbt[2 * b + 1] - fbt[2 * b]) * 1e-6);
      std::sort(v.begin(), v.end()); fb_solo = v[v.size() / 2];
    }
  }
  // ---- (A) compute first, memory job inside it
  double mem_co = 1e30, a_cp = 0, a_offset = 0;
  std::string chk_co;
  int shared_sms = -1;
  // the memory job is launched 5 ms after the compute job (host sleep), so the
  // compute job's blocks are resident first (a device-side gate kernel on the
  // memory stream did not start until the FMA kernel had finished)
  set_cp_len(cp_mult * mem_solo);
  for (int r = 0; r < reps; ++r) {
    reset_mem();
    CK(cudaMemset(fma_started, 0, 4)); CK(cudaMemset(mem_started, 0, 4)); CK(cudaDeviceSynchronize());
    CK(cudaEventRecord(c0, sc_)); launch_cp(); CK(cudaEventRecord(c1, sc_));
    cudaStreamQuery(sc_);  // flush the launch
    std::this_thread::sleep_for(std::chrono::milliseconds(5));  // let the compute job fill the SMs first
    CK(cudaEventRecord(m0, sm_)); launch_mem(); CK(cudaEventRecord(m1, sm_));
    CK(cudaDeviceSynchronize());
    if (ms(m0, m1) < mem_co) { mem_co = ms(m0, m1); a_cp = ms(c0, c1); a_offset = ms(c0, m0); }
    chk_co = check_mem();
  }
  const bool mem_inside_a = a_offset >= 0 && a_offset + mem_co <= a_cp;
  if (cp == "fma") {
    std::set<int> f(fma_sm, fma_sm + sms);
    shared_sms = 0;
    for (int b = 0; b < mem_blocks; ++b) shared_sms += f.count(mem_sm[b]);
  }
  const int fma_distinct = cp == "fma" ? (int)std::set<int>(fma_sm, fma_sm + sms).size() : -1;
  const int mem_distinct_co = (int)std::set<int>(mem_sm, mem_sm + mem_blocks).size();
  // ---- (B) memory job first (long), compute inside it
  const double mem_bytes_short = mem_bytes;
  set_mem_len(3 * cp_solo + 10);
  double cp_co = 1e30, b_mem = 0, b_offset = 0;
  set_cp_len(3 * mem_solo);  // back to the solo-run length
  std::vector<double> fb_shared, fb_other;
  for (int r = 0; r < reps; ++r) {
    reset_mem();
    CK(cudaMemset(fma_started, 0, 4)); CK(cudaMemset(mem_started, 0, 4)); CK(cudaDeviceSynchronize());
    CK(cudaEventRecord(m0, sm_)); launch_mem(); CK(cudaEventRecord(m1, sm_));
    gate<<<1, 1, 0, sc_>>>(mem_started, mem_blocks, 0);
    CK(cudaEventRecord(c0, sc_)); launch_cp(); CK(cudaEventRecord(c1, sc_));
    CK(cudaDeviceSynchronize());
    if (ms(c0, c1) < cp_co) {
      cp_co = ms(c0, c1); b_mem = ms(m0, m1); b_offset = ms(m0, c0);
      if (cp == "fma") {
        std::set<int> msms(mem_sm, mem_sm + mem_blocks);
        fb_shared.clear(); fb_other.clear();
        for (int b = 0; b < sms; ++b)
          (msms.count(fma_sm[b]) ? fb_shared : fb_other).push_back((fbt[2 * b + 1] - fbt[2 * b]) * 1e-6);
      }
    }
  }
  auto med = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
  const bool cp_inside_b = b_offset >= 0 && b_offset + cp_co <= b_mem;
  const double cp_unit_solo = cp == "fma" ? cp_work() / cp_solo / 1e6 : cp_work() / cp_solo / 1e9;  // Giter/s or TFLOP/s
  const double cp_unit_co = cp == "fma" ? cp_work() / cp_co / 1e6 : cp_work() / cp_co / 1e9;
  printf("gpu=%s test=corun mechanism=same_process_two_streams carveout=%d mem=%s cp=%s mem_blocks=%d depth=%d mem_occ_per_sm=%d "
         "fma_smem_kb=%d fma_occ_per_sm=%d gemm_n=%d gemm_count=%d "
         "mem_distinct_sms_solo=%d mem_distinct_sms_co=%d fma_distinct_sms=%d mem_blocks_on_fma_sms=%d "
         "check_solo=%s check_co=%s mem_solo_gbs=%.1f mem_co_gbs=%.1f mem_ratio=%.3f mem_inside_cp=%d "
         "cp_solo_ms=%.2f cp_co_ms=%.2f cp_ratio=%.3f cp_inside_mem=%d cp_rate_solo=%.2f cp_rate_co=%.2f cp_rate_unit=%s "
         "fma_block_ms_solo_med=%.2f fma_block_ms_co_on_mem_sms_med=%.2f (n=%zu) fma_block_ms_co_other_sms_med=%.2f (n=%zu) "
         "a_cp_ms=%.2f a_mem_start_offset_ms=%.2f\n",
         prop.name, carveout, mem.c_str(), cp.c_str(), mem_blocks, D, mem_occ, fma_smem_kb, fma_occ, gemm_n, gemm_count,
         mem_distinct_solo, mem_distinct_co, fma_distinct, shared_sms, chk_solo.c_str(), chk_co.c_str(),
         mem_bytes_short / (mem_solo * 1e6), mem_bytes_short / (mem_co * 1e6), mem_solo / mem_co, (int)mem_inside_a,
         cp_solo, cp_co, cp_solo / cp_co, (int)cp_inside_b, cp_unit_solo, cp_unit_co,
         cp == "fma" ? "Giter_per_s(block-iters)" : "TFLOP/s", fb_solo, med(fb_shared), fb_shared.size(),
         med(fb_other), fb_other.size(), a_cp, a_offset);
  return 0;
}
