// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0

// The fp8/bf8 lane of the TFR dot-product model (fedp.h) pairs two products
// by their exponent fields, and a zero product keeps the field of its other
// operand, so it can sit above its nonzero partner. Shaped as SimX evaluates
// it (window W = 25, 8 words: 32 products in 16 lanes), every input here is
// one the datapath sums exactly, so the result must equal the exact dot
// product, worked out below in integers.

#include "fedp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

namespace {

constexpr uint32_t kWords = 8;
constexpr uint32_t kElems = 4 * kWords;
constexpr uint32_t kLanes = kElems / 2;
constexpr int kLsb = -40; // weight of bit 0 of the exact sum

struct Fmt {
  const char* name;
  int eb, sb;
  int band_lo, band_hi; // exponent fields of the nonzero elements
  int max_field;        // the largest finite exponent field
};

const Fmt kFp8 = {"fp8", 4, 3, 4, 8, 15};
const Fmt kBf8 = {"bf8", 5, 2, 11, 15, 30};

int bias(const Fmt& f) { return (1 << (f.eb - 1)) - 1; }
int field(const Fmt& f, uint8_t x) { return (x >> f.sb) & ((1 << f.eb) - 1); }
bool is_zero(uint8_t x) { return (x & 0x7f) == 0; }

// a normal element as m * 2^x
void decode(const Fmt& f, uint8_t x, int64_t& m, int& e) {
  m = (1 << f.sb) | (x & ((1 << f.sb) - 1));
  e = field(f, x) - bias(f) - f.sb;
}

uint8_t make(const Fmt& f, int sign, int fld, int frac) {
  return uint8_t((sign << 7) | (fld << f.sb) | frac);
}

struct Case {
  uint8_t a[kElems], b[kElems];
  uint8_t sf_a[kLanes], sf_b[kLanes];
  int c_sign = 0, c_m = 0, c_x = 0; // C = (-1)^c_sign * c_m * 2^c_x
};

// The exact dot product as a float; false if it is not one (a bad case).
bool exact(const Fmt& f, const Case& t, bool mx, float& out) {
  int64_t s = 0;
  for (uint32_t k = 0; k < kElems; ++k) {
    if (is_zero(t.a[k]) || is_zero(t.b[k])) continue;
    int64_t ma, mb;
    int xa, xb;
    decode(f, t.a[k], ma, xa);
    decode(f, t.b[k], mb, xb);
    int x = xa + xb;
    if (mx) x += int(t.sf_a[k / 2]) + int(t.sf_b[k / 2]) - 254;
    int64_t p = (ma * mb) << (x - kLsb);
    s += ((t.a[k] ^ t.b[k]) & 0x80) ? -p : p;
  }
  if (t.c_m != 0) {
    int64_t c = int64_t(t.c_m) << (t.c_x - kLsb);
    s += t.c_sign ? -c : c;
  }
  const double v = std::ldexp(double(s), kLsb);
  out = float(v);
  return double(out) == v;
}

uint32_t pack(const uint8_t* e, uint32_t w) {
  return uint32_t(e[4 * w]) | (uint32_t(e[4 * w + 1]) << 8) |
         (uint32_t(e[4 * w + 2]) << 16) | (uint32_t(e[4 * w + 3]) << 24);
}

struct Stats {
  long cases = 0, bad = 0, inexact = 0, above = 0;
};

void check(const Fmt& f, TfrFedp& fedp, const Case& t, bool mx, Stats& st) {
  uint32_t a[kWords], b[kWords];
  for (uint32_t w = 0; w < kWords; ++w) {
    a[w] = pack(t.a, w);
    b[w] = pack(t.b, w);
  }
  const float c = t.c_m ? (t.c_sign ? -1.0f : 1.0f) * std::ldexp(float(t.c_m), t.c_x) : 0.0f;
  const float got = mx ? fedp(a, b, c, kWords, t.sf_a, t.sf_b) : fedp(a, b, c, kWords);
  float want;
  ++st.cases;
  if (!exact(f, t, mx, want)) {
    ++st.inexact;
    return;
  }
  uint32_t ug, uw;
  std::memcpy(&ug, &got, 4);
  std::memcpy(&uw, &want, 4);
  if (ug != uw && ++st.bad <= 4) {
    std::printf("%s%s mismatch: got 0x%08x want 0x%08x, a/b:", mx ? "mx" : "", f.name, ug, uw);
    for (uint32_t k = 0; k < kElems; ++k) std::printf(" %02x/%02x", t.a[k], t.b[k]);
    std::printf(" c=%a\n", double(c));
  }
  // lanes whose zero product has the larger exponent field
  for (uint32_t r = 0; r < kLanes; ++r) {
    auto sel = [&](uint32_t k) {
      return std::max(field(f, t.a[k]), 1) + std::max(field(f, t.b[k]), 1);
    };
    const bool z0 = is_zero(t.a[2 * r]) || is_zero(t.b[2 * r]);
    const bool z1 = is_zero(t.a[2 * r + 1]) || is_zero(t.b[2 * r + 1]);
    if ((z0 && !z1 && sel(2 * r) > sel(2 * r + 1)) || (z1 && !z0 && sel(2 * r + 1) > sel(2 * r))) {
      ++st.above;
    }
  }
}

// One product of an element pair: nonzero (both in the band), or zero with
// the other operand anywhere in the finite range, half of the time in the top
// four fields.
void product(const Fmt& f, std::mt19937& rng, bool zero, uint8_t& a, uint8_t& b) {
  std::uniform_int_distribution<int> bit(0, 1), frac(0, (1 << f.sb) - 1);
  std::uniform_int_distribution<int> band(f.band_lo, f.band_hi), any(0, f.max_field);
  std::uniform_int_distribution<int> top(f.max_field - 3, f.max_field);
  auto finite = [&] {
    const int fld = bit(rng) ? any(rng) : top(rng);
    // fp8 (E4M3) has no infinity; its NaN is the top field with frac all ones
    const int fr = (f.eb == 4 && fld == 15) ? std::min(frac(rng), 6) : frac(rng);
    return make(f, bit(rng), fld, fr);
  };
  if (!zero) {
    a = make(f, bit(rng), band(rng), frac(rng));
    b = make(f, bit(rng), band(rng), frac(rng));
    return;
  }
  const uint8_t z = make(f, bit(rng), 0, 0);
  if (bit(rng)) {
    a = z;
    b = finite();
  } else {
    a = finite();
    b = z;
  }
}

long run(const Fmt& f, bool mx, bool renorm, int frm, uint32_t n) {
  std::mt19937 rng(0x5eed + f.eb * 16 + (mx ? 1 : 0) + (renorm ? 2 : 0) + frm * 4);
  std::uniform_int_distribution<int> kind(0, 3), bit(0, 1), cm(0, 15), cx(-6, 2), sf(125, 129);
  TfrFedp fedp(f.eb, f.sb, frm, 25, renorm);
  Stats st;
  Case t;
  for (uint32_t i = 0; i < n; ++i) {
    for (uint32_t r = 0; r < kLanes; ++r) {
      const int k = kind(rng); // 0 none, 1 first, 2 second, 3 both products zero
      product(f, rng, k & 1, t.a[2 * r], t.b[2 * r]);
      product(f, rng, k & 2, t.a[2 * r + 1], t.b[2 * r + 1]);
      t.sf_a[r] = uint8_t(sf(rng));
      t.sf_b[r] = uint8_t(sf(rng));
    }
    t.c_sign = bit(rng);
    t.c_m = bit(rng) ? cm(rng) : 0;
    t.c_x = cx(rng);
    check(f, fedp, t, mx, st);
  }
  // every lane: a zero product at the top field over a small product (at the
  // least normal fields, bf8's sits 29 fields below: out of the lane if the
  // zero product set its exponent)
  for (int lo : {f.band_lo, 1}) {
    for (int first = 0; first < 2; ++first) {
      for (uint32_t r = 0; r < kLanes; ++r) {
        const uint32_t z = 2 * r + (first ? 0 : 1), p = 2 * r + (first ? 1 : 0);
        t.a[z] = make(f, r & 1, 0, 0);
        t.b[z] = make(f, 0, f.max_field, 0);
        t.a[p] = make(f, (r >> 1) & 1, lo, r % (1 << f.sb));
        t.b[p] = make(f, 0, lo + (r & 1), 0);
        t.sf_a[r] = t.sf_b[r] = 127;
      }
      t.c_m = 0;
      check(f, fedp, t, mx, st);
    }
  }
  std::printf("%s%s renorm=%d frm=%d: %ld cases, %ld lanes with the zero product above, %ld mismatched, %ld not exact\n",
              mx ? "mx" : "", f.name, renorm, frm, st.cases, st.above, st.bad, st.inexact);
  // the vectors are built to be exact and to reach the zero-above lanes
  return st.bad + st.inexact + (st.above < long(n) ? 1 : 0);
}

} // namespace

int main() {
  long bad = 0;
  for (const Fmt* f : {&kFp8, &kBf8}) {
    for (bool mx : {false, true}) {
      bad += run(*f, mx, false, 0, 50000);
    }
    bad += run(*f, false, true, 0, 20000);
    bad += run(*f, false, false, 1, 20000);
  }
  std::printf(bad ? "FAILED!\n" : "PASSED!\n");
  return bad ? 1 : 0;
}
