ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

SRC_DIR := $(VORTEX_HOME)/tests/unittest/cache_lazy_fair
SIMX_DIR := $(VORTEX_HOME)/sim/simx
VPATH := $(SRC_DIR):$(SIMX_DIR)/mem:$(SIMX_DIR)/amo:$(SW_COMMON_DIR)
OBJS := main.o cache.o amo_unit.o util.o

CXXFLAGS += -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread -MMD -MP
CXXFLAGS += -I$(ROOT_DIR)/sw -I$(ROOT_DIR)/hw -I$(SW_COMMON_DIR) -I$(SIM_COMMON_DIR)
CXXFLAGS += -I$(SIMX_DIR) -I$(SIMX_DIR)/mem -I$(SIMX_DIR)/amo
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN) $(CONFIGS)

.PHONY: all run-simx run-rtl clean
all: cache_lazy_fair

cache_lazy_fair: $(OBJS)
	$(CXX) $^ -pthread -o $@

run-simx: cache_lazy_fair
	./cache_lazy_fair

run-rtl:
	$(MAKE) -C $(ROOT_DIR)/hw/unittest/cache run PROJECT=cache_lazy_fair

clean:
	rm -f cache_lazy_fair $(OBJS) $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
