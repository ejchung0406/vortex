// Shared host/device helpers for the go/no-go experiments (gng_*.cu).
//  - sticky per-thread TPC mask: the libsmctrl QMD/TMD pre-upload callback of
//    tpc_smctrl.h, but the mask stays set for every launch from the calling
//    host thread until changed (needed for back-to-back cuBLASLt/CUTLASS
//    launches; tpc_smctrl.h applies a mask to one launch only)
//  - NVML sampler thread (SM clock, memory clock, power, clock-event
//    reasons) every --nvml-ms, summarised over a window
//  - host realtime <-> %globaltimer offset
//  - TPC map file ("bit <b> sms <s..>" lines, written by tpc_lab probe)
#pragma once
#include <cuda.h>
#include <cuda_runtime.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

typedef unsigned __int128 u128;
#define GNG_MAXSM 256

// ------------------------------------------------------------------ device helpers
__device__ __forceinline__ uint32_t gng_smid() { uint32_t r; asm volatile("mov.u32 %0, %%smid;" : "=r"(r)); return r; }
__device__ __forceinline__ unsigned long long gng_gt() {
  unsigned long long t; asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(t)); return t;
}
__global__ void gng_k_stamp(unsigned long long* out) { if (threadIdx.x == 0) *out = gng_gt(); }

// ------------------------------------------------------------------ host time
static inline int64_t host_ns() { timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec; }
static inline void sleep_until_host(int64_t t) {
  for (;;) { int64_t d = t - host_ns(); if (d <= 0) return; usleep((useconds_t)std::min<int64_t>(d / 1000, 20000)); }
}
// gt - host: median of 7 round trips
static int64_t g_gt_off = 0;
static void calibrate_gt() {
  unsigned long long* d; CK(cudaMalloc(&d, 8));
  std::vector<int64_t> v;
  for (int i = 0; i < 7; ++i) {
    const int64_t a = host_ns();
    gng_k_stamp<<<1, 32>>>(d); CK(cudaDeviceSynchronize());
    const int64_t b = host_ns();
    unsigned long long g; CK(cudaMemcpy(&g, d, 8, cudaMemcpyDeviceToHost));
    v.push_back((int64_t)g - (a + b) / 2);
  }
  std::sort(v.begin(), v.end()); g_gt_off = v[3];
  CK(cudaFree(d));
}
static inline uint64_t host_to_gt(int64_t h) { return (uint64_t)(h + g_gt_off); }
static inline int64_t gt_to_host(uint64_t g) { return (int64_t)g - g_gt_off; }

// ------------------------------------------------------------------ sticky TPC mask
namespace gngm {
static const CUuuid cb_id = {{0x2c, (char)0x8e, 0x0a, (char)0xd8, 0x07, 0x10, (char)0xab, 0x4e,
                              (char)0x90, (char)0xdd, 0x54, 0x71, (char)0x9f, (char)0xe5, (char)0xf7, 0x4b}};
static std::once_flag g_once;
static thread_local bool t_on = false;
static thread_local u128 t_disable = 0;
static std::atomic<long> g_cb{0}, g_applied{0};
static void callback(void*, int, int, const void* in) {
  ++g_cb;
  if (!t_on) return;
  if (*(const uint32_t*)in < 5 * sizeof(void*)) return;
  char* tmd = (char*)*((void* const*)in + 4);
  if (!tmd) return;
  const uint8_t ver = *(uint8_t*)(tmd + 72);
  const u128 m = t_disable;
  if (ver >= 0x40) {
    uint32_t* w = (uint32_t*)(tmd + 304);
    w[0] = (uint32_t)m; w[1] = (uint32_t)(m >> 32); w[2] = (uint32_t)(m >> 64); w[3] = (uint32_t)(m >> 96);
    *(uint32_t*)tmd |= 0x80000000u;
  } else if (ver >= 0x16) {
    *(uint32_t*)(tmd + 84) = (uint32_t)m; *(uint32_t*)(tmd + 88) = (uint32_t)(m >> 32);
  } else return;
  ++g_applied;
}
static void setup() {
  std::call_once(g_once, [] {
    cuInit(0);
    uintptr_t* tbl = nullptr;
    if (cuGetExportTable((const void**)&tbl, &cb_id) != CUDA_SUCCESS || !tbl) { fprintf(stderr, "gng mask: no export table\n"); exit(1); }
    int (*sub)(uint32_t*, void (*)(void*, int, int, const void*), void*) = (decltype(sub))tbl[3];
    int (*ena)(uint32_t, uint32_t, int, int) = (decltype(ena))tbl[6];
    uint32_t h = 0;
    if (sub(&h, callback, nullptr) || ena(1, h, 0xb, 0x1)) { fprintf(stderr, "gng mask: callback setup failed\n"); exit(1); }
  });
}
}  // namespace gngm
// enabled TPC set for every later launch from this host thread; 0 = unmasked
static inline void gng_mask(u128 enabled) {
  // install the driver callback only when a mask is first needed, so that unmasked runs can use
  // CUPTI (which takes the same callback subscriber slot)
  if (enabled == 0 && !gngm::t_on) return;
  gngm::setup();
  gngm::t_on = enabled != 0;
  gngm::t_disable = ~enabled;
}
static inline u128 u128_bit(int b) { return ((u128)1) << b; }
static inline int u128_popc(u128 x) { return __builtin_popcountll((uint64_t)x) + __builtin_popcountll((uint64_t)(x >> 64)); }

// ------------------------------------------------------------------ TPC map
static int g_sm2bit[GNG_MAXSM];
static std::vector<std::vector<int>> g_bit2sms(128);
static void load_map(const std::string& f) {
  for (int i = 0; i < GNG_MAXSM; ++i) g_sm2bit[i] = -1;
  FILE* fp = fopen(f.c_str(), "r");
  if (!fp) { fprintf(stderr, "cannot open map %s\n", f.c_str()); exit(1); }
  char line[512];
  while (fgets(line, sizeof line, fp)) {
    int b; char* q = line;
    if (sscanf(q, "bit %d sms", &b) != 1) continue;
    q = strstr(q, "sms") + 3;
    int s, n;
    while (sscanf(q, "%d%n", &s, &n) == 1) { if (s >= 0 && s < GNG_MAXSM) { g_sm2bit[s] = b; g_bit2sms[b].push_back(s); } q += n; }
  }
  fclose(fp);
}
static std::vector<int> parse_list(const std::string& s) {
  std::vector<int> v;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string t = s.substr(i, j - i);
    if (!t.empty()) {
      size_t d = t.find('-');
      if (d != std::string::npos) { int a = atoi(t.substr(0, d).c_str()), b = atoi(t.substr(d + 1).c_str()); for (int k = a; k <= b; ++k) v.push_back(k); }
      else v.push_back(atoi(t.c_str()));
    }
    i = j + 1;
  }
  return v;
}
static u128 mask_of_sms(const std::vector<int>& sms) {
  u128 m = 0;
  for (int s : sms) { if (g_sm2bit[s] < 0) { fprintf(stderr, "SM %d not in map\n", s); exit(1); } m |= u128_bit(g_sm2bit[s]); }
  return m;
}

// ------------------------------------------------------------------ NVML sampler
struct NvSample { int64_t t; unsigned sm_mhz, mem_mhz, mw; unsigned long long reasons; };
struct Nvml {
  typedef int (*fi)(); typedef int (*fh)(const char*, void**); typedef int (*fc)(void*, int, unsigned*);
  typedef int (*fp)(void*, unsigned*); typedef int (*fr)(void*, unsigned long long*);
  fi init = nullptr; fh byPci = nullptr; fc clock = nullptr; fp power = nullptr; fr reasons = nullptr;
  void* dev = nullptr;
  bool ok = false;
  std::vector<NvSample> s;
  std::mutex mu;
  std::atomic<bool> stop{false};
  std::thread th;
  int period_ms = 10;
  void start(int period) {
    period_ms = period;
    void* h = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!h) { fprintf(stderr, "nvml: dlopen failed\n"); return; }
    init = (fi)dlsym(h, "nvmlInit_v2"); byPci = (fh)dlsym(h, "nvmlDeviceGetHandleByPciBusId_v2");
    clock = (fc)dlsym(h, "nvmlDeviceGetClockInfo"); power = (fp)dlsym(h, "nvmlDeviceGetPowerUsage");
    reasons = (fr)dlsym(h, "nvmlDeviceGetCurrentClocksEventReasons");
    if (!reasons) reasons = (fr)dlsym(h, "nvmlDeviceGetCurrentClocksThrottleReasons");
    if (!init || !byPci || !clock || !power || init()) { fprintf(stderr, "nvml: init failed\n"); return; }
    char bus[64]; CK(cudaDeviceGetPCIBusId(bus, sizeof bus, 0));
    if (byPci(bus, &dev)) { fprintf(stderr, "nvml: no handle for %s\n", bus); return; }
    ok = true;
    th = std::thread([this] {
      while (!stop) {
        NvSample x{}; x.t = host_ns();
        clock(dev, 1 /*NVML_CLOCK_SM*/, &x.sm_mhz); clock(dev, 2 /*NVML_CLOCK_MEM*/, &x.mem_mhz);
        power(dev, &x.mw); if (reasons) reasons(dev, &x.reasons);
        { std::lock_guard<std::mutex> g(mu); s.push_back(x); }
        usleep(period_ms * 1000);
      }
    });
  }
  void finish() { stop = true; if (th.joinable()) th.join(); }
  // window [a, b] in host ns; returns summary string
  std::string summary(int64_t a, int64_t b, double* clk_med_out = nullptr) {
    if (!ok) return "nvml=none";
    std::vector<double> c, p; unsigned long long r = 0; int n = 0;
    { std::lock_guard<std::mutex> g(mu);
      for (auto& x : s) if (x.t >= a && x.t <= b) { c.push_back(x.sm_mhz); p.push_back(x.mw / 1000.0); r |= x.reasons; ++n; } }
    if (n == 0) return "nvml_samples=0";
    std::sort(c.begin(), c.end()); std::sort(p.begin(), p.end());
    if (clk_med_out) *clk_med_out = c[c.size() / 2];
    char buf[256];
    // reasons: 0x1 idle, 0x4 sw power cap, 0x8 hw slowdown, 0x20 sw thermal, 0x40 hw thermal, 0x80 hw power brake
    snprintf(buf, sizeof buf, "nvml_n=%d clk_min=%.0f clk_med=%.0f pwr_med=%.0f pwr_max=%.0f reasons_or=0x%llx", n, c.front(), c[c.size() / 2], p[p.size() / 2], p.back(), r & ~1ull);
    return buf;
  }
};

static inline double median_of(std::vector<double> v) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }
