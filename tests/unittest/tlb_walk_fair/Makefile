ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := tlb_walk_fair
SIMX_DIR := $(VORTEX_HOME)/sim/simx

SRCS := $(VORTEX_HOME)/tests/unittest/$(PROJECT)/main.cpp
SRCS += $(SIMX_DIR)/mem/tlb_l2.cpp $(SIMX_DIR)/mem/mmu_tlb.cpp
CXXFLAGS += -I$(SIMX_DIR) -I$(SIM_COMMON_DIR) -DVX_CFG_VM_ENABLE
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN)
LDFLAGS += -pthread

include ../common.mk

# SimX register layouts use GNU anonymous structs.
CXXFLAGS := $(filter-out -pedantic,$(CXXFLAGS))
