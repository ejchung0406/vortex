ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

PROJECT := cache_nc_rsp
SRC_DIR := $(VORTEX_HOME)/tests/unittest/$(PROJECT)

include ../cache_simx.mk

.PHONY: run
run: $(PROJECT)
	./$(PROJECT) 0 0
	./$(PROJECT) 0 1
	./$(PROJECT) 2 0
	./$(PROJECT) 2 1
