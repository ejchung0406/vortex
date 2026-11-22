#!/bin/bash
# usage: probe.sh <src tree> <xlen> [-D...]
set -e
SRC=$1; XL=$2; shift 2
S=$(dirname $(readlink -f $0))
H=$(mktemp -d $S/h.XXXX)
cd $SRC
XLEN=$XL python3 ci/gen_config.py --config VX_config.toml --output $H/VX_config.vh --format verilog
XLEN=$XL python3 ci/gen_config.py --config VX_types.toml --output $H/VX_types.vh --format verilog --resolved
R=$SRC/hw/rtl
/storage/project/r-hkim358-0/echung67/vortex-tools/verilator/bin/verilator --lint-only -Wno-fatal -Wno-lint -Wno-style --language 1800-2012 \
  -DVX_CFG_XLEN=$XL -DVX_CFG_XLEN_$XL -DSIMULATION -DSV_DPI "$@" -I$H -I$SRC/hw -I$R -I$R/libs -I$R/interfaces -I$SRC/hw/dpi -I$SRC/sw \
  $R/VX_gpu_pkg.sv $S/probe.sv --top-module probe 2>&1 | grep -o "PROBE.*\|%Error.*" | head -5
rm -rf $H
