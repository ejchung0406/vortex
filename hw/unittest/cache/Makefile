ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := cache

RTL_DIR := $(VORTEX_HOME)/hw/rtl
DPI_DIR := $(VORTEX_HOME)/hw/dpi

SRC_DIR := $(VORTEX_HOME)/hw/unittest/cache

CXXFLAGS := -I$(SRC_DIR) -I$(VORTEX_HOME)/hw/unittest/common -I$(SW_COMMON_DIR)
CXXFLAGS += -I$(ROOT_DIR)/sw -I$(ROOT_DIR)/hw

SRCS += $(SRC_DIR)/main.cpp

DBG_TRACE_FLAGS := -DDBG_TRACE_CACHE

RTL_PKGS := $(RTL_DIR)/VX_gpu_pkg.sv

RTL_INCLUDE := -I$(ROOT_DIR)/sw -I$(ROOT_DIR)/hw -I$(RTL_DIR) -I$(DPI_DIR) -I$(RTL_DIR)/libs
RTL_INCLUDE += -I$(RTL_DIR)/interfaces -I$(RTL_DIR)/mem -I$(RTL_DIR)/vm -I$(RTL_DIR)/cache -I$(SRC_DIR)

TOP := VX_cache_top

ifeq ($(PROJECT),cache_lazy_fair)
SRCS := $(VORTEX_HOME)/tests/unittest/cache_lazy_fair/rtl.cpp
PARAMS += --Mdir obj_lazy_fair
PARAMS += -GNUM_REQS=1 -GNUM_BANKS=1 -GNUM_WAYS=1 -GCACHE_SIZE=2048
PARAMS += -GLINE_SIZE=64 -GSECTOR_SIZE=64 -GWORD_SIZE=64 -GLATENCY=2 -GMSHR_SIZE=16
PARAMS += -GWRITEBACK=1 -GDIRTY_BYTES=1 -GLAZY_FETCH=1 -GAMO_ENABLE=0 -GREAD_MERGE=0
endif

include ../common.mk

ifeq ($(PROJECT),cache_lazy_fair)
$(DESTDIR)/$(PROJECT): $(VORTEX_HOME)/tests/unittest/cache_lazy_fair/stimulus.h
endif
