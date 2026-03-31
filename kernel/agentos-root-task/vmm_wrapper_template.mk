# VMM wrapper Makefile — generated from vmm_wrapper_template.mk
# Runs from BUILD_DIR so vmm.mk's vpath rules resolve correctly.
# Variables @LIBVMM@, @SDDF@, @BOARD_DIR@, @MICROKIT_CONFIG@ are substituted at generation time.

LIBVMM  := @LIBVMM@
SDDF    := @SDDF@
BOARD_DIR := @BOARD_DIR@
MICROKIT_CONFIG := @MICROKIT_CONFIG@

ARCH   := aarch64
TARGET := aarch64-none-elf
CC     := clang
LD     := ld.lld
AS     := llvm-as
AR     := llvm-ar
RANLIB := llvm-ranlib

SDDF_CUSTOM_LIBC := 1

CFLAGS := \
    -mstrict-align \
    -ffreestanding \
    -g3 -O3 -Wall \
    -Wno-unused-function \
    -DMICROKIT_CONFIG_$(MICROKIT_CONFIG) \
    -DBOARD_qemu_virt_aarch64 \
    -I$(BOARD_DIR)/include \
    -I$(LIBVMM)/include \
    -I$(SDDF)/include \
    -I$(SDDF)/include/sddf/util/custom_libc \
    -I$(SDDF)/include/microkit \
    -MD -MP \
    -target $(TARGET)

# vpath is the key: makes vmm.mk's "libvmm/%.o: src/%.c" find sources in $(LIBVMM)/src/
vpath %.c $(LIBVMM)

include $(LIBVMM)/vmm.mk
include $(SDDF)/util/util.mk

.PHONY: vmm-libs
vmm-libs: libvmm.a libsddf_util_debug.a
