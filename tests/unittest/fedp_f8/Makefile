ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := fedp_f8

SRC_DIR := $(VORTEX_HOME)/tests/unittest/$(PROJECT)

SRCS := $(SRC_DIR)/main.cpp

# The model's shifts must stay defined: undefined behaviour fails the test.
CXXFLAGS += -fsanitize=undefined -fno-sanitize-recover=undefined

include ../common.mk
