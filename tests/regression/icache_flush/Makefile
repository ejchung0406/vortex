ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := icache_flush

SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

VX_SRCS := $(SRC_DIR)/kernel.cpp $(SRC_DIR)/start.S

OPTS ?= -n64

# This test builds its kernel with the GNU toolchain rather than LLVM (see the
# compiler overrides below), so the ISA string must stay within what the pinned
# GNU assembler accepts.
GNU_KERNEL := 1

include ../common.mk

VX_LDFLAGS = -Wl,-Bstatic,--gc-sections,-T,$(VORTEX_HOME)/sw/kernel/scripts/link$(XLEN).ld,--defsym=STARTUP_ADDR=$(STARTUP_ADDR)

VX_CC  = $(RISCV_TOOLCHAIN_PATH)/bin/$(RISCV_PREFIX)-gcc
VX_CXX = $(RISCV_TOOLCHAIN_PATH)/bin/$(RISCV_PREFIX)-g++
VX_DP  = $(RISCV_TOOLCHAIN_PATH)/bin/$(RISCV_PREFIX)-objdump
VX_CP  = $(RISCV_TOOLCHAIN_PATH)/bin/$(RISCV_PREFIX)-objcopy

# The second image: the same sources with the other transform, at the same address.
all: kernel_b.vxbin
run-simx run-rtlsim run-opae run-xrt run-aved run-firesim run-foldsim: kernel_b.vxbin

kernel_b.elf: $(VX_SRCS) $(VORTEX_KN_PATH)/lib$(KERNEL_LIB).a $(CONFIG_STAMP)
	$(VX_CXX) $(VX_CFLAGS) -DKERNEL_B $(filter-out $(CONFIG_STAMP),$^) $(VX_LDFLAGS) -o $@

kernel_b.vxbin: kernel_b.elf
	OBJCOPY=$(VX_CP) $(VORTEX_HOME)/sw/kernel/scripts/vxbin.py $< $@
