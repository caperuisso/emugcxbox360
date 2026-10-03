# Builds the headless test host with the Xbox 360 compiler as a Linux/PPC32
# program, to run the core's big-endian code under qemu-ppc:
#   make -f tests/ppc_linux/Makefile && qemu-ppc build_ppc/emugc_headless GAME FIELDS
# 32-bit only (qemu-ppc has no 64-bit CPU), so -mcpu=cell's 64-bit
# instructions are replaced by a G4 target; the code paths are the same.
PREFIX  := $(HOME)/xenon/bin/xenon-
CXX     := $(PREFIX)g++
CC      := $(PREFIX)gcc
OUT     := build_ppc
ARCH    := -m32 -mcpu=7450 -mno-powerpc64 -maltivec -mhard-float -fno-pic
CXXFLAGS := -O2 $(EXTRA) -DEMUGC_NO_THREADS -fno-strict-aliasing -std=gnu++17 $(ARCH) -Isrc -Ithird_party
CFLAGS   := -O2 $(EXTRA) $(ARCH) -Ithird_party

CORE := $(shell find src/core src/xenos -name '*.cpp')
OBJS := $(patsubst %.cpp,$(OUT)/%.o,$(CORE) tests/headless_main.cpp) \
        $(OUT)/third_party/zstd/zstddeclib.o $(OUT)/tests/ppc_linux/shim.o $(OUT)/tests/ppc_linux/start.o

$(OUT)/emugc_headless: $(OBJS)
	$(CXX) $(ARCH) -nostartfiles -static -Wl,-Ttext-segment=0x10000000 -L$(HOME)/xenon/xenon/lib/32 -L$(HOME)/xenon/lib/gcc/xenon/9.4.0/32 -o $@ $(OBJS) -lstdc++ -lm -lc -lgcc

$(OUT)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@$(CXX) -MMD -MP $(CXXFLAGS) -c $< -o $@
$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/%.o: %.S
	@mkdir -p $(dir $@)
	@$(CC) $(ARCH) -c $< -o $@

-include $(OBJS:.o=.d)

TEST_OBJS := $(filter-out $(OUT)/tests/headless_main.o,$(OBJS)) $(OUT)/tests/test_cpu.o
$(OUT)/emugc_tests: $(TEST_OBJS)
	$(CXX) $(ARCH) -nostartfiles -static -Wl,-Ttext-segment=0x10000000 -L$(HOME)/xenon/xenon/lib/32 -L$(HOME)/xenon/lib/gcc/xenon/9.4.0/32 -o $@ $(TEST_OBJS) -lstdc++ -lm -lc -lgcc
