// LD_PRELOAD shim: apply one TPC mask to every kernel launch of an unmodified
// process (libsmctrl's "global mask", 128-bit). TPC_ENABLE_BITS="0,3,5-9"
// lists the enabled TPC mask bits. Same callback as tpc_smctrl.h.
//   g++ -O2 -shared -fPIC tpc_preload.cpp -o tpc_preload.so -I$CUDA/include -L$CUDA/lib64 -lcuda
#include <cuda.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
typedef unsigned __int128 u128;
static const CUuuid id = {{0x2c, (char)0x8e, 0x0a, (char)0xd8, 0x07, 0x10, (char)0xab, 0x4e,
                           (char)0x90, (char)0xdd, 0x54, 0x71, (char)0x9f, (char)0xe5, (char)0xf7, 0x4b}};
static u128 g_disable = 0;
static long g_n = 0;
static void cb(void*, int, int, const void* in) {
  if (*(const uint32_t*)in < 5 * sizeof(void*)) return;
  char* tmd = (char*)*((void* const*)in + 4);
  if (!tmd) return;
  const uint8_t ver = *(uint8_t*)(tmd + 72);
  if (ver >= 0x40) {
    uint32_t* w = (uint32_t*)(tmd + 304);
    w[0] = (uint32_t)g_disable; w[1] = (uint32_t)(g_disable >> 32); w[2] = (uint32_t)(g_disable >> 64); w[3] = (uint32_t)(g_disable >> 96);
    *(uint32_t*)tmd |= 0x80000000u;
  } else if (ver >= 0x16) {
    *(uint32_t*)(tmd + 84) = (uint32_t)g_disable; *(uint32_t*)(tmd + 88) = (uint32_t)(g_disable >> 32);
  }
  ++g_n;
}
__attribute__((constructor)) static void init() {
  const char* e = getenv("TPC_ENABLE_BITS");
  if (!e || !*e) return;
  u128 en = 0;
  std::string s(e);
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i); if (j == std::string::npos) j = s.size();
    std::string t = s.substr(i, j - i);
    size_t d = t.find('-');
    if (d != std::string::npos) for (int k = atoi(t.substr(0, d).c_str()); k <= atoi(t.substr(d + 1).c_str()); ++k) en |= ((u128)1) << k;
    else if (!t.empty()) en |= ((u128)1) << atoi(t.c_str());
    i = j + 1;
  }
  g_disable = ~en;
  cuInit(0);
  uintptr_t* tbl = nullptr;
  if (cuGetExportTable((const void**)&tbl, &id) != CUDA_SUCCESS || !tbl) { fprintf(stderr, "tpc_preload: no export table\n"); return; }
  int (*sub)(uint32_t*, void (*)(void*, int, int, const void*), void*) = (decltype(sub))tbl[3];
  int (*ena)(uint32_t, uint32_t, int, int) = (decltype(ena))tbl[6];
  uint32_t h = 0;
  if (sub(&h, cb, nullptr) || ena(1, h, 0xb, 0x1)) { fprintf(stderr, "tpc_preload: callback setup failed\n"); return; }
  fprintf(stderr, "tpc_preload: mask set, %d TPCs enabled\n",
          __builtin_popcountll((uint64_t)en) + __builtin_popcountll((uint64_t)(en >> 64)));
}
__attribute__((destructor)) static void fini() {
  if (getenv("TPC_ENABLE_BITS")) fprintf(stderr, "tpc_preload: %ld launches masked\n", g_n);
}
