ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

SRC_DIR := $(VORTEX_HOME)/tests/unittest/cache_lazy_fair
PROJECT := cache_lazy_fair

include ../cache_simx.mk

.PHONY: all run-simx run-rtl
all: $(PROJECT)

run-simx: $(PROJECT)
	./$(PROJECT)

run-rtl:
	$(MAKE) -C $(ROOT_DIR)/hw/unittest/cache run PROJECT=cache_lazy_fair
