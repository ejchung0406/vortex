ROOT_DIR := $(realpath ../../..)
include $(ROOT_DIR)/config.mk

SIMX_DIR := $(VORTEX_HOME)/sim/simx
SRCS := $(VORTEX_HOME)/tests/unittest/dcache_client_slots/main.cpp $(SW_COMMON_DIR)/util.cpp
CXXFLAGS += -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread
CXXFLAGS += -I$(ROOT_DIR)/sw -I$(ROOT_DIR)/hw -I$(SW_COMMON_DIR) -I$(SIM_COMMON_DIR) -I$(SIMX_DIR) -I$(SIMX_DIR)/mem
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN) $(CONFIGS)
CXXFLAGS += -DVX_CFG_NUM_CORES=1 -DVX_CFG_SOCKET_SIZE=1 -DVX_CFG_NUM_DCACHES=1 -DVX_CFG_NUM_THREADS=4
CXXFLAGS += -DVX_CFG_DCACHE_READ_MERGE=1 -DVX_CFG_L1_LINK_LATENCY=0 -DVX_CFG_DCACHE_CHANNELS=2

.PHONY: all run clean
all: coalescer adapter

coalescer: CXXFLAGS += -DVX_CFG_NUM_LSU_LANES=4 '-DVX_CFG_DCACHE_WORD_SIZE=($(XLEN)/4)'
coalescer: $(SIMX_DIR)/mem/mem_coalescer.cpp
adapter: CXXFLAGS += -DVX_CFG_NUM_LSU_LANES=2 '-DVX_CFG_DCACHE_WORD_SIZE=($(XLEN)/8)'
adapter: $(SIMX_DIR)/mem/lsu_mem_adapter.cpp

coalescer adapter: $(SRCS)
	$(CXX) $(CXXFLAGS) -MM -MP -MT $@ $(filter %.cpp,$^) > $@.d
	$(CXX) $(CXXFLAGS) $(filter %.cpp,$^) -o $@

run: all
	./coalescer
	./adapter

clean:
	rm -f coalescer adapter coalescer.d adapter.d

-include coalescer.d adapter.d
