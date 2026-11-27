SIMX_DIR := $(VORTEX_HOME)/sim/simx
VPATH := $(SRC_DIR):$(SIMX_DIR)/mem:$(SIMX_DIR)/amo:$(SW_COMMON_DIR)
OBJS := main.o cache.o amo_unit.o util.o

CXXFLAGS += -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Werror -pthread -MMD -MP
CXXFLAGS += -I$(ROOT_DIR)/sw -I$(ROOT_DIR)/hw -I$(SW_COMMON_DIR) -I$(SIM_COMMON_DIR)
CXXFLAGS += -I$(SIMX_DIR) -I$(SIMX_DIR)/mem -I$(SIMX_DIR)/amo
CXXFLAGS += -DVX_CFG_XLEN=$(XLEN) -DVX_CFG_XLEN_$(XLEN) $(CONFIGS)

$(PROJECT): $(OBJS)
	$(CXX) $^ -pthread -o $@

.PHONY: clean
clean:
	rm -f $(PROJECT) $(OBJS) $(OBJS:.o=.d)

-include $(OBJS:.o=.d)
