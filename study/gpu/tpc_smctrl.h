// Per-launch TPC masks on NVIDIA GPUs, a 128-bit reimplementation of the
// callback mechanism of libsmctrl (J. Bakita and J. H. Anderson, RTAS'23;
// source in third_party/libsmctrl, commit c250928). What is the same: the
// driver's private callback export table (cuGetExportTable with the UUID
// below, subscribe at slot 3, enable at slot 6), the QMD/TMD pre-upload
// callback (domain 0xb, cbid 0x1), the TMD pointer at in_params[4], the TMD
// version byte at offset 72, and for TMD V04 (Hopper) the 128-bit TPC disable
// mask at byte offsets 304, 308, 312, 316 plus the enable bit 31 of word 0.
// What is different: libsmctrl's next/global masks are 64-bit and force TPCs
// 64-127 off on V04; here all 128 bits are set from a uint128 so that the
// upper TPC indexes are reachable. Set bit = TPC disabled (libsmctrl
// convention). tpc_next_enable(e) takes the ENABLED set and inverts it.
// TPC_TMD_DUMP=1 in the environment prints the first 512 bytes of the TMD of
// every masked launch (for porting).
#pragma once
#include <cuda.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

typedef unsigned __int128 u128;

namespace tpcm {
static const CUuuid callback_funcs_id = {{0x2c, (char)0x8e, 0x0a, (char)0xd8, 0x07, 0x10, (char)0xab, 0x4e,
                                          (char)0x90, (char)0xdd, 0x54, 0x71, (char)0x9f, (char)0xe5, (char)0xf7, 0x4b}};
static bool g_setup = false;
static thread_local bool g_have_next = false;
static thread_local u128 g_next_disable = 0;
static int g_last_tmd_ver = -1;
static long g_callbacks = 0, g_applied = 0;

static void callback(void*, int, int, const void* in_params) {
  ++g_callbacks;
  if (*(const uint32_t*)in_params < 5 * sizeof(void*)) {
    fprintf(stderr, "tpc_smctrl: callback params too small (%u bytes)\n", *(const uint32_t*)in_params);
    abort();
  }
  char* tmd = (char*)*((void* const*)in_params + 4);
  if (!tmd) { fprintf(stderr, "tpc_smctrl: TMD pointer NULL\n"); abort(); }
  const uint8_t ver = *(uint8_t*)(tmd + 72);
  g_last_tmd_ver = ver;
  if (!g_have_next) return;
  g_have_next = false;
  if (getenv("TPC_TMD_DUMP")) {
    fprintf(stderr, "TMD ver 0x%02x:", ver);
    for (int i = 0; i < 512; i += 4) fprintf(stderr, "%s%08x", i % 32 ? " " : "\n  ", *(uint32_t*)(tmd + i));
    fprintf(stderr, "\n");
  }
  const u128 m = g_next_disable;
  if (ver >= 0x40) {
    uint32_t* w = (uint32_t*)(tmd + 304);
    w[0] = (uint32_t)m; w[1] = (uint32_t)(m >> 32); w[2] = (uint32_t)(m >> 64); w[3] = (uint32_t)(m >> 96);
    *(uint32_t*)tmd |= 0x80000000u;
  } else if (ver >= 0x16) {
    *(uint32_t*)(tmd + 84) = (uint32_t)m; *(uint32_t*)(tmd + 88) = (uint32_t)(m >> 32);
  } else {
    fprintf(stderr, "tpc_smctrl: TMD version 0x%02x has no TPC mask\n", ver); abort();
  }
  ++g_applied;
}

static void setup() {
  if (g_setup) return;
  g_setup = true;
  cuInit(0);
  uintptr_t* tbl = nullptr;
  CUresult r = cuGetExportTable((const void**)&tbl, &callback_funcs_id);
  if (r != CUDA_SUCCESS || !tbl) { fprintf(stderr, "tpc_smctrl: cuGetExportTable failed (%d)\n", (int)r); abort(); }
  int (*subscribe)(uint32_t*, void (*)(void*, int, int, const void*), void*);
  int (*enable)(uint32_t, uint32_t, int, int);
  subscribe = (decltype(subscribe))tbl[3];
  enable = (decltype(enable))tbl[6];
  uint32_t h = 0;
  int e = subscribe(&h, callback, nullptr);
  if (e) { fprintf(stderr, "tpc_smctrl: subscribe failed (%d)\n", e); abort(); }
  e = enable(1, h, 0xb, 0x1);
  if (e) { fprintf(stderr, "tpc_smctrl: enable failed (%d)\n", e); abort(); }
}
}  // namespace tpcm

// enabled set of TPC bits for the next launch from this thread; 0 = no mask
static inline void tpc_next_enable(u128 enabled) {
  tpcm::setup();
  if (enabled == 0) { tpcm::g_have_next = false; return; }
  tpcm::g_next_disable = ~enabled;
  tpcm::g_have_next = true;
}
static inline u128 u128_bit(int b) { return ((u128)1) << b; }
static inline int u128_popc(u128 x) { return __builtin_popcountll((uint64_t)x) + __builtin_popcountll((uint64_t)(x >> 64)); }
static inline void u128_hex(u128 x, char* out) {
  snprintf(out, 40, "%016llx%016llx", (unsigned long long)(uint64_t)(x >> 64), (unsigned long long)(uint64_t)x);
}
