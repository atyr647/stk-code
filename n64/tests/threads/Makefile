# Threading-shim regression ROM. Needs N64_INST (libdragon preview) and the
# GCC source tree Pak's toolchain script leaves in $(N64_INST)/build.
BUILD_DIR = build
include $(N64_INST)/include/n64.mk

COMPAT   := ../../compat
GCC_SRC  ?= $(N64_INST)/build/gcc-16.2.0/libstdc++-v3
CXXFLAGS += -I$(COMPAT)/include -D_GLIBCXX_HAS_GTHREADS=1
CFLAGS   += -I$(COMPAT)/include
LDFLAGS  += --wrap=gettimeofday

all: threads.z64

$(BUILD_DIR)/libstdcxx_%.o: $(GCC_SRC)/src/c++11/%.cc
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -Wno-error -I$(GCC_SRC)/libsupc++ -o $@ $<
$(BUILD_DIR)/clock_libdragon.o: $(COMPAT)/src/clock_libdragon.cpp
	@mkdir -p $(dir $@)
	$(CXX) -c $(CXXFLAGS) -o $@ $<
$(BUILD_DIR)/gthr_libdragon.o: $(COMPAT)/src/gthr_libdragon.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/threads.elf: $(BUILD_DIR)/main.o $(BUILD_DIR)/gthr_libdragon.o $(BUILD_DIR)/clock_libdragon.o \
    $(BUILD_DIR)/libstdcxx_thread.o $(BUILD_DIR)/libstdcxx_condition_variable.o \
    $(BUILD_DIR)/libstdcxx_mutex.o
threads.z64: N64_ROM_TITLE = "STK THREADS"

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: all clean
