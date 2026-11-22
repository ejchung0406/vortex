all: cvfpu softfloat ramulator cocogfx

cvfpu:

SOFTFLOAT_DIR := softfloat/build/Linux-x86_64-GCC
SOFTFLOAT_OPTS := -fPIC -DSOFTFLOAT_ROUND_ODD -DINLINE_LEVEL=5 -DSOFTFLOAT_FAST_DIV32TO16 -DSOFTFLOAT_FAST_DIV64TO32 -DTHREAD_LOCAL=_Thread_local

# softfloat's own makefile does not track SOFTFLOAT_OPTS, so an archive built
# with older options (e.g. non-TLS softfloat_exceptionFlags) is otherwise kept.
softfloat:
	@echo '$(SOFTFLOAT_OPTS)' | cmp -s - softfloat.opts || $(MAKE) -C $(SOFTFLOAT_DIR) clean
	SPECIALIZE_TYPE=RISCV SOFTFLOAT_OPTS="$(SOFTFLOAT_OPTS)" $(MAKE) -C $(SOFTFLOAT_DIR)
	@echo '$(SOFTFLOAT_OPTS)' > softfloat.opts

ramulator/libramulator.so:
	cd ramulator && mkdir -p build && cd build && cmake .. && make -j4

# The GDDR6 device model Ramulator ships but does not build. It registers with
# Ramulator's factory when the library loads, built as Ramulator builds itself.
# Ramulator's cmake fetches spdlog and yaml-cpp into ramulator/ext, so the
# model compiles after it; otherwise the host's spdlog/fmt headers are used.
RAMULATOR_EXT_DIR := ramulator_ext
RAMULATOR_EXT_INCS := -isystem ramulator/src -isystem ramulator/ext/spdlog/include -isystem ramulator/ext/yaml-cpp/include
RAMULATOR_EXT_CXXFLAGS := -std=c++20 -O3 -DNDEBUG -fPIC $(RAMULATOR_EXT_INCS)

$(RAMULATOR_EXT_DIR)/gddr6.o: ramulator/src/dram/impl/GDDR6.cpp | ramulator/libramulator.so
	mkdir -p $(RAMULATOR_EXT_DIR)
	$(CXX) $(RAMULATOR_EXT_CXXFLAGS) -c $< -o $@

# Ramulator's generic controller with its read and write buffer sizes as
# parameters (they are constants there), registered as "Windowed", making the
# same FRFCFS choices with less work (see the source). The copy and its
# shortcuts follow the pinned submodule's generic controller, FRFCFS, DRAM
# node, action and prerequisite lambdas and the HBM2/HBM3/GDDR6 devices: the
# build stops if any of them differs from the version recorded in
# upstream.sha256, so a submodule update is reconciled with the copy first.
$(RAMULATOR_EXT_DIR)/windowed_controller.o: ramulator_vortex/windowed_controller.cpp ramulator_vortex/upstream.sha256 | ramulator/libramulator.so
	@sha256sum --quiet -c ramulator_vortex/upstream.sha256 || { echo "ramulator_vortex: upstream Ramulator sources changed; reconcile windowed_controller.cpp, then update upstream.sha256"; exit 1; }
	mkdir -p $(RAMULATOR_EXT_DIR)
	$(CXX) $(RAMULATOR_EXT_CXXFLAGS) -c $< -o $@

# Written beside and renamed, so a simulator that has the library loaded keeps
# the one it started with.
$(RAMULATOR_EXT_DIR)/libramulator-vortex.so: $(RAMULATOR_EXT_DIR)/gddr6.o $(RAMULATOR_EXT_DIR)/windowed_controller.o ramulator/libramulator.so
	$(CXX) -shared $(filter %.o,$^) -Lramulator -lramulator -o $@.tmp
	mv $@.tmp $@

ramulator: ramulator/libramulator.so $(RAMULATOR_EXT_DIR)/libramulator-vortex.so

cocogfx/libcocogfx.a:
	$(MAKE) -C cocogfx
cocogfx: cocogfx/libcocogfx.a

clean:
	$(MAKE) -C $(SOFTFLOAT_DIR) clean
	rm -f softfloat.opts
	rm -rf ramulator/build ramulator/libramulator.so
	rm -f $(RAMULATOR_EXT_DIR)/*.o $(RAMULATOR_EXT_DIR)/*.so $(RAMULATOR_EXT_DIR)/*.cpp
	$(MAKE) -C cocogfx clean

.PHONY: all cvfpu softfloat ramulator cocogfx