// Issue rate of ONE warp, and of W warps sharing one SM, by instruction class.
// Every loop body is inline PTX; the SASS is checked with cuobjdump -sass
// (warprate_sass.py) so the instructions executed per iteration are exact.
// Timing: clock64 (SM clock cycles) around the loop, per warp; %globaltimer (ns)
// for the effective clock; %smid for placement.
//   warprate [--iters N] [--reps R] [--only KIND] [--warps LIST]
#include <cuda.h>
#include <cuda_runtime.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <algorithm>
#include <string>
#include "tpc_smctrl.h"

#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { \
  fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, cudaGetErrorString(e_)); exit(1); } } while (0)

#define R2(x) x x
#define R4(x) R2(x) R2(x)
#define R8(x) R4(x) R4(x)
#define R16(x) R8(x) R8(x)
#define R32(x) R16(x) R16(x)
#define R64(x) R32(x) R32(x)
#define R128(x) R64(x) R64(x)

enum Kind {
  K_IDEP, K_IIND4, K_IIND8, K_FDEP, K_FIND4, K_FIND8,
  K_BR1, K_BR4, K_BR16, K_LDS, K_LDSCHASE, K_STS, K_PROD, K_PRODALU, K_LDS16, K_PRODARR, K_ARRONLY, K_WAITONLY, K_MIX, K_NKINDS
};
static const char* kname[K_NKINDS] = {
  "int_dep", "int_ind4", "int_ind8", "fma_dep", "fma_ind4", "fma_ind8",
  "br_every1", "br_every4", "br_every16", "lds_own", "lds_chase", "sts_own", "tma_producer", "producer_alu_only", "lds_own16", "producer_arrive_only", "arrive_no_wait", "try_wait_only", "fma_int_mix"};

struct Rec { long long c0, c1; unsigned long long g0, g1; int smid, spins; };

constexpr int RING = 64;    // per-warp mbarrier/slot ring of the producer kernels
constexpr int SLOT = 16;    // bytes per bulk copy

// Integer chains alternate add and xor so that ptxas cannot fold a run of adds
// into one LEA/IMAD/IADD3 (it does fold add-only chains); %8 = y, %9 = z.
#define A0 "add.u32 %0, %0, %8;\n\t"
#define A1 "add.u32 %1, %1, %8;\n\t"
#define A2 "add.u32 %2, %2, %8;\n\t"
#define A3 "add.u32 %3, %3, %8;\n\t"
#define A4 "add.u32 %4, %4, %8;\n\t"
#define A5 "add.u32 %5, %5, %8;\n\t"
#define A6 "add.u32 %6, %6, %8;\n\t"
#define A7 "add.u32 %7, %7, %8;\n\t"
#define X0 "xor.b32 %0, %0, %9;\n\t"
#define X1 "xor.b32 %1, %1, %9;\n\t"
#define X2 "xor.b32 %2, %2, %9;\n\t"
#define X3 "xor.b32 %3, %3, %9;\n\t"
#define X4 "xor.b32 %4, %4, %9;\n\t"
#define X5 "xor.b32 %5, %5, %9;\n\t"
#define X6 "xor.b32 %6, %6, %9;\n\t"
#define X7 "xor.b32 %7, %7, %9;\n\t"
#define IDEP2 A0 X0
#define IIND4 A0 A1 A2 A3 X0 X1 X2 X3
#define IIND8 A0 A1 A2 A3 A4 A5 A6 A7 X0 X1 X2 X3 X4 X5 X6 X7
#define FFMA4 "fma.rn.f32 %0, %0, %8, %9;\n\tfma.rn.f32 %1, %1, %8, %9;\n\tfma.rn.f32 %2, %2, %8, %9;\n\tfma.rn.f32 %3, %3, %8, %9;\n\t"
#define FFMA8 FFMA4 "fma.rn.f32 %4, %4, %8, %9;\n\tfma.rn.f32 %5, %5, %8, %9;\n\tfma.rn.f32 %6, %6, %8, %9;\n\tfma.rn.f32 %7, %7, %8, %9;\n\t"
// Taken forward branch over a never-executed loop that exits to the branch
// target: a loop cannot be if-converted into predicated instructions, and with
// two predecessors at the target ptxas cannot thread consecutive branches on the
// same predicate into one (both happen with a plain skip; checked in SASS).
#define BRSKIP "{\n\t.reg .pred Q;\n\t@P bra.uni L;\n\tS:\n\tadd.u32 %4, %4, %9;\n\tsetp.ne.u32 Q, %4, 0;\n\t@Q bra.uni S;\n\tL:\n\t}\n\t"
#define BLK4A A1 A2 A3 BRSKIP
#define BLK4B X1 X2 X3 BRSKIP
#define BLK16A A0 A1 A2 A3 X0 X1 X2 X3 A0 A1 A2 A3 X0 X1 X2 BRSKIP
#define BLK16B A0 A1 A2 X3 X0 X1 X2 A3 A0 A1 A2 X3 X0 X1 X2 BRSKIP
// 8 independent loads of the thread's own word, then 4 three-input xors (one
// SASS LOP3 each) into 4 accumulators: ptxas deletes loads whose result is not
// used, even volatile ones, so every loaded value is consumed.
#define LDS8 "ld.volatile.shared.u32 %0, [%12];\n\tld.volatile.shared.u32 %1, [%12];\n\tld.volatile.shared.u32 %2, [%12];\n\tld.volatile.shared.u32 %3, [%12];\n\t" \
             "ld.volatile.shared.u32 %4, [%12];\n\tld.volatile.shared.u32 %5, [%12];\n\tld.volatile.shared.u32 %6, [%12];\n\tld.volatile.shared.u32 %7, [%12];\n\t" \
             "xor.b32 %8, %8, %0;\n\txor.b32 %8, %8, %1;\n\txor.b32 %9, %9, %2;\n\txor.b32 %9, %9, %3;\n\t" \
             "xor.b32 %10, %10, %4;\n\txor.b32 %10, %10, %5;\n\txor.b32 %11, %11, %6;\n\txor.b32 %11, %11, %7;\n\t"
// two groups of 8 loads in different registers, so one group's loads can be in
// flight while the other's results are consumed (16 loads outstanding)
#define LDS16 "ld.volatile.shared.u32 %0, [%20];\n\tld.volatile.shared.u32 %1, [%20];\n\tld.volatile.shared.u32 %2, [%20];\n\tld.volatile.shared.u32 %3, [%20];\n\t" \
              "ld.volatile.shared.u32 %4, [%20];\n\tld.volatile.shared.u32 %5, [%20];\n\tld.volatile.shared.u32 %6, [%20];\n\tld.volatile.shared.u32 %7, [%20];\n\t" \
              "xor.b32 %16, %16, %8;\n\txor.b32 %16, %16, %9;\n\txor.b32 %17, %17, %10;\n\txor.b32 %17, %17, %11;\n\t" \
              "xor.b32 %18, %18, %12;\n\txor.b32 %18, %18, %13;\n\txor.b32 %19, %19, %14;\n\txor.b32 %19, %19, %15;\n\t" \
              "ld.volatile.shared.u32 %8, [%20];\n\tld.volatile.shared.u32 %9, [%20];\n\tld.volatile.shared.u32 %10, [%20];\n\tld.volatile.shared.u32 %11, [%20];\n\t" \
              "ld.volatile.shared.u32 %12, [%20];\n\tld.volatile.shared.u32 %13, [%20];\n\tld.volatile.shared.u32 %14, [%20];\n\tld.volatile.shared.u32 %15, [%20];\n\t" \
              "xor.b32 %16, %16, %0;\n\txor.b32 %16, %16, %1;\n\txor.b32 %17, %17, %2;\n\txor.b32 %17, %17, %3;\n\t" \
              "xor.b32 %18, %18, %4;\n\txor.b32 %18, %18, %5;\n\txor.b32 %19, %19, %6;\n\txor.b32 %19, %19, %7;\n\t"
// FFMA and integer ops interleaved, 8 independent chains each: can one warp
// issue more than one instruction per cycle to different pipes?
#define MIX8 "fma.rn.f32 %0, %0, %16, %17;\n\tadd.u32 %8, %8, %18;\n\tfma.rn.f32 %1, %1, %16, %17;\n\tadd.u32 %9, %9, %18;\n\t" \
             "fma.rn.f32 %2, %2, %16, %17;\n\tadd.u32 %10, %10, %18;\n\tfma.rn.f32 %3, %3, %16, %17;\n\tadd.u32 %11, %11, %18;\n\t" \
             "fma.rn.f32 %4, %4, %16, %17;\n\tadd.u32 %12, %12, %18;\n\tfma.rn.f32 %5, %5, %16, %17;\n\tadd.u32 %13, %13, %18;\n\t" \
             "fma.rn.f32 %6, %6, %16, %17;\n\tadd.u32 %14, %14, %18;\n\tfma.rn.f32 %7, %7, %16, %17;\n\tadd.u32 %15, %15, %18;\n\t" \
             "fma.rn.f32 %0, %0, %16, %17;\n\txor.b32 %8, %8, %19;\n\tfma.rn.f32 %1, %1, %16, %17;\n\txor.b32 %9, %9, %19;\n\t" \
             "fma.rn.f32 %2, %2, %16, %17;\n\txor.b32 %10, %10, %19;\n\tfma.rn.f32 %3, %3, %16, %17;\n\txor.b32 %11, %11, %19;\n\t" \
             "fma.rn.f32 %4, %4, %16, %17;\n\txor.b32 %12, %12, %19;\n\tfma.rn.f32 %5, %5, %16, %17;\n\txor.b32 %13, %13, %19;\n\t" \
             "fma.rn.f32 %6, %6, %16, %17;\n\txor.b32 %14, %14, %19;\n\tfma.rn.f32 %7, %7, %16, %17;\n\txor.b32 %15, %15, %19;\n\t"
#define STS1 "st.volatile.shared.u32 [%8], %0;\n\t"

template <int K>
__global__ void __launch_bounds__(1024, 2) kern(int iters, unsigned y, Rec* rec, unsigned* sink, const uint4* gsrc) {
  extern __shared__ __align__(128) unsigned char smem[];
  const int lane = threadIdx.x & 31, w = threadIdx.x >> 5;
  const uint32_t sbase = (uint32_t)__cvta_generic_to_shared(smem);
  // per thread word region (lds/sts), per warp ring (producer)
  const uint32_t myword = sbase + threadIdx.x * 4;
  const uint32_t ring_bar = sbase + w * (RING * 8);
  const uint32_t ring_dat = sbase + (blockDim.x / 32) * (RING * 8) + w * (RING * SLOT);
  if (K == K_LDS || K == K_LDSCHASE || K == K_STS || K == K_LDS16) {
    reinterpret_cast<uint32_t*>(smem)[threadIdx.x] = myword;   // self pointer for the chase
  }
  if (K == K_PROD || K == K_PRODARR || K == K_ARRONLY || K == K_WAITONLY) {
    for (int i = lane; i < RING; i += 32)
      asm volatile("mbarrier.init.shared::cta.b64 [%0], 1;" :: "r"(ring_bar + i * 8) : "memory");
  }
  if (K == K_PROD || K == K_PRODARR || K == K_ARRONLY || K == K_WAITONLY) asm volatile("fence.mbarrier_init.release.cluster;" ::: "memory");
  __syncthreads();
  uint32_t a0 = threadIdx.x, a1 = a0 + 1, a2 = a0 + 2, a3 = a0 + 3, a4 = a0 + 4, a5 = a0 + 5, a6 = a0 + 6, a7 = a0 + 7;
  float f0 = 1.f + lane, f1 = f0 + 1, f2 = f0 + 2, f3 = f0 + 3, f4 = f0 + 4, f5 = f0 + 5, f6 = f0 + 6, f7 = f0 + 7;
  const float fa = 0.999f + 1e-7f * (float)(y & 7), fb = 1e-3f;
  const uint32_t z = y * 0x9e3779b1u;
  const uint32_t py = y;    // branch predicate source: y != 0 at run time, unknown to the compiler
  uint32_t b[16] = {};
  if (K == K_LDSCHASE) a0 = myword;   // the chase starts at the thread's own self-pointing word
  uint32_t spins = 0;   // producer: try_wait executions (= iterations when it never waits)
  __syncwarp();
  long long c0; unsigned long long g0;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(g0));
  c0 = clock64();
#pragma unroll 1
  for (int it = 0; it < iters; ++it) {
    if (K == K_IDEP) {
      asm volatile(R64(IDEP2) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z));
    } else if (K == K_IIND4) {
      asm volatile(R16(IIND4) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z));
    } else if (K == K_IIND8) {
      asm volatile(R8(IIND8) : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z));
    } else if (K == K_FDEP) {
      asm volatile(R128("fma.rn.f32 %0, %0, %1, %2;\n\t") : "+f"(f0) : "f"(fa), "f"(fb));
    } else if (K == K_FIND4) {
      asm volatile(R32(FFMA4) : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3), "+f"(f4), "+f"(f5), "+f"(f6), "+f"(f7) : "f"(fa), "f"(fb));
    } else if (K == K_FIND8) {
      asm volatile(R16(FFMA8) : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3), "+f"(f4), "+f"(f5), "+f"(f6), "+f"(f7) : "f"(fa), "f"(fb));
    } else if (K == K_BR1) {
      asm volatile("{\n\t.reg .pred P;\n\tsetp.ne.u32 P, %10, 0;\n\t" R128(BRSKIP) "}\n\t"
                   : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z), "r"(py));
    } else if (K == K_BR4) {
      asm volatile("{\n\t.reg .pred P;\n\tsetp.ne.u32 P, %10, 0;\n\t" R16(BLK4A BLK4B) "}\n\t"
                   : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z), "r"(py));
    } else if (K == K_BR16) {
      asm volatile("{\n\t.reg .pred P;\n\tsetp.ne.u32 P, %10, 0;\n\t" R4(BLK16A BLK16B) "}\n\t"
                   : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z), "r"(py));
    } else if (K == K_LDS) {
      uint32_t l0, l1, l2, l3, l4, l5, l6, l7;
      asm volatile(R16(LDS8) : "=&r"(l0), "=&r"(l1), "=&r"(l2), "=&r"(l3), "=&r"(l4), "=&r"(l5), "=&r"(l6), "=&r"(l7),
                   "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3) : "r"(myword) : "memory");
    } else if (K == K_LDS16) {
      asm volatile(R8(LDS16) : "+r"(b[0]), "+r"(b[1]), "+r"(b[2]), "+r"(b[3]), "+r"(b[4]), "+r"(b[5]), "+r"(b[6]), "+r"(b[7]),
                   "+r"(b[8]), "+r"(b[9]), "+r"(b[10]), "+r"(b[11]), "+r"(b[12]), "+r"(b[13]), "+r"(b[14]), "+r"(b[15]),
                   "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3) : "r"(myword) : "memory");
    } else if (K == K_MIX) {
      asm volatile(R4(MIX8) : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3), "+f"(f4), "+f"(f5), "+f"(f6), "+f"(f7),
                   "+r"(b[0]), "+r"(b[1]), "+r"(b[2]), "+r"(b[3]), "+r"(b[4]), "+r"(b[5]), "+r"(b[6]), "+r"(b[7])
                   : "f"(fa), "f"(fb), "r"(y), "r"(z));
    } else if (K == K_LDSCHASE) {
      asm volatile(R128("ld.volatile.shared.u32 %0, [%0];\n\t") : "+r"(a0) :: "memory");
    } else if (K == K_STS) {
      asm volatile(R128(STS1) :: "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7), "r"(myword) : "memory");
    } else if (K == K_PROD || K == K_PRODALU || K == K_PRODARR || K == K_ARRONLY || K == K_WAITONLY) {
      // TMA producer: wait for the slot's previous fill, arrive.expect_tx, one bulk copy (lane 0),
      // then scalar bookkeeping: the address/parity arithmetic plus 8 add/xor on 4 chains,
      // ~30 scalar SASS instructions per iteration in all (warprate_sass.py).
      const uint32_t s = it & (RING - 1);
      const uint32_t par = ((it / RING) & 1) ^ 1;
      if (K == K_PROD) {
        asm volatile(
            "{\n\t.reg .pred W, E;\n\t"
            "WAIT:\n\t"
            "add.u32 %0, %0, 1;\n\t"
            "mbarrier.try_wait.parity.shared::cta.b64 W, [%1], %2;\n\t"
            "@!W bra WAIT;\n\t"
            "setp.eq.u32 E, %5, 0;\n\t"
            "@E mbarrier.arrive.expect_tx.shared::cta.b64 _, [%1], %6;\n\t"
            "@E cp.async.bulk.shared::cluster.global.mbarrier::complete_tx::bytes [%3], [%4], %6, [%1];\n\t"
            "}\n\t"
            : "+r"(spins) : "r"(ring_bar + s * 8), "r"(par), "r"(ring_dat + s * SLOT), "l"(gsrc + (s + w * RING)), "r"(lane), "n"(SLOT)
            : "memory");
      }
      if (K == K_PRODARR) {   // the same wait and lane-0 arrive, without expect_tx and the bulk copy
        asm volatile(
            "{\n\t.reg .pred W, E;\n\t"
            "WAIT:\n\t"
            "add.u32 %0, %0, 1;\n\t"
            "mbarrier.try_wait.parity.shared::cta.b64 W, [%1], %2;\n\t"
            "@!W bra WAIT;\n\t"
            "setp.eq.u32 E, %3, 0;\n\t"
            "@E mbarrier.arrive.shared::cta.b64 _, [%1];\n\t"
            "}\n\t"
            : "+r"(spins) : "r"(ring_bar + s * 8), "r"(par), "r"(lane) : "memory");
      }
      if (K == K_ARRONLY) {   // lane-0 arrive on the slot's barrier (count 1: completes its phase), no wait
        asm volatile("{\n\t.reg .pred E;\n\tsetp.eq.u32 E, %1, 0;\n\t@E mbarrier.arrive.shared::cta.b64 _, [%0];\n\t}\n\t"
                     :: "r"(ring_bar + s * 8), "r"(lane) : "memory");
      }
      if (K == K_WAITONLY) {  // try_wait for the phase before the barrier's first: true at once, never arrived on
        asm volatile(
            "{\n\t.reg .pred W;\n\t"
            "WAIT:\n\t"
            "add.u32 %0, %0, 1;\n\t"
            "mbarrier.try_wait.parity.shared::cta.b64 W, [%1], 1;\n\t"
            "@!W bra WAIT;\n\t"
            "}\n\t"
            : "+r"(spins) : "r"(ring_bar + s * 8) : "memory");
      }
      asm volatile(A0 A1 A2 A3 X0 X1 X2 X3 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7) : "r"(y), "r"(z));
    }
  }
  const long long c1 = clock64();
  unsigned long long g1;
  asm volatile("mov.u64 %0, %%globaltimer;" : "=l"(g1));
  if (K == K_PROD || K == K_PRODARR) {   // drain: wait for the last fill of every slot
    for (int s = 0; s < RING && s < iters; ++s) {
      const int last = ((iters - 1 - s) / RING) * RING + s;
      const uint32_t par = (last / RING) & 1;
      asm volatile("{\n\t.reg .pred W;\n\tWAIT2:\n\tmbarrier.try_wait.parity.shared::cta.b64 W, [%0], %1;\n\t@!W bra WAIT2;\n\t}"
                   :: "r"(ring_bar + s * 8), "r"(par) : "memory");
    }
  }
  uint32_t smid;
  asm volatile("mov.u32 %0, %%smid;" : "=r"(smid));
  unsigned v = a0 ^ a1 ^ a2 ^ a3 ^ a4 ^ a5 ^ a6 ^ a7 ^ __float_as_uint(f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7);
  for (int i = 0; i < 16; ++i) v ^= b[i];
  if (v == 0x9e3779b9u) sink[0] = v;
  if (lane == 0) {
    Rec& r = rec[blockIdx.x * (blockDim.x / 32) + w];
    r.c0 = c0; r.c1 = c1; r.g0 = g0; r.g1 = g1; r.smid = (int)smid; r.spins = (int)spins;
  }
}

typedef void (*KFn)(int, unsigned, Rec*, unsigned*, const uint4*);
static KFn kfn[K_NKINDS] = {kern<0>, kern<1>, kern<2>, kern<3>, kern<4>, kern<5>, kern<6>,
                            kern<7>, kern<8>, kern<9>, kern<10>, kern<11>, kern<12>, kern<13>, kern<14>, kern<15>, kern<16>, kern<17>, kern<18>};

int main(int argc, char** argv) {
  int iters = 4000, reps = 3, only = -1;
  std::vector<int> wl = {1, 2, 4, 8, 16, 32, 64};
  bool nomask = false;
  for (int i = 1; i < argc; ++i) {
    auto is = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (is("--iters")) iters = atoi(argv[++i]);
    else if (is("--reps")) reps = atoi(argv[++i]);
    else if (is("--only")) only = atoi(argv[++i]);
    else if (is("--warps")) { wl.clear(); char* s = argv[++i]; for (char* t = strtok(s, ","); t; t = strtok(nullptr, ",")) wl.push_back(atoi(t)); }
    else if (!strcmp(argv[i], "--nomask")) nomask = true;
    else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 1; }
  }
  int dev; CK(cudaGetDevice(&dev));
  cudaDeviceProp prop; CK(cudaGetDeviceProperties(&prop, dev));
  printf("device=%s sms=%d clock_khz=%d\n", prop.name, prop.multiProcessorCount, prop.clockRate);
  Rec* rec; unsigned* sink; uint4* gsrc;
  CK(cudaMalloc(&rec, sizeof(Rec) * 4096)); CK(cudaMalloc(&sink, 64)); CK(cudaMalloc(&gsrc, 1 << 20));
  CK(cudaMemset(gsrc, 1, 1 << 20));
  const size_t smem = 32 * (RING * 8) + 32 * (RING * SLOT) + 4096;  // 32 warps per block max
  for (int k = 0; k < K_NKINDS; ++k)
    CK(cudaFuncSetAttribute(kfn[k], cudaFuncAttributeMaxDynamicSharedMemorySize, (int)smem));
  std::vector<Rec> h(4096);
  for (int k = 0; k < K_NKINDS; ++k) {
    if (only >= 0 && k != only) continue;
    for (int W : wl) {
      // W <= 32: one CTA of W warps. W = 64: CTAs of 32 warps confined to one TPC (2 SMs),
      // 4 CTAs so each SM holds 2 (64 warps); records are grouped by smid.
      const int bw = W <= 32 ? W : 32;
      const int nb = W <= 32 ? 1 : 4;
      const int it = (k == K_LDSCHASE || k == K_FDEP || k == K_IDEP) ? iters / 2 : iters;
      double best_warp = 1e30, best_sm = 1e30, best_ghz = 0; int best_nsm = 0, best_wsm = 0, best_spin = 0;
      for (int r = 0; r < reps + 1; ++r) {
        if (!nomask) tpc_next_enable(u128_bit(0));
        kfn[k]<<<nb, bw * 32, smem>>>(it, 3u, rec, sink, gsrc);
        CK(cudaGetLastError()); CK(cudaDeviceSynchronize());
        if (r == 0) continue;   // warm-up
        CK(cudaMemcpy(h.data(), rec, sizeof(Rec) * nb * bw, cudaMemcpyDeviceToHost));
        // group by SM
        std::vector<int> sms;
        for (int i = 0; i < nb * bw; ++i) sms.push_back(h[i].smid);
        std::sort(sms.begin(), sms.end()); sms.erase(std::unique(sms.begin(), sms.end()), sms.end());
        double worst_sm = 0, worst_warp = 0, ghz = 0; int wsm_min = 1 << 30, maxspin = 0;
        for (int i = 0; i < nb * bw; ++i) maxspin = std::max(maxspin, h[i].spins);
        for (int sm : sms) {
          long long lo = LLONG_MAX, hi = 0; unsigned long long glo = ~0ull, ghi = 0; int n = 0;
          for (int i = 0; i < nb * bw; ++i) if (h[i].smid == sm) {
            lo = std::min(lo, h[i].c0); hi = std::max(hi, h[i].c1);
            glo = std::min(glo, h[i].g0); ghi = std::max(ghi, h[i].g1); ++n;
            worst_warp = std::max(worst_warp, (double)(h[i].c1 - h[i].c0) / it);
          }
          worst_sm = std::max(worst_sm, (double)(hi - lo) / it);
          ghz = (double)(hi - lo) / (double)(ghi - glo);
          wsm_min = std::min(wsm_min, n);
        }
        if (worst_sm < best_sm) { best_sm = worst_sm; best_warp = worst_warp; best_ghz = ghz; best_nsm = (int)sms.size(); best_wsm = wsm_min; best_spin = maxspin; }
      }
      // cyc_per_iter_sm: SM span (first warp start .. last warp end) per loop iteration;
      // cyc_per_iter_warp: slowest warp's own loop time per iteration.
      printf("kind=%s W=%d iters=%d nsm=%d warps_per_sm=%d cyc_per_iter_warp=%.2f cyc_per_iter_sm=%.2f ghz=%.3f max_trywait=%d\n",
             kname[k], W, it, best_nsm, best_wsm, best_warp, best_sm, best_ghz, best_spin);
      fflush(stdout);
    }
  }
  return 0;
}
