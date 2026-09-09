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

# -D__thread= is mandatory and must match linux_vmm.c / pd_entry.c (VMM_CFLAGS).
# libsel4 declares __sel4_ipc_buffer as extern __thread. Without this, libvmm.a
# (guest_start → seL4_TCB_WriteRegisters, 38 MRs) uses a TLS copy that stays
# NULL and VMFaults at address 0. linux_vmm's global is a different symbol.
CFLAGS := \
    -mstrict-align \
    -ffreestanding \
    -g3 -O3 -Wall \
    -Wno-unused-function \
    -DBOARD_qemu_virt_aarch64 \
    -D__thread= \
    -DAOS_NO_UBSAN=1 \
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

# libvmm.mk adds -fsanitize-trap=undefined (brk). In this PD that trap is a
# VCPUFault/UserException with no handler: the guest dies after the first
# emulated MMIO. Strip it after the include so CHECK_LIBVMM_CFLAGS rebuilds.
CFLAGS := $(filter-out -fsanitize=undefined -fsanitize-trap=undefined,$(CFLAGS))

# smc.c calls seL4_ARM_SMC() which is a typedef (not a function) in Microkit SDK 2.1.
# Override the pattern rule with a stub that compiles cleanly.  On QEMU virt,
# PSCI is handled by QEMU's built-in emulation; no ARM_SMC_CAP is needed.
SMC_STUB := @KERNEL_SRC_DIR@/src/smc_stub.c
libvmm/arch/aarch64/smc.o: $(SMC_STUB)
	${CC} ${CFLAGS} -c -o $@ $<

.PHONY: vmm-libs
vmm-libs: libvmm.a libsddf_util_debug.a
