// Differential test: TfrFedpRef (unoptimized base) vs TfrFedp (new), same bits.
// Usage: fedp_diff <cases> <seed> [mode]   mode: 0 compare, 1 run ref only, 2 run new only
#include "fedp_ref.h"
#include "fedp.h"
#include <random>
#include <cstdlib>
#include <cinttypes>

static std::mt19937_64 rng;
static uint64_t R() { return rng(); }
static uint32_t g_inf_permille = 1000;

// a random element of an (eb, sb) format, biased toward special encodings
static uint32_t rand_elem(int eb, int sb) {
  uint32_t width = 1 + eb + sb;
  uint32_t emax = (1u << eb) - 1;
  uint32_t sign = R() & 1;
  uint32_t exp, frac;
  uint32_t pick = R() % 10;
  if (pick == 2 && (R() % 1000) >= g_inf_permille) pick = 9;
  switch (pick) {
  case 0: exp = 0; frac = 0; break;                                      // zero
  case 1: exp = 0; frac = R() & ((1u << sb) - 1); break;                 // subnormal
  case 2: exp = emax; frac = (R() & 1) ? 0 : (R() & ((1u << sb) - 1)); break; // inf/nan
  case 3: exp = emax - (R() % 2); frac = (1u << sb) - 1 - (R() % 2); break;  // near max / e4m3 nan
  case 4: exp = 1 + (R() % 2); frac = R() & ((1u << sb) - 1); break;     // smallest normals
  case 5: exp = (1u << (eb - 1)) - 1 + (R() % 3) - 1; frac = R() & 3; break; // near 1
  default: exp = R() & emax; frac = R() & ((1u << sb) - 1); break;
  }
  if (exp == emax && (R() % 1000) >= g_inf_permille) exp = emax - 1;
  // e4m3: exp 15 frac 7 is its NaN
  if (eb == 4 && sb == 3 && exp == 15 && frac == 7 && (R() % 1000) >= g_inf_permille) frac = 6;
  return ((sign << (eb + sb)) | (exp << sb) | frac) & ((width >= 32) ? ~0u : ((1u << width) - 1));
}

static uint32_t rand_word(int eb, int sb) {
  uint32_t width = 1 + eb + sb;
  bool packed = (width <= 16u) && ((32u % width) == 0u);
  if (!packed) {
    uint32_t v = rand_elem(eb, sb);
    if (R() % 4 == 0) v |= uint32_t(R()) << width; // garbage above the element
    return v;
  }
  uint32_t w = 0;
  for (uint32_t i = 0; i < 32 / width; ++i) w |= rand_elem(eb, sb) << (i * width);
  if (R() % 16 == 0 && g_inf_permille >= 100) w = uint32_t(R());
  return w;
}

static float rand_c() {
  uint32_t u;
  switch (R() % 8) {
  case 0: u = (R() & 1) << 31; break;
  case 1: u = ((R() & 1) << 31) | (R() & 0x7fffff); break;
  case 2: u = ((R() & 1) << 31) | 0x7f800000 | ((R() & 1) ? 0 : (R() & 0x7fffff)); break;
  case 3: u = ((R() & 1) << 31) | ((uint32_t(110 + R() % 40)) << 23) | (R() & 0x7fffff); break;
  default: u = uint32_t(R()); break;
  }
  float f; std::memcpy(&f, &u, 4); return f;
}

// copy a word, but make it partly cancel another (same magnitudes, flipped sign)
struct Fmt { int eb, sb; bool mx; bool fp4; };

int main(int argc, char** argv) {
  uint64_t cases = argc > 1 ? strtoull(argv[1], 0, 0) : 1000000;
  uint64_t seed = argc > 2 ? strtoull(argv[2], 0, 0) : 1;
  int mode = argc > 3 ? atoi(argv[3]) : 0;
  rng.seed(seed);
  const Fmt fmts[] = {
    {5, 10, false, false}, {8, 7, false, false}, {4, 3, false, false}, {5, 2, false, false},
    {8, 10, false, false}, {4, 3, true, false}, {5, 2, true, false}, {2, 1, true, true},
    // other formats the RTL unit test can configure
    {3, 2, false, false}, {2, 3, false, false}, {6, 9, false, false}, {5, 6, false, false},
  };
  const uint32_t ns[] = {1, 2, 4, 8, 16};
  // persistent objects for the new model (scratch reuse across calls and formats)
  TfrFedp persist(5, 10);
  uint64_t mism = 0; uint64_t cls[4] = {0,0,0,0};
  uint64_t xsum = 0;
  for (uint64_t t = 0; t < cases; ++t) {
    { uint32_t q = R() % 4; g_inf_permille = q == 0 ? 0 : q == 1 ? 10 : q == 2 ? 100 : 1000; }
    const Fmt& f = fmts[R() % (sizeof(fmts) / sizeof(fmts[0]))];
    uint32_t n = ns[R() % 5];
    int frm = (R() % 3 == 0) ? int(R() % 5) : 0;
    int W = (R() % 4 == 0) ? int(24 + R() % 5) : 25;
    bool renorm = (R() % 4 == 0);
    if (f.fp4) renorm = false;
    uint32_t a[16], b[16];
    uint8_t sfa[32], sfb[32];
    for (uint32_t i = 0; i < n; ++i) { a[i] = rand_word(f.eb, f.sb); b[i] = rand_word(f.eb, f.sb); }
    // cancellation: b' = b, a' = -a for some words
    if (R() % 4 == 0 && n >= 2) {
      uint32_t width = 1 + f.eb + f.sb;
      bool packed = (width <= 16u) && ((32u % width) == 0u);
      uint32_t epw = packed ? 32 / width : 1;
      uint32_t sbit = 0;
      for (uint32_t e = 0; e < epw; ++e) sbit |= 1u << (e * width + width - 1);
      a[1] = a[0] ^ sbit; b[1] = b[0];
      if (R() & 1) { uint32_t m = (1u << width) - 1; a[0] = (a[0] & ~m) | ((a[0] & m)); a[0] ^= (a[0] ^ (a[0] >> width)) & 0; }
    }
    for (uint32_t i = 0; i < 2 * n; ++i) {
      uint32_t r = R() % 8;
      sfa[i] = (r == 0 && g_inf_permille >= 100) ? 0xff : (r == 1 && g_inf_permille >= 100) ? 0x7f : r == 2 ? 0 : r == 3 ? uint8_t(127 + R() % 5 - 2) : uint8_t(R());
      r = R() % 8;
      sfb[i] = (r == 0 && g_inf_permille >= 100) ? 0xff : (r == 1 && g_inf_permille >= 100) ? 0x7f : r == 2 ? 0 : r == 3 ? uint8_t(127 + R() % 5 - 2) : uint8_t(R());
    }
    bool e4m3 = f.fp4 && (R() & 1);
    float c = rand_c();
    bool use_persist = (frm == 0 && W == 25 && !renorm);

    uint32_t r_ref = 0, r_new = 0;
    if (mode != 2) {
      TfrFedpRef ref(f.eb, f.sb, frm, W, renorm);
      float d = f.mx ? ref(a, b, c, n, sfa, sfb, e4m3) : ref(a, b, c, n);
      std::memcpy(&r_ref, &d, 4);
    }
    if (mode != 1) {
      float d;
      if (use_persist && mode == 0) {
        persist.set_format(f.eb, f.sb);
        d = f.mx ? persist(a, b, c, n, sfa, sfb, e4m3) : persist(a, b, c, n);
      } else {
        TfrFedp nw(f.eb, f.sb, frm, W, renorm);
        d = f.mx ? nw(a, b, c, n, sfa, sfb, e4m3) : nw(a, b, c, n);
      }
      std::memcpy(&r_new, &d, 4);
    }
    { uint32_t r = (mode == 1 ? r_ref : r_new); uint32_t e = (r >> 23) & 0xff; cls[(r & 0x7fffffff) == 0 ? 0 : e == 255 ? ((r & 0x7fffff) ? 1 : 2) : 3]++; }
    xsum = xsum * 1000003 + (mode == 1 ? r_ref : r_new);
    if (mode == 0 && r_ref != r_new) {
      if (mism < 20) {
        printf("MISMATCH case %" PRIu64 " fmt e%dm%d mx=%d fp4=%d e4m3=%d n=%u frm=%d W=%d renorm=%d c=0x%08x: ref=0x%08x new=0x%08x\n",
               t, f.eb, f.sb, f.mx, f.fp4, e4m3, n, frm, W, renorm, 0u, r_ref, r_new);
      }
      ++mism;
    }
  }
  printf("zero=%" PRIu64 " nan=%" PRIu64 " inf=%" PRIu64 " finite=%" PRIu64 "\n", cls[0], cls[1], cls[2], cls[3]);
  printf("cases=%" PRIu64 " seed=%" PRIu64 " mismatches=%" PRIu64 " xsum=%016" PRIx64 "\n", cases, seed, mism, xsum);
  return mism != 0;
}
