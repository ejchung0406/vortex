# DCACHE_CHANNELS knob (branch feature/dcache-channels, worktree vortex-exp/wt_dcchan)

Started 2026-11-23 from c257766a2. Goal: let an LSU block keep two dcache
channels with a 64 B dcache word (h200s: 8 lanes x 8 B), so a line-aligned
load stream is one word per channel pair and blocks touching two words are
not serialised.

## What SimX's coalescer does (sim/simx/mem/mem_coalescer.cpp at c257766a2)

- output_ratio_ = lanes / outputs. Output o owns lanes [o*G, (o+1)*G).
  With 8 lanes, 2 outputs: lanes 0-3 -> channel 0, lanes 4-7 -> channel 1.
- Per round, each output seeds on its lowest unsent active lane and takes the
  group's unsent lanes whose word address (addr & ~(word-1)) equals the seed's
  (AMO: no merge). All outputs of a round go out together (out_round_), build
  tick and drain tick alternate.
- 3+ distinct words in a group: one word per round per group, lowest lane first.
- Misses: one per round that is not the request's last.
- Line-aligned stream at word 64 / 2 ch: both channels carry the same 64 B
  word; the dcache's READ_MERGE (= DCACHE_CHANNELS) serves both with one bank
  access.
- This is a fixed lane partition, the same structure the RTL coalescer already
  had (group = DATA_RATIO lanes); one cycle in hardware. So no change of rule:
  the RTL now takes the group from OUT_REQS (= DCACHE_CHANNELS).
- Not chosen: grouping by word across the whole block (seed 0, then seed 1
  among the rest) would put alternating-lane patterns (A,B,A,B,...) in one
  round instead of two, but needs a second seed/compare stage behind the first
  and changes the measured SimX behaviour.

## Knob

VX_CFG_DCACHE_CHANNELS in VX_config.toml, default
up(lanes * (XLEN//8) // word) = the old derivation (checked equal for XLEN
32/64 x NT 1..32). RTL VX_gpu_pkg DCACHE_CHANNELS and SimX constants.h take it.
Asserts: divides NUM_LSU_LANES; equals it without a coalescer.

## Runs (rover, prefix dcc_)

Sources: dcc_base = c257766a2; dcc_w1 = 0833b3a91 (knob + RTL + lane_words).

### Identity, knob unset (h200s, 1 SM, SimX; dcc_b_* = c257766a2, dcc_i_* = 162331892)
fetch_squash 10,018 | warp_rate 113,382 | tmem_roof -n512 -i8 62,075 |
bwstream -n2048 -u4 -b32 406,539 | gemm_block 571,376 | gemm_tmem_decode 1,320,528:
all identical in cycles and instructions. (Base also: ldstream -l 244,813,
ldstream 327,131, war_order 343,239.)

### Parity, small config (XLEN 64, NT 8, word 64), SimX / rtlsim cycles
lane_words ch2 39,083 / 39,096; ch2 + ISSUE_WIDTH 2 39,046 / 39,024;
ch4 39,206 / 39,177; ldstream -t32 -b8 -u4 -m0.125 -l ch2 23,611 / 23,624.
Instructions and coalescer misses equal (lane_words misses 320 ch2, 160 ch4).
lane_words also passes with ch1 (word 64 default) on both.

### h200s, 1 SM, SimX cycles (dcc_b_* base c257766a2; dcc_k_* / dcc_kc_* on 162331892)
| kernel | 32 B, 2 ch (base) | 64 B, 2 ch | 64 B, 2 ch, CRSQ 16 |
|---|---|---|---|
| ldstream -t2048 -b8 -u8 -m16 -a -l | 244,813 | 187,314 | 181,614 |
| ldstream -t2048 -b8 -u8 -m16 -a | 327,131 | 316,205 | 315,885 |
| bwstream -n2048 -u4 -b32 | 406,539 | 406,562 | 403,996 |
| gemm_block (MTILES=1) | 571,376 | 572,323 | 574,021 |
| war_order | 343,239 | 343,220 | 321,423 |
| gemm_tmem_decode (KSTEPS=2 PIPE=6 XFER=8192) | 1,320,528 | 1,317,208 | 1,317,208 |
| warp_rate | 113,382 | 113,382 | 113,382 |
Coalescer misses with 64 B / 2 ch: 0 on both ldstreams (base 512), war_order 221,184 both.
Profile commit 879f87625 (h200s word 64, ch 2): ldstream -l with the profile alone
(dcc_p_ldl) 187,314 = the -D run.

cache_ways-2 config (DCACHE as LLC, SET_HASH) passes on SimX at default and with
NT8/word64/ch2 (SimX 18,582, rtlsim 18,623).

Beacon area/depth: rover ~/vortex-rover/dcc_beacon/run.sh, results
~/vortex-rover/results/dcc_beacon (two-core top_vortex, XLEN 64, NT 8; tools
from fcbeacon.WZHp6F).

### Beacon (rover, 2026-11-23): two-core top_vortex, XLEN 64, NT 8, source 879f87625
Banks follow the default NUM_BANKS = pow2(min(DCACHE_NUM_REQS,16)) unless set.
| build | max lv | deep ends | LUT | FF | BRAM | DSP | p_fmax (est.) |
|---|---|---|---|---|---|---|---|
| A 32 B, 2 ch (2 banks) | 26 | 77,832 | 376,357 | 298,860 | 100 | 66 | 253.2 |
| B 64 B, 1 ch (1 bank) | 26 | 76,246 | 379,133 | 282,198 | 79 | 66 | 253.2 |
| C 64 B, 2 ch (2 banks) | 26 | 79,704 | 440,128 | 323,988 | 98 | 66 | 236.0 |
| D 64 B, 2 ch, NUM_BANKS=1 | 26 | 76,536 | 387,064 | 296,144 | 79 | 66 | 248.5 |
| E 64 B, 1 ch, NUM_BANKS=2 | 26 | 78,270 | 431,473 | 310,030 | 98 | 66 | 234.1 |
--against across configurations is refused (config hash), so no per-register
compare; max depth is 26 in all five. C - B = +61.0k LUT: of it the second
64-byte bank (E - B = +52.3k: bank datapath, and the 64-byte AMO unit,
15.0k LUT per bank against 3.3k at 32 B) and the second channel (D - B =
+7.9k LUT, +13.9k FF over two cores: per socket the cache bypass +2.0k, the
LSU memory path +1.1k LUT/+3.3k FF, dMMU +1.7k, dcache request crossbar
+1.2k). h200s fixes NUM_BANKS at 4, so there the channel costs the D - B
kind of area; the word 32 -> 64 widens its four banks (per bank 7.7k ->
20.9k LUT here, mostly the AMO unit).

### RTL identity, knob unset: uniform_ld NT16 rtlsim, c257766a2 and 162331892: 55,899 both.

### h200s rtlsim, word 64, ch 2 (source 162331892, model dcc_m2_kh_rtl, build 46 min, peak 77 GB)
ldstream -t2048 -b8 -u8 -m16 -a -l: rtlsim 172,345 vs SimX 187,314 (-8.0%), instrs equal.
war_order: rtlsim 329,894 vs SimX 343,220 (-3.9%); at 602af5c84 without the knob the gap was -4.18%.
No base-config rtlsim for ldstream -l exists, so whether the -8% is new is open.
