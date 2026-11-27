ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := cache_queues
SRC_DIR := $(VORTEX_HOME)/tests/unittest/$(PROJECT)

include ../cache_simx.mk

.PHONY: run
run: $(PROJECT)
	./$(PROJECT)
