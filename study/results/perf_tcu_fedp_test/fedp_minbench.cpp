#include "fedp_ref.h"
#include "fedp_v1.h"
#include "fedp_cur.h"
#include <chrono>
#include <random>
#include <cstdio>
#include <vector>
#include <algorithm>
template <typename F>
double trial(F& f, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, const std::vector<float>& c, uint32_t n, uint32_t& sink) {
  const int N = (int)c.size();
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) { float d = f(&a[i * n], &b[i * n], c[i], n); uint32_t u; memcpy(&u, &d, 4); sink += u; }
  return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
}
int main(int argc, char** argv) {
  int eb = atoi(argv[1]), sb = atoi(argv[2]); uint32_t n = atoi(argv[3]); int T = argc > 4 ? atoi(argv[4]) : 200;
  const int N = 512;
  std::mt19937 rng(1);
  std::vector<uint32_t> a(N * n), b(N * n); std::vector<float> c(N);
  uint32_t width = 1 + eb + sb;
  uint32_t epw = (width <= 16 && 32 % width == 0) ? 32 / width : 1;
  uint32_t m = 0; for (uint32_t e = 0; e < epw; ++e) m |= (1u << (e * width + width - 2));
  for (auto& x : a) { x = rng() & ~m; }
  for (auto& x : b) { x = rng() & ~m; }
  for (auto& f : c) f = float(int(rng() % 2000) - 1000) / 7.0f;
  TfrFedpRef r(eb, sb); TfrFedpV1 v1(eb, sb); TfrFedpCur cur(eb, sb);
  double br = 1e30, b1 = 1e30, bc = 1e30; uint32_t sink = 0;
  for (int t = 0; t < T; ++t) {
    br = std::min(br, trial(r, a, b, c, n, sink));
    b1 = std::min(b1, trial(v1, a, b, c, n, sink));
    bc = std::min(bc, trial(cur, a, b, c, n, sink));
  }
  printf("e%dm%d n=%u min ns/dot: ref %.1f  v1 %.1f  cur %.1f   (sink %u)\n", eb, sb, n, br, b1, bc, sink);
}
