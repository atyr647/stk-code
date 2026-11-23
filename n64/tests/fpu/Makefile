# Regression ROM for the FPU completion handler (n64/compat/src/fpu_emu.c).
# Needs N64_INST (libdragon preview).
BUILD_DIR = build
include $(N64_INST)/include/n64.mk

COMPAT := ../../compat
# IEEE semantics, as the STK build uses (n64.mk adds -ffast-math, after
# CFLAGS)
N64_C_AND_CXX_FLAGS := $(filter-out -ffast-math,$(N64_C_AND_CXX_FLAGS))

all: fpu.z64

FIXED := -DSTK_N64_FPU_EMU_FIXED_REGS $(foreach r,20 21 22 23 24 25 26 27 28 29 30 31,'-ffixed-$$f$(r)')
$(BUILD_DIR)/fpu_emu.o: $(COMPAT)/src/fpu_emu.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $(FIXED) -o $@ $<
$(BUILD_DIR)/fpu_setup.o: fpu_setup.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD_DIR)/fpu.elf: $(BUILD_DIR)/main.o $(BUILD_DIR)/fpu_emu.o $(BUILD_DIR)/fpu_setup.o
fpu.z64: N64_ROM_TITLE = "STK FPU"

clean:
	rm -rf $(BUILD_DIR) *.z64
.PHONY: all clean
