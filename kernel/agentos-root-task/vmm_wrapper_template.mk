# VMM wrapper Makefile — generated from vmm_wrapper_template.mk
# Runs from BUILD_DIR so vmm.mk's vpath rules resolve correctly.
# Variables @LIBVMM@, @SDDF@, @BOARD_DIR@ are substituted at generation time.

LIBVMM  := @LIBVMM@
SDDF    := @SDDF@
BOARD_DIR := @BOARD_DIR@

ARCH   := aarch64
TARGET := aarch64-none-elf
CC     := clang
LD     := ld.lld
AS     := llvm-as
AR     := $(shell command -v llvm-ar 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ar 2>/dev/null || command -v /usr/local/opt/llvm/bin/llvm-ar 2>/dev/null || command -v ar)
RANLIB := $(shell command -v llvm-ranlib 2>/dev/null || command -v /opt/homebrew/opt/llvm/bin/llvm-ranlib 2>/dev/null || command -v /usr/local/opt/llvm/bin/llvm-ranlib 2>/dev/null || command -v ranlib)

SDDF_CUSTOM_LIBC := 1

CFLAGS := \
    -mstrict-align \
    -ffreestanding \
    -g3 -O3 -Wall \
    -Wno-unused-function \
    -DBOARD_qemu_virt_aarch64 \
    -I$(BOARD_DIR)/include \
    -I$(LIBVMM)/include \
    -I$(SDDF)/include \
    -I$(SDDF)/include/microkit \
    -I$(SDDF)/include/sddf/util/custom_libc \
    -include @KERNEL_SRC_DIR@/include/sel4_debug_putchar_compat.h \
    -MD -MP \
    -target $(TARGET)

# vpath is the key: makes vmm.mk's "libvmm/%.o: src/%.c" find sources in $(LIBVMM)/src/
vpath %.c $(LIBVMM)

include $(LIBVMM)/vmm.mk
include $(SDDF)/util/util.mk

# smc.c calls seL4_ARM_SMC() which is a typedef (not a function) in Microkit SDK 2.1.
# Override the pattern rule with a stub that compiles cleanly.  On QEMU virt,
# PSCI is handled by QEMU's built-in emulation; no ARM_SMC_CAP is needed.
SMC_STUB := @KERNEL_SRC_DIR@/src/smc_stub.c
libvmm/arch/aarch64/smc.o: $(SMC_STUB)
	${CC} ${CFLAGS} -c -o $@ $<

.PHONY: vmm-libs
vmm-libs: libvmm.a libsddf_util_debug.a
