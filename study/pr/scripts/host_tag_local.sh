#!/usr/bin/bash
# Local SimX build+run: host_tag_local.sh <src copy> <name> <runsh> <xlen>
SRC=$1; NAME=$2; RUNSH=$3; XLEN=${4:-64}
EXP=/storage/project/r-hkim358-0/echung67/vortex-exp
TOOLS=/storage/project/r-hkim358-0/echung67/vortex-tools
export OUT=$EXP/results/$NAME
BUILD=/tmp/$USER/$NAME
export VORTEX_VM_MEGAPAGE_HEAP=${VORTEX_VM_MEGAPAGE_HEAP:-1}
export MAKEFLAGS=-j3
rm -rf "$BUILD" "$OUT"; mkdir -p "$OUT" "$BUILD"
source "$TOOLS/env.sh"
cd "$BUILD"
{ echo "name=$NAME"; echo "src=$SRC"; echo "host=$(hostname)"; echo "xlen=$XLEN"; echo "aslr=$(cat /proc/sys/kernel/randomize_va_space)"; echo "local=1"; } > "$OUT/meta.txt"
cp "$RUNSH" "$OUT/runsh.sh"
nice "$SRC/configure" --xlen="$XLEN" --tooldir="$TOOLS" > "$OUT/configure.log" 2>&1
nice make -s -j3 software > "$OUT/build.log" 2>&1 || { echo "rc=build" >> "$OUT/meta.txt"; exit 1; }
start=$(date +%s)
nice bash "$RUNSH" > "$OUT/run.log" 2>&1 && rc=0 || rc=$?
echo "wall_s=$(( $(date +%s) - start ))" >> "$OUT/meta.txt"
echo "rc=$rc" >> "$OUT/meta.txt"
