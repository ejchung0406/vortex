// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <limits>
#include <utility>

#ifdef FEDP_TRACE
#define FEDP_LOG(...) std::fprintf(stderr, __VA_ARGS__);
#else
#define FEDP_LOG(...)
#endif

#ifdef FEDP_RTL_WATCH
#define RTL_WATCH(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define RTL_WATCH(...)
#endif

class TfrFedp {
public:
  explicit TfrFedp(int exp_bits = 5, int sig_bits = 10, int frm = 0, int W = 25, bool renorm = false, bool no_window = false)
      : exp_bits_(exp_bits), sig_bits_(sig_bits), frm_(frm), W_(W), renorm_(renorm), no_window_(no_window) {
    FEDP_LOG("[ctor] fmt=e%dm%d, frm=%d, W=%d, renorm_=%s, no_window_=%s\n",
        exp_bits_, sig_bits_, frm_, W_, (renorm_ ? "true" : "false"), (no_window_ ? "true" : "false"));
    assert(exp_bits_ > 0 && exp_bits_ <= 8);
    assert(sig_bits_ > 0 && sig_bits_ <= 10);
    assert(frm_ >= 0 && frm_ <= 4);
  }

  // Switch the input format, keeping the term buffers (a caller evaluating
  // several formats can hold one object). For the debug traces it starts over
  // as a newly built object would: the ctor line, request numbering from 0.
  void set_format(int exp_bits, int sig_bits) {
    exp_bits_ = exp_bits;
    sig_bits_ = sig_bits;
    req_id_ = 0;
    FEDP_LOG("[ctor] fmt=e%dm%d, frm=%d, W=%d, renorm_=%s, no_window_=%s\n",
        exp_bits_, sig_bits_, frm_, W_, (renorm_ ? "true" : "false"), (no_window_ ? "true" : "false"));
    assert(exp_bits_ > 0 && exp_bits_ <= 8);
    assert(sig_bits_ > 0 && sig_bits_ <= 10);
  }

  // Block-scaled formats: one scale byte per lane for A and B, two lanes per
  // 32-bit register (a lane is 16 bits of it: two fp8 or four fp4 elements).
  //   fp8/bf8 (MXFP8/MXBF8) and fp4 (MXFP4): E8M0 scales;
  //   fp4 with e4m3_scale (NVFP4): E4M3 scales.
  float operator()(const uint32_t *a, const uint32_t *b, float c, uint32_t n,
                   const uint8_t *sf_a, const uint8_t *sf_b, bool e4m3_scale = false) {
    sf_a_ = sf_a;
    sf_b_ = sf_b;
    e4m3_scale_ = e4m3_scale;
    float d = (*this)(a, b, c, n);
    sf_a_ = sf_b_ = nullptr;
    e4m3_scale_ = false;
    return d;
  }

  float operator()(const uint32_t *a, const uint32_t *b, float c, uint32_t n) {
    const auto c_enc = bitsFromF32(c);
    rtl_watch_s0(a, b, c_enc, n);

    const auto c_dec = decode_input(c_enc, 8, 23);
    const auto c_term = decodeC_to_common(c_dec);

    // The product terms and C, in the term buffers; returns the term count.
    const auto mul = (exp_bits_ == 2 && sig_bits_ == 1)
                   ? multiply_f4(a, b, n, c_term)
                   : multiply_to_common(a, b, n, c_term);

    rtl_watch_s1(mul, c_term, c_enc);

    HR_ = 32 - lzcN(mul.nt - 1, 32);

    const auto aln = alignment(mul);

    rtl_watch_s2(aln, mul, c_term, c_enc);

    const auto acc = accumulate(aln, mul.nt);

    rtl_watch_s3(acc, mul, c_term, c_enc);

    const auto nrm = normalize(acc);

    rtl_watch_norm(nrm, acc.flags, mul.L);

    const auto out = rounding(nrm);
    rtl_watch_s4(out);

    ++req_id_;

    return f32FromBits(out);
  }

private:
  enum FRM_TYPE { FRM_RNE = 0, FRM_RTZ = 1, FRM_RDN = 2, FRM_RUP = 3, FRM_RMM = 4 };

  static const uint8_t FL_NAN  = 1;
  static const uint8_t FL_PINF = 2;
  static const uint8_t FL_NINF = 4;

  static const int F32_BIAS = 127;
  static const int EXP_POS_BIAS = 128;

  struct dec_t {
    int  sign;
    int  frac;
    int  exp;
    bool is_zero;
    bool is_sub;
  };
  struct cterm_t {
    int sign;
    int Mc;
    int Ec;
    int cls;
    int sp;
  };

  struct pterm_t {
    int sign;
    int Mp;
  };

  // The terms live in the per-object buffers below (sign_, mp_, eps_ then
  // shifts_): nt terms, the products first and C last.
  struct mul_res_t {
    uint32_t nt;
    int L;
    uint8_t flags;
  };

  // Aligned terms in aln_ (and their sticky bits in stk_).
  struct align_t {
    int sticky;
    int L;
    uint8_t flags;
  };
  struct acc_t {
    uint32_t V;
    int sticky;
    int L;
    uint8_t flags;
  };
  struct nrm_t {
    int sign;
    uint32_t kept;
    int g;
    int st;
    int e;
    uint8_t flags;
    // Debug info
    uint32_t abs_sum;
    int shift_amt;
    bool round_up;
  };

  // -------- utils --------
  static inline uint32_t bitsFromF32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
  static inline float f32FromBits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
  static inline int bias(int eb) { return (1 << (eb - 1)) - 1; }
  static inline int lzcN(uint32_t x, int w) { if (!x) return w; return __builtin_clz(x) - (32 - w); }
  static inline uint32_t sign_ext(uint32_t x, uint32_t width) {
    uint32_t mask = 1u << (width - 1);
    x = x & ((1u << width) - 1);
    return (x ^ mask) - mask;
  }

  // -------- [RTL-WATCH] helpers --------
  inline void rtl_watch_s0([[maybe_unused]] const uint32_t* a, [[maybe_unused]] const uint32_t* b, [[maybe_unused]] uint32_t c_enc, [[maybe_unused]] uint32_t n) {
#ifdef FEDP_RTL_WATCH
    RTL_WATCH("[RTL_WATCH] FEDP-S0(%lu): a_row=", req_id_);
    RTL_WATCH("{");
    for (int i = (int)n - 1; i >= 0; --i) {
      RTL_WATCH("0x%x", a[i]);
      if (i > 0) RTL_WATCH(", ");
    }
    RTL_WATCH("}, b_col={");
    for (int i = (int)n - 1; i >= 0; --i) {
      RTL_WATCH("0x%x", b[i]);
      if (i > 0) RTL_WATCH(", ");
    }
    RTL_WATCH("}, c_val=0x%x\n", c_enc);
#endif
  }

  inline void rtl_watch_s1([[maybe_unused]] const mul_res_t& res, [[maybe_unused]] const cterm_t& c_term, [[maybe_unused]] uint32_t c_enc) {
#ifdef FEDP_RTL_WATCH
    RTL_WATCH("[RTL_WATCH] FEDP-S1(%lu): max_exp=0x%x, shift_amt=", req_id_, res.L);
    RTL_WATCH("{");
    if (res.nt != 0) {
      RTL_WATCH("0x%x", shifts_[res.nt - 1]);
      if (res.nt > 1) RTL_WATCH(", ");
      for (int i = (int)res.nt - 2; i >= 0; --i) {
        RTL_WATCH("0x%x", shifts_[i]);
        if (i > 0) RTL_WATCH(", ");
      }
    }
    RTL_WATCH("}, raw_sig={");
    uint32_t c_raw = c_term.Mc;
    if (c_term.sign) c_raw |= (1u << 24);
    if (c_term.cls == 2 || c_term.cls == 1) c_raw |= (1u << 23);
    RTL_WATCH("0x%x", c_raw);
    if (res.nt > 1) RTL_WATCH(", ");
    for (int i = (int)res.nt - 2; i >= 0; --i) {
      uint32_t raw_val = mp_[i];
      if (sign_[i]) raw_val |= (1u << 24);
      RTL_WATCH("0x%x", raw_val);
      if (i > 0) RTL_WATCH(", ");
    }
    RTL_WATCH("}\n");
#endif
  }

  inline void rtl_watch_s2([[maybe_unused]] const align_t& aln, [[maybe_unused]] const mul_res_t& res, [[maybe_unused]] const cterm_t& c_term, [[maybe_unused]] uint32_t c_enc) {
#ifdef FEDP_RTL_WATCH
    RTL_WATCH("[RTL_WATCH] FEDP-S2(%lu): max_exp=0x%x, aln_sig=", req_id_, res.L);
    RTL_WATCH("{");
    if (res.nt != 0) {
      uint32_t c_sig = aln_[res.nt - 1];
      RTL_WATCH("0x%x", c_sig);
      if (res.nt > 1) RTL_WATCH(", ");
      for (int i = (int)res.nt - 2; i >= 0; --i) {
        RTL_WATCH("0x%x", aln_[i]);
        if (i > 0) RTL_WATCH(", ");
      }
    }
    RTL_WATCH("}, sticky_bits={");
    if (res.nt != 0) {
      RTL_WATCH("0x%x", stk_[res.nt - 1]);
      if (res.nt > 1) RTL_WATCH(", ");
      for (int i = (int)res.nt - 2; i >= 0; --i) {
        RTL_WATCH("0x%x", stk_[i]);
        if (i > 0) RTL_WATCH(", ");
      }
    }
    RTL_WATCH("}\n");
#endif
  }

  inline void rtl_watch_s3([[maybe_unused]] const acc_t& acc, [[maybe_unused]] const mul_res_t& res, [[maybe_unused]] const cterm_t& c_term, [[maybe_unused]] uint32_t c_enc) {
#ifdef FEDP_RTL_WATCH
    uint32_t sigs_sign = 0;
    for (size_t i = 0; i < res.nt - 1; ++i) {
      if (sign_[i]) sigs_sign |= (1 << i);
    }
    RTL_WATCH("[RTL_WATCH] FEDP-S3(%lu): acc_sig=0x%x, max_exp=0x%x, sigs_sign=0x%x, sticky=%d\n",
        req_id_, acc.V, res.L, sigs_sign, acc.sticky ? 1 : 0);
#endif
  }

  inline void rtl_watch_norm([[maybe_unused]] const nrm_t& nrm, [[maybe_unused]] int flags, [[maybe_unused]] int max_exp) {
#ifdef FEDP_RTL_WATCH
    int lsb = nrm.kept & 1;
    RTL_WATCH("[RTL_WATCH] FEDP-NORM(%lu): abs_sum=0x%x, L=%d, G=%d, R=%d, S=%d, Rup=%d\n",
        req_id_, nrm.abs_sum, lsb, nrm.g, 0, nrm.st, nrm.round_up
    );
#endif
  }

  inline void rtl_watch_s4([[maybe_unused]] uint32_t result) {
#ifdef FEDP_RTL_WATCH
    RTL_WATCH("[RTL_WATCH] FEDP-S4(%lu): result=0x%08x\n", req_id_, result);
#endif
  }

  // Exponent of a zero term: it stands for the datapath's exponent 0, which
  // add_c_term places WA - 1 below this model's exponent origin.
  static constexpr int kZeroE = -(1 << 20);

  // The datapath's accumulator width over `lanes` product terms and C.
  int acc_width(uint32_t lanes) const { return W_ + 2 + (32 - lzcN(lanes, 32)); }

  // Size the term buffers for `terms` terms; they only grow, so a reused
  // object stops allocating after its first evaluation.
  void reserve_terms(uint32_t terms) {
    if (mp_.size() < terms) {
      sign_.resize(terms);
      mp_.resize(terms);
      eps_.resize(terms);
      sel_eps_.resize(terms);
      nz_.resize(terms);
      shifts_.resize(terms);
      aln_.resize(terms);
      stk_.resize(terms);
    }
  }

  // -------- logic --------
  static dec_t decode_input(uint32_t enc, int eb, int sb) {
    dec_t d{};
    d.sign = (enc >> (eb + sb)) & 1u;
    d.exp = (enc >> sb) & ((1u << eb) - 1u);
    d.frac = enc & ((1u << sb) - 1u);
    d.is_zero = (d.exp == 0 && d.frac == 0);
    d.is_sub = (d.exp == 0 && d.frac != 0);
    return d;
  }

  static cterm_t decodeC_to_common(const dec_t &c) {
    const int ebC = 8, sbC = 23;
    int cls = 0, Mc = 0, eC = 0, sp = 0;
    if (c.exp == 255) { sp = c.frac ? 3 : (c.sign ? 2 : 1); }
    else if (c.is_zero) { cls = 0; Mc = 0; eC = 0; }
    else if (c.is_sub) { cls = 1; Mc = c.frac; eC = (1 - bias(ebC)) - sbC; }
    else { cls = 2; Mc = ((1u << sbC) | c.frac); eC = (c.exp - bias(ebC)) - sbC; }
    return {c.sign, Mc, eC, cls, sp};
  }

  // Decode the n packed words of a and b and form their products in the term
  // buffers; fp8/bf8 then reduce them pairwise in place.
  mul_res_t multiply_to_common(const uint32_t *a, const uint32_t *b, uint32_t n, const cterm_t &c_term) {
    const int sb = sig_bits_, eb = exp_bits_;
    const uint32_t width = 1u + eb + sb;
    const bool packed = (width <= 16u) && ((32u % width) == 0u);
    const uint32_t epw = packed ? (32u / width) : 1u;
    const uint32_t np = n * epw;
    reserve_terms(np + 1);

    uint8_t c_nan  = (c_term.sp == 3) ? FL_NAN  : 0;
    uint8_t c_pinf = (c_term.sp == 1) ? FL_PINF : 0;
    uint8_t c_ninf = (c_term.sp == 2) ? FL_NINF : 0;
    uint8_t flags  = (c_nan | c_pinf | c_ninf);

    // the tensor core's formats with their fields as constants
    if (renorm_) {
      flags |= products(a, b, n);
    } else if (eb == 5 && sb == 10) {
      flags |= products_fixed<5, 10>(a, b, n);
    } else if (eb == 8 && sb == 7) {
      flags |= products_fixed<8, 7>(a, b, n);
    } else if (eb == 4 && sb == 3) {
      flags |= products_fixed<4, 3>(a, b, n);
    } else if (eb == 5 && sb == 2) {
      flags |= products_fixed<5, 2>(a, b, n);
    } else if (eb == 8 && sb == 10) {
      flags |= products_fixed<8, 10>(a, b, n);
    } else {
      flags |= products(a, b, n);
    }

    int* const sign = sign_.data();
    int* const mp = mp_.data();
    int* const eps = eps_.data();
    const int* const sel_eps = sel_eps_.data();
    const int* const nonzero = nz_.data();

    uint32_t nprod = np;

    // FP8/BF8: reduce product terms pairwise, lane j/2 written over term j
    if (width <= 8) {
      const int WA = acc_width(np / 2);
      for (uint32_t j = 0; j < np; j += 2) {
        const uint32_t r = j / 2;
        if (j + 1 < np) {
          auto red = reduce_f8({sign[j], mp[j]}, sel_eps[j], nonzero[j] != 0,
                               {sign[j + 1], mp[j + 1]}, sel_eps[j + 1], nonzero[j + 1] != 0);
          // MX: the lane's E8M0 scales add to its exponent field, which is
          // 10 bits wide and sits WA - 1 above this model's exponents
          if (sf_a_) {
            // an E8M0 scale of 0xFF is NaN, and so is every element it scales
            if (sf_a_[r] == 0xff || sf_b_[r] == 0xff) {
              flags |= FL_NAN;
            }
            // a lane scaled below the exponent range sits at exponent 0,
            // where it only adds to the sticky bit
            if (red.second != kZeroE) {
              int lane_exp = red.second + (WA - 1) + int(sf_a_[r]) + int(sf_b_[r]) - 254;
              red.second = std::max(lane_exp, 0) - (WA - 1);
            }
          }
          sign[r] = red.first.sign;
          mp[r] = red.first.Mp;
          eps[r] = red.second;
        } else {
          sign[r] = sign[j];
          mp[r] = mp[j];
          eps[r] = eps[j];
        }
      }
      nprod = (np + 1) / 2;
    }

    return add_c_term(nprod, c_term, flags);
  }

  // The n * epw products of a and b into sign_, mp_, eps_ (per-term *window
  // LSB exponent*, not raw Ep), sel_eps_ and nz_; returns their NaN/Inf
  // flags.
  uint8_t products(const uint32_t *a, const uint32_t *b, uint32_t n) {
    const int eb = exp_bits_;
    const int sb = sig_bits_;
    const uint32_t width = 1u + eb + sb;
    const bool packed = (width <= 16u) && ((32u % width) == 0u);
    const uint32_t epw = packed ? (32u / width) : 1u;
    const uint32_t wmask = packed ? ((1u << width) - 1u) : ~0u;
    const int all1 = (1 << eb) - 1;
    const bool is_e4m3 = (eb == 4 && sb == 3);
    const int bs = bias(eb);
    const int shift_sf = 22 - (2 * sb);
    const int ep_off = F32_BIAS + EXP_POS_BIAS + 1 - W_;
    const bool renorm = renorm_;
    int* __restrict const sign = sign_.data();
    int* __restrict const mp = mp_.data();
    int* __restrict const eps = eps_.data();
    int* __restrict const sel_eps = sel_eps_.data();
    int* __restrict const nonzero = nz_.data();
    uint8_t flags = 0;

    for (uint32_t w = 0; w < n; ++w) {
      uint32_t aw = a[w], bw = b[w];
      for (uint32_t e = 0; e < epw; ++e) {
        const auto ad = decode_input(aw & wmask, eb, sb);
        const auto bd = decode_input(bw & wmask, eb, sb);
        if (packed) { aw >>= width; bw >>= width; }

        bool zA = ad.is_zero;
        bool zB = bd.is_zero;
        bool p_is_zero = zA || zB;

        bool infA, infB, nanA, nanB;
        if (is_e4m3) {
          infA = false; infB = false;
          nanA = (ad.exp == 15 && ad.frac == 7);
          nanB = (bd.exp == 15 && bd.frac == 7);
        } else {
          infA = (ad.exp == all1 && !ad.frac); infB = (bd.exp == all1 && !bd.frac);
          nanA = (ad.exp == all1 && ad.frac); nanB = (bd.exp == all1 && bd.frac);
        }

        bool is_nan = nanA || nanB || (infA && zB) || (infB && zA);
        bool is_inf = (infA || infB) && !is_nan;
        bool sign_xor = ((ad.sign ^ bd.sign) != 0);

        flags |= is_nan ? FL_NAN : 0;
        flags |= is_inf ? (sign_xor ? FL_NINF : FL_PINF) : 0;

        int Ea = ad.is_sub ? (1 - bs) : (ad.exp - bs);
        int Eb = bd.is_sub ? (1 - bs) : (bd.exp - bs);
        int Ma = zA ? 0 : (ad.is_sub ? ad.frac : ((1 << sb) | ad.frac));
        int Mb = zB ? 0 : (bd.is_sub ? bd.frac : ((1 << sb) | bd.frac));

        int Mp = (Ma * Mb) << shift_sf;
        int Ep = (Ea + Eb) + ep_off;

        if (renorm) {
          int lzc_prod = (ad.is_sub ? lzcN(Ma, sb) : 0) + (bd.is_sub ? lzcN(Mb, sb) : 0);
          Mp <<= lzc_prod;
          Ep -= lzc_prod;
        }

        const uint32_t k = w * epw + e;
        // the fp8 lane pairs its two products by their exponent fields even
        // when one is zero (a zero field counts as 1, as for a subnormal)
        sel_eps[k] = (std::max(ad.exp, 1) - bs) + (std::max(bd.exp, 1) - bs) + ep_off;
        nonzero[k] = !p_is_zero;

        if (p_is_zero) {
          Ep = kZeroE;
          Mp = 0;
        }

        sign[k] = sign_xor;
        mp[k] = Mp;
        eps[k] = Ep;
      }
    }
    return flags;
  }

  // products() for an EB/SB format without renormalization, branch-free.
  // A zero or subnormal field reads as exponent 1 (the zero product's
  // exponent is replaced, and its mantissa product is 0), so Ep and the fp8
  // lane's pairing exponent are the same sum. Inf/NaN flags are worked out
  // in a second pass, only when some element has an all-ones exponent.
  template <int EB, int SB>
  uint8_t products_fixed(const uint32_t *a, const uint32_t *b, uint32_t n) {
    constexpr uint32_t width = 1u + EB + SB;
    constexpr bool packed = (width <= 16u) && ((32u % width) == 0u);
    constexpr uint32_t epw = packed ? (32u / width) : 1u;
    constexpr uint32_t wmask = packed ? ((1u << width) - 1u) : ~0u;
    constexpr uint32_t magmask = (1u << (EB + SB)) - 1u;
    constexpr int all1 = (1 << EB) - 1;
    constexpr bool is_e4m3 = (EB == 4 && SB == 3);
    // the least magnitude with an all-ones exponent (e4m3: its NaN)
    constexpr uint32_t kSpecial = is_e4m3 ? 0x7fu : (uint32_t(all1) << SB);
    constexpr int bs = (1 << (EB - 1)) - 1;
    constexpr int shift_sf = 22 - (2 * SB);
    constexpr uint32_t fmask = (1u << SB) - 1u;
    const int ep_off = F32_BIAS + EXP_POS_BIAS + 1 - W_ - 2 * bs;
    int* __restrict const sign = sign_.data();
    int* __restrict const mp = mp_.data();
    int* __restrict const eps = eps_.data();
    int* __restrict const sel_eps = sel_eps_.data();
    int* __restrict const nonzero = nz_.data();
    uint32_t special = 0;

    for (uint32_t w = 0; w < n; ++w) {
      const uint32_t aw = a[w], bw = b[w];
      for (uint32_t e = 0; e < epw; ++e) {
        const uint32_t ae = (aw >> (e * width)) & wmask;
        const uint32_t be = (bw >> (e * width)) & wmask;
        const uint32_t magA = ae & magmask, magB = be & magmask;
        const int eA = int(magA >> SB), eB = int(magB >> SB);
        const int Ma = (int(eA != 0) << SB) | int(magA & fmask);
        const int Mb = (int(eB != 0) << SB) | int(magB & fmask);
        const int sel = std::max(eA, 1) + std::max(eB, 1) + ep_off;
        const int p_is_zero = (magA == 0) | (magB == 0);
        special |= uint32_t(is_e4m3 ? ((magA == kSpecial) | (magB == kSpecial))
                                    : ((magA >= kSpecial) | (magB >= kSpecial)));

        const uint32_t k = w * epw + e;
        if (width <= 8) { // read only by the fp8 lane reduction
          sel_eps[k] = sel;
          nonzero[k] = p_is_zero ^ 1;
        }
        sign[k] = int(((ae ^ be) >> (EB + SB)) & 1);
        mp[k] = (Ma * Mb) << shift_sf;
        eps[k] = p_is_zero ? kZeroE : sel;
      }
    }
    if (!special) {
      return 0;
    }

    uint8_t flags = 0;
    for (uint32_t w = 0; w < n; ++w) {
      const uint32_t aw = a[w], bw = b[w];
      for (uint32_t e = 0; e < epw; ++e) {
        const auto ad = decode_input((aw >> (e * width)) & wmask, EB, SB);
        const auto bd = decode_input((bw >> (e * width)) & wmask, EB, SB);
        bool infA, infB, nanA, nanB;
        if (is_e4m3) {
          infA = false; infB = false;
          nanA = (ad.exp == 15 && ad.frac == 7);
          nanB = (bd.exp == 15 && bd.frac == 7);
        } else {
          infA = (ad.exp == all1 && !ad.frac); infB = (bd.exp == all1 && !bd.frac);
          nanA = (ad.exp == all1 && ad.frac); nanB = (bd.exp == all1 && bd.frac);
        }
        bool is_nan = nanA || nanB || (infA && bd.is_zero) || (infB && ad.is_zero);
        bool is_inf = (infA || infB) && !is_nan;
        bool sign_xor = ((ad.sign ^ bd.sign) != 0);
        flags |= is_nan ? FL_NAN : 0;
        flags |= is_inf ? (sign_xor ? FL_NINF : FL_PINF) : 0;
      }
    }
    return flags;
  }

  // Append the C term after the nprod products and take every term's shift
  // to the largest exponent.
  mul_res_t add_c_term(uint32_t nprod, const cterm_t &c_term, uint8_t flags) {
    bool c_is_zero = (c_term.cls == 0 && c_term.sp == 0);
    int Ec = c_term.Ec + F32_BIAS + EXP_POS_BIAS + 24 - W_;
    if (c_is_zero) {
      Ec = kZeroE;
    }

    const uint32_t nt = nprod + 1;
    int* const eps = eps_.data();
    sign_[nprod] = c_term.sign;
    mp_[nprod] = c_term.Mc;
    eps[nprod] = Ec;
    const int WA = acc_width(nprod);
    int L = std::numeric_limits<int>::min();
    for (uint32_t i = 0; i < nt; ++i) {
      int e = (eps[i] == kZeroE) ? -(WA - 1) : eps[i];
      eps[i] = e;
      L = std::max(L, e);
    }

    int* const shifts = shifts_.data();
    for (uint32_t i = 0; i < nt; ++i) {
      shifts[i] = L - eps[i];
    }

    return {nt, L, flags};
  }

  // Block-scaled fp4 (E2M1), one lane per 16 bits: its four products are
  // aligned to their largest exponent in a 6-bit lane field, summed in 27-bit
  // two's complement, and leave the datapath as one term. E2M1 has no
  // infinity or NaN. MXFP4 adds the E8M0 scales to the lane exponent; NVFP4
  // folds the E4M3 scales' exponent fields into the 6-bit field (they fit) and
  // multiplies each product by the scales' mantissa product.
  mul_res_t multiply_f4(const uint32_t *a, const uint32_t *b, uint32_t n, const cterm_t &c_term) {
    assert(sf_a_ && sf_b_ && "fp4 needs block scales");
    const int SIG_SHIFT = 11;
    const int EXP_TERM_BIAS = 32;
    const int exp_comp = e4m3_scale_ ? -(2 * 1 + 2 * (7 + 3)) : -(2 * 1 + 10);
    const int exp_adj = (EXP_TERM_BIAS + exp_comp) & 63;
    // the lane exponent the datapath emits is 10 bits wide; this
    // model's lane exponents sit WA - 1 below it
    const int WA = acc_width(2 * n);
    const int bias_base = F32_BIAS + 2 * (23 - 22) - W_ + WA - 1 + EXP_POS_BIAS;
    auto f4_man = [](uint32_t x) -> uint32_t { return ((x >> 1) & 3) == 0 ? 1u : (2u | (x & 1)); };
    auto f4_exp = [](uint32_t x) -> uint32_t {
      uint32_t e2 = (x >> 2) & 1, e1 = (x >> 1) & 1;
      return (e2 & ~e1 & 1) | ((e2 & e1) << 1);
    };
    reserve_terms(2 * n + 1);
    uint8_t flags = 0;
    if (c_term.sp == 3) flags |= FL_NAN;
    if (c_term.sp == 1) flags |= FL_PINF;
    if (c_term.sp == 2) flags |= FL_NINF;
    for (uint32_t lane = 0; lane < 2 * n; ++lane) {
      const uint32_t word = lane / 2, off = (lane % 2) * 16;
      const uint32_t sfa = sf_a_[lane], sfb = sf_b_[lane];
      // NVFP4 UE4M3 scales (bit 7 is padding): 0x7F is NaN. MXFP4 E8M0
      // scales join the lane exponent; 0xFF is NaN.
      uint32_t sf_man_prod = 1;
      int sf_exp = 0;        // into the 6-bit term exponent (NVFP4)
      int sf_lane_exp = 0;   // into the lane exponent (MXFP4)
      if (e4m3_scale_) {
        sf_man_prod = (8u | (sfa & 7)) * (8u | (sfb & 7));
        sf_exp = int((sfa >> 3) & 15) + int((sfb >> 3) & 15);
        if ((sfa & 0x7f) == 0x7f || (sfb & 0x7f) == 0x7f) {
          flags |= FL_NAN;
        }
      } else {
        sf_lane_exp = int(sfa) + int(sfb) - 254;
        if (sfa == 0xff || sfb == 0xff) {
          flags |= FL_NAN;
        }
      }
      uint32_t texp[4], tprod[4], tsign[4];
      bool tvalid[4];
      uint32_t max_exp = 0;
      for (int j = 0; j < 4; ++j) {
        uint32_t ra = (a[word] >> (off + 4 * j)) & 0xf;
        uint32_t rb = (b[word] >> (off + 4 * j)) & 0xf;
        tvalid[j] = (ra & 7) != 0 && (rb & 7) != 0;
        tsign[j] = ((ra ^ rb) >> 3) & 1;
        uint32_t prod = f4_man(ra) * f4_man(rb);
        tprod[j] = e4m3_scale_ ? ((prod * sf_man_prod) & 0x7ff) : prod;
        texp[j] = tvalid[j] ? (uint32_t(exp_adj + sf_exp + int(f4_exp(ra)) + int(f4_exp(rb))) & 63) : 0;
        max_exp = std::max(max_exp, texp[j]);
      }
      uint32_t sum = 0; // 27-bit two's complement
      for (int j = 0; j < 4; ++j) {
        uint32_t shift = (max_exp - texp[j]) & 63;
        uint32_t mag = tvalid[j] ? ((tprod[j] << SIG_SHIFT) & 0xffffff) : 0;
        uint32_t aligned = (shift >= 24) ? 0 : (mag >> shift);
        sum = (sum + (tsign[j] ? (0u - aligned) : aligned)) & 0x7ffffff;
      }
      uint32_t sum_sign = (sum >> 26) & 1;
      uint32_t abs_sum = (sum_sign ? (0u - sum) : sum) & 0x3ffffff;
      if (abs_sum == 0) {
        sign_[lane] = 0;
        mp_[lane] = 0;
        eps_[lane] = kZeroE;
        continue;
      }
      const int lane_exp = (int(max_exp) + bias_base + exp_comp + sf_lane_exp) & 1023;
      sign_[lane] = int(sum_sign);
      mp_[lane] = int(abs_sum & 0xffffff);
      eps_[lane] = lane_exp - (WA - 1);
    }
    return add_c_term(2 * n, c_term, flags);
  }

  // One fp8 lane: its two products, aligned to the one the datapath picks as
  // larger. A zero product never sets the lane exponent.
  std::pair<pterm_t, int> reduce_f8(const pterm_t& t1, int e1, bool v1, const pterm_t& t2, int e2, bool v2) {
    // Pick max exponent term
    int max_E, max_s, min_E, min_s;
    int64_t max_P, min_P;

    if (!v1 && !v2) {
      return {{0, 0}, kZeroE};
    }
    if (v1 && (!v2 || e1 >= e2)) {
      max_E = e1; max_P = t1.Mp; max_s = t1.sign;
      min_E = e2; min_P = t2.Mp; min_s = t2.sign;
    } else {
      max_E = e2; max_P = t2.Mp; max_s = t2.sign;
      min_E = e1; min_P = t1.Mp; min_s = t1.sign;
    }

    int diff = max_E - min_E;

    // Align smaller term. A zero product may have the larger exponent field
    // (diff < 0); it aligns to 0, as any shift of it does.
    int64_t aligned = 0;
    if (diff >= 32 || diff < 0) {
      aligned = 0;
    } else if (diff == 0) {
      aligned = min_P;
    } else {
      aligned = (min_P >> diff);
    }

    // Signed add
    int64_t val1 = max_s ? -max_P : max_P;
    int64_t val2 = min_s ? -aligned : aligned;
    int64_t val  = val1 + val2;

    if (val == 0) {
      // Only an exact cancellation of two equal-exponent products clears the
      // lane exponent; a product aligned out of the lane keeps it, so the
      // empty lane still sets the largest exponent.
      bool cancel = (max_s != min_s) && (e1 == e2) && (t1.Mp == t2.Mp);
      return {{0, 0}, cancel ? kZeroE : (max_E + 1)};
    }

    int s = (val < 0) ? 1 : 0;
    int64_t P = (val < 0) ? -val : val;
    int E = max_E;

    // scaling by 1 to avoid renormalization
    P >>= 1;
    E += 1;

    FEDP_LOG("[reduce_f8 (%ld)] t1=(%d, %d, %d), t2=(%d, %d, %d) | diff=%d max_P=0x%lx min_P=0x%lx aligned=0x%lx val=0x%lx -> s=%d P=0x%lx E=%d\n",
        req_id_, t1.sign, t1.Mp, e1, t2.sign, t2.Mp, e2, diff, max_P, min_P, aligned, val, s, P, E);

    return {{s, (int)P}, E};
  }

  align_t alignment(const mul_res_t &res) {
    const uint32_t nt = res.nt;
    const int* const sign = sign_.data();
    const int* const mp = mp_.data();
    const int* const shifts = shifts_.data();
    uint32_t* const Ts = aln_.data();
    int* const stk = stk_.data();

    const int W = W_;
    const int WA = W + 2;
    const uint32_t mask = (1ULL << WA) - 1;
    int global_sticky = 0;

    if (W >= 22 && W <= 28) {
      // A term's shift k = L - its exponent is never negative. Placed 32 + MANT
      // bits up in 64 bits and shifted down by k, the high word is the window
      // value and the low word the bits shifted out. A magnitude is below
      // 2^24 <= 2^WA (hence W >= 22), so past k = WA + MANT the window part is
      // 0 and the low word holds the whole magnitude: capping k at 32 + MANT
      // keeps every bit. That holds while WA <= 32; the path is taken up to
      // W = 28, the widest the differential test sweeps with it.
      for (uint32_t i = 0; i < nt; ++i) {
        const int MANT = (i + 1 == nt) ? (W - 24) : (W - 23);
        const uint64_t placed = uint64_t(uint32_t(mp[i])) << (32 + MANT);
        const uint64_t y = placed >> std::min(shifts[i], 32 + MANT);
        const uint32_t shifted_mag = uint32_t(y >> 32);
        const int s_bit = uint32_t(y) != 0;
        // two's complement of a negative term: (v ^ -1) + 1
        const uint32_t neg = 0u - uint32_t(sign[i]);
        Ts[i] = (((shifted_mag & mask) ^ neg) - neg) & mask;
        stk[i] = s_bit;
        global_sticky |= s_bit;
      }
      return {global_sticky, res.L, res.flags};
    }

    for (uint32_t i = 0; i < nt; ++i) {
      const int MANT = (i + 1 == nt) ? (W - 24) : (W - 23);

      uint32_t mag = mp[i];
      int k = shifts[i];

      uint32_t shifted_mag = 0;
      bool sticky_bit = false;

      if (mag != 0) {
        if (k <= MANT) {
          if (k > (MANT - WA)) {
            shifted_mag = mag << (MANT - k);
          }
        } else {
          if (k < (WA + MANT)) {
            int sh = k - MANT;
            shifted_mag = mag >> sh;
            sticky_bit = (mag & ((1ULL << sh) - 1)) != 0;
          } else {
            sticky_bit = true;
          }
        }
      }

      uint32_t final_val = shifted_mag & mask;
      if (sign[i]) final_val = (~final_val + 1) & mask;

      int s_bit = sticky_bit ? 1 : 0;
      Ts[i] = final_val;
      stk[i] = s_bit;
      global_sticky |= s_bit;
    }

    return {global_sticky, res.L, res.flags};
  }

  // The datapath's carry-save tree over the sign-extended terms: its result,
  // taken modulo the tree width, is their plain sum modulo that width.
  acc_t accumulate(const align_t &a, uint32_t nt) {
    const int Wa = W_ + 2;
    const int Ww = Wa + HR_;
    const uint32_t mask = (1ULL << Ww) - 1;
    const uint32_t* const Ts = aln_.data();
    uint32_t sum = 0;
    for (uint32_t i = 0; i < nt; ++i) {
      sum += sign_ext(Ts[i], Wa);
    }
    return {sum & mask, a.sticky, a.L, a.flags};
  }

  nrm_t normalize(const acc_t &x) {
    const int Ww = (W_ + 2) + HR_;
    const uint32_t mask = (1ULL << Ww) - 1;
    if (x.V == 0) return {0, 0, 0, x.sticky, -1000000000, x.flags, 0, 0, 0};

    int s = ((x.V >> (Ww - 1)) & 1);
    uint32_t Q = -x.V & mask;
    uint32_t X = s ? Q : x.V;
    int msb = 31 - __builtin_clz(X);
    int e = x.L + msb - EXP_POS_BIAS;
    int sh = (msb + 1) - 24;

    uint32_t kept;
    int g = 0, st = x.sticky;
    if (sh >= 0) {
      kept = (X >> sh) & ((1u << 24) - 1);
      uint32_t rem = X & ((1 << sh) - 1);
      g = (sh > 0) ? ((rem >> (sh - 1)) & 1ULL) : 0;
      st = ((rem & ((1ULL << (sh > 1 ? sh - 1 : 0)) - 1)) != 0 || st) ? 1 : 0;
    } else {
      kept = (X << (-sh)) & ((1u << 24) - 1);
    }

    bool lsb_bit = (kept & 1);
    bool round_up = false;
    if (frm_ == FRM_RNE) {
      round_up = (g && (st || lsb_bit));
    }
    return {s, kept, g, st, e, x.flags, X, sh, round_up};
  }

  uint32_t rounding(const nrm_t &r) {
    if (r.flags & FL_NAN) return 0x7fc00000;
    if ((r.flags & FL_PINF) && (r.flags & FL_NINF)) return 0x7fc00000;
    if (r.flags & FL_PINF) return 0x7f800000;
    if (r.flags & FL_NINF) return 0xff800000;

    if (r.kept == 0 && r.st == 0) return 0u;

    const uint32_t s_bit = r.sign << 31;
    bool discarded = (r.g == 1 || r.st == 1);
    bool round_up = false;
    switch (frm_) {
    case FRM_RNE: round_up = (r.g == 1 && (r.st == 1 || (r.kept & 1) == 1)); break;
    case FRM_RTZ: round_up = false; break;
    case FRM_RDN: round_up = (r.sign == 1 && discarded); break;
    case FRM_RUP: round_up = (r.sign == 0 && discarded); break;
    case FRM_RMM: round_up = (r.g == 1); break;
    }

    uint32_t kept_rounded = r.kept + (round_up ? 1 : 0);
    int be = r.e;
    if (kept_rounded & (1u << 24)) { kept_rounded >>= 1; be += 1; }
    // the datapath's result exponent is a signed 10-bit quantity
    be = ((be & 1023) ^ 512) - 512;
    if (be >= 255) return s_bit | 0x7f800000u;
    if (be <= 0) return s_bit;
    return s_bit | ((be & 0xff) << 23) | (kept_rounded & 0x7FFFFFu);
  }

  // members --------
  int exp_bits_, sig_bits_, frm_, W_, HR_;
  bool renorm_, no_window_;
  const uint8_t *sf_a_ = nullptr; // per-lane block scales, null when unscaled
  const uint8_t *sf_b_ = nullptr;
  bool e4m3_scale_ = false;
  uint64_t req_id_ = 0;
  // term buffers, one entry per term (see reserve_terms)
  std::vector<int>      sign_;
  std::vector<int>      mp_;
  std::vector<int>      eps_;
  std::vector<int>      sel_eps_;
  std::vector<int>      nz_;
  std::vector<int>      shifts_;
  std::vector<uint32_t> aln_;
  std::vector<int>      stk_;
};
