#!/usr/bin/bash
# Part B: verify the TPC -> GPC hypothesis with masked cluster launches.
# A cluster's CTAs are co-scheduled in one GPC, one CTA per SM (max smem):
# a cluster that spans every SM of a mask proves those SMs share a GPC; a
# mask whose SMs are in two GPCs cannot hold a cluster larger than either.
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
L=./tpc_lab
t() { echo "### $*"; timeout 30 $L cluster --timeout-s 15 "$@"; echo "exit=$?"; }
# hypothesis: groups of the mask-bit index space
t --cluster 8  --nclusters 1 --tpcs 0-3
t --cluster 16 --nclusters 1 --tpcs 0-3,75,78,81,83
t --cluster 16 --nclusters 1 --tpcs 9-16
t --cluster 16 --nclusters 1 --tpcs 18-25
t --cluster 16 --nclusters 1 --tpcs 27-34
t --cluster 16 --nclusters 1 --tpcs 36-43
t --cluster 16 --nclusters 1 --tpcs 45-52
t --cluster 16 --nclusters 1 --tpcs 54-61
t --cluster 16 --nclusters 1 --tpcs 55-62
t --cluster 16 --nclusters 1 --tpcs 63-70
t --cluster 16 --nclusters 1 --tpcs 64-71
# where do 75,78,81,83 go if not with 0-3: pair each with GPC0's bit 0 (cluster 4 spans both TPCs)
for b in 75 78 81 83; do t --cluster 4 --nclusters 1 --tpcs 0,$b; done
# negative controls: two TPCs from different hypothesised GPCs
t --cluster 4 --nclusters 1 --tpcs 9,18
t --cluster 4 --nclusters 1 --tpcs 0,9
# unmasked placement for comparison
t --cluster 16 --nclusters 7
t --cluster 16 --nclusters 8
