#!/usr/bin/bash
# Part B follow-up: the four TPCs at mask bits 75, 78, 81, 83 (SMs 124-131)
cd /storage/project/r-hkim358-0/echung67/vortex-exp/gpu
t() { echo "### $*"; timeout 30 ./tpc_lab cluster --timeout-s 10 "$@"; echo "exit=$?"; }
for b in 75 78 81 83; do t --cluster 2 --nclusters 1 --tpcs $b; done
t --cluster 4 --nclusters 1 --tpcs 75,78
t --cluster 4 --nclusters 1 --tpcs 81,83
t --cluster 4 --nclusters 1 --tpcs 75,83
t --cluster 8 --nclusters 1 --tpcs 75,78,81,83
for b in 75 78 81 83; do for o in 9 54 63; do t --cluster 4 --nclusters 1 --tpcs $o,$b; done; done
t --cluster 8 --nclusters 2 --tpcs 0-3,75,78,81,83
t --cluster 4 --nclusters 4 --tpcs 0-3,75,78,81,83
t --cluster 2 --nclusters 66
