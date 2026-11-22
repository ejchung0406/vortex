ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := bwstream

SRC_DIR := $(VORTEX_HOME)/tests/regression/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

VX_SRCS := $(SRC_DIR)/kernel.cpp

VX_EXTRA_KERNELS := kernel_timed

VX_HDRS := $(SRC_DIR)/stream.h $(SRC_DIR)/common.h

HDRS := $(SRC_DIR)/common.h

OPTS ?= -n64 -r1

KERNEL_LIB := vortex2

include ../common.mk
