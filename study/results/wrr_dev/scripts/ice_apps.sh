#!/usr/bin/bash
# ice_apps.sh <cfgtag> "<configs>" <app tags...>: submit application points to ICE (SimX, h200s, 1 core), at most 6 wrr_ jobs at a time
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
tag=$1; cf=$2; shift 2
first=1
for a in "$@"; do
  case $a in
  roof64)  app=tmem_roof; args="-m0 -n64 -i256"; x="" ;;
  roof128) app=tmem_roof; args="-m0 -n128 -i256"; x="" ;;
  roof512) app=tmem_roof; args="-m0 -n512 -i256"; x="" ;;
  dxa64)   app=bwstream_dxa; args="-e64 -n256 -g1 -d4 -c0"; x="" ;;
  dxa4k)   app=bwstream_dxa; args="-e4096 -n64 -g1 -d4 -c0"; x="" ;;
  gemm)    app=gemm_block; args="-m16 -n1024 -k4096 -s0"; x="-DMTILES=1 -DPROFILE_ENABLE" ;;
  bw)      app=bwstream; args="-n64 -b32"; x="" ;;
  war)     app=war_order; args=""; x="" ;;
  ldp32)   app=ldstream; args="-t1024 -b8 -u8 -m16 -a"; x="" ;;
  ldl32)   app=ldstream; args="-t1024 -b8 -u8 -m16 -a -l"; x="" ;;
  ldp64)   app=ldstream; args="-t2048 -b8 -u8 -m16 -a"; x="" ;;
  ldl64)   app=ldstream; args="-t2048 -b8 -u8 -m16 -a -l"; x="" ;;
  esac
  while [ "$(ssh -o BatchMode=yes ice 'squeue -u $USER -h -o %j' | grep -c '^wrr_')" -ge 6 ]; do sleep 60; done
  NAME=wrr_${a}_$tag APP=$app ARGS="$args" PROFILE=h200s CORES=1 PERF=4 DRIVER=simx \
    XCONF="$cf $x -DVX_DBG_STALL_TIMEOUT=20000000" SRC=$EXP/src_wr_b ICE_PUSH=$first TIME=6:00:00 $EXP/ice/ice_submit.sh
  first=0
done
