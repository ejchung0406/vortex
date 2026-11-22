# simx-l2-fwd-hold: not a separate pull request (folded into simx-dcache-fwd-hold)

## State

No branch `pr/simx-l2-fwd-hold` and no worktree were created. Nothing to send.

## Why

The fork's commit 462002754 adds `VX_CFG_L2_FWD_HOLD` (default 0) so that the L2's
banks get the same fill-forward hold as the dcache's `VX_CFG_DCACHE_FWD_HOLD`.
On `pr/simx-dcache-fwd-hold` the hold is unconditional in the shared SimX
`CacheBank`, as it is in every RTL `VX_cache_bank`, so the L2 (and the icache)
already hold there. That leaves this branch no hold code.

The other part of 462002754 passes `VX_CFG_L2_CRSQ_SIZE` to the model. On
upstream this does not make the L2 depth real: the L2 does not read-merge, and
the depth is modelled only on the read-merge response path. A non-zero L2
CRSQ would hit the "modeled under read merging only" assertion. Making it real
means sizing the bank inputs of the response crossbar (`TxRxCrossBar::RspIn` in
`sim/simx/types.h`, including its 1x1 bypass). That is a separate change, not
done here.

## L2 evidence (runs made for this effort, XLEN 64)

Knob copy (`src_pr_simx-dcache-fwd-hold_knob`), dcache hold on, L2 hold off -> on;
branch copy = both on:

| case | L2 replay | L2 hold (= branch) | rtlsim |
|---|---|---|---|
| sgemv -m512 -n512, 4 cores, 16w x 16t, L2 | 174,825 | 173,134 | 173,946 (+0.5% -> -0.5%) |
| the same, 8 cores, 2 L2 banks | 176,241 | 176,881 | not run (job timed out at the wind-down) |
| sgemm -n128 8x8 on 2/4/8 cores, 1 or 2 L2 banks; vecadd 4 cores; raycast 4 cores | identical | identical | not run |
| every existing multi-core parity case | identical | identical | (see the dcache note) |

rtlsim parity for the L2 hold is not shown: the only case where it moves cycles
lands as far below rtlsim (-0.5%) as the replay lands above it (+0.5%). This
part is lower confidence than the dcache part. Results:
`vortex-exp/results/pr_fh_l2_*`.

Out of scope, mentioned only (fork measurement): `bwstream_dxa` on 4 cores
(16x16, L2) takes 19,874 cycles in SimX and 18,321 in rtlsim with either
setting.

## If a separate L2 change is still wanted

The honest options are: (a) nothing, since the hold is in A; or (b) a branch
that models the core-response queue depth on the crossbar response path so
that `VX_CFG_L2_CRSQ_SIZE` takes effect, with its own rtlsim check at
`L2_CRSQ_SIZE=2`.
