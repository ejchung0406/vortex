#!/bin/bash
# usage: ut_lint.sh <src tree> [-D...]: Verilator lint of hw/unittest/dxa_core's top, as its Makefile builds it
SRC=$1; shift
S=$(dirname $(readlink -f $0)); H=$(mktemp -d $S/h.XXXX); cd $SRC
XLEN=${XLEN:-64} python3 ci/gen_config.py --config VX_config.toml --output $H/VX_config.vh --format verilog
XLEN=${XLEN:-64} python3 ci/gen_config.py --config VX_types.toml --output $H/VX_types.vh --format verilog --resolved
R=$SRC/hw/rtl; sed "s#@[A-Z_]*@##g" hw/unittest/verilator.vlt.in > $H/verilator.vlt 2>/dev/null
/storage/project/r-hkim358-0/echung67/vortex-tools/verilator/bin/verilator --lint-only -DVX_CFG_EXT_DXA_ENABLE --language 1800-2012 --assert -Wall -Wpedantic -Wno-DECLFILENAME -Wno-REDEFMACRO -Wno-GENUNNAMED $H/verilator.vlt -DSIMULATION -DVX_CFG_XLEN=${XLEN:-64} -DVX_CFG_XLEN_${XLEN:-64} -DNDEBUG "$@" -I$H -I$SRC/sw -I$SRC/hw -I$R -I$SRC/hw/dpi -I$R/libs -I$R/interfaces -I$R/mem -I$R/vm -I$R/dxa -I$SRC/hw/unittest/dxa_core $R/VX_gpu_pkg.sv $R/dxa/VX_dxa_pkg.sv $SRC/hw/unittest/dxa_core/VX_dxa_core_top.sv --top-module VX_dxa_core_top 2>&1 | grep "^%" | sed "s#$SRC/##" | cut -c1-170 | head -6
echo "exit=${PIPESTATUS[0]}"; rm -rf $H
