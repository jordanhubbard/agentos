# agentOS FreeBSD VMM PD — Makefile
#
# Builds freebsd_vmm.elf: the VMM protection domain that hosts FreeBSD
# as a raw seL4 virtual machine guest using libvmm.
#
# Prerequisites:
#   - SEL4_BOARD_DIR pointing to the board package with seL4 headers
#     (e.g. microkit-sdk-2.1.0/board/qemu_virt_aarch64; we use its seL4
#     headers and kernel API stubs — NOT libmicrokit)
#   - LIBVMM_DIR pointing to the libvmm checkout
#   - EDK2_FD pointing to the edk2-aarch64-code.fd firmware image
#   - FREEBSD_DTB pointing to the compiled freebsd-guest.dtb (optional)
#   - clang + ld.lld + llvm-objcopy (LLVM cross toolchain)

BOARD           ?= qemu_virt_aarch64
AGENTOS_ROOT    := $(abspath ../..)
SEL4_BOARD_DIR  ?= $(AGENTOS_ROOT)/microkit-sdk-2.1.0/board/$(BOARD)/release
LIBVMM_DIR      ?= $(AGENTOS_ROOT)/libvmm
BUILD_DIR       ?= build
LIBVMM_LIB_DIR  ?= $(AGENTOS_ROOT)/build/$(BOARD)
EDK2_FD         ?= $(AGENTOS_ROOT)/guest-images/edk2-aarch64-code.fd
FREEBSD_DTB     ?= $(BUILD_DIR)/freebsd-guest.dtb

SEL4_INCLUDE    := $(SEL4_BOARD_DIR)/include
SEL4_LIB        := $(SEL4_BOARD_DIR)/lib
LIBVMM_INCLUDE  := $(LIBVMM_DIR)/include
LIBVMM_SRC      := $(LIBVMM_DIR)/src
SDDF_DIR        := $(LIBVMM_DIR)/dep/sddf

CC      := clang --target=aarch64-none-elf
LD      := ld.lld
OBJCOPY := llvm-objcopy

AGENTOS_INCLUDE := ../agentos-root-task/include

CFLAGS := \
    -mstrict-align \
    -ffreestanding \
    -nostdlib \
    -O2 \
    -Wall -Wextra \
    -I$(SEL4_INCLUDE) \
    -I$(LIBVMM_INCLUDE) \
    -I$(SDDF_DIR)/include \
    -I$(SDDF_DIR)/include/sddf/util/custom_libc \
    -I$(SDDF_DIR)/include/microkit \
    -I$(AGENTOS_INCLUDE) \
    -I.

LDFLAGS := \
    -L$(SEL4_LIB) \
    -L$(LIBVMM_LIB_DIR) \
    -Ttext-segment=0x800000 \
    --entry=freebsd_vmm_main

# seL4 API calls are inline (from sel4/sel4.h); no libmicrokit needed.
# libc builtins (memcpy/memset) are provided by libvmm or compiler intrinsics.
LIBS := -lvmm -lsddf_util_debug

SRCS := vmm.c vmm_mux.c

OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))

# Embedded binary blobs
EDK2_OBJ    := $(BUILD_DIR)/edk2_aarch64.o
DTB_OBJ     := $(BUILD_DIR)/freebsd_dtb.o

.PHONY: all clean dtb fetch-guest

# fetch-guest: download FreeBSD 14 AArch64 disk image + EDK2 UEFI firmware
fetch-guest:
	@echo "[freebsd-vmm] Fetching FreeBSD guest images..."
	@../../scripts/fetch-freebsd-guest.sh

all: $(BUILD_DIR)/freebsd_vmm.elf

# Provide a helpful error when EDK2 firmware is missing
$(EDK2_FD):
	@echo "[freebsd-vmm] EDK2 firmware not found at $(EDK2_FD)."
	@echo "[freebsd-vmm] Run: make -C kernel/freebsd-vmm fetch-guest"
	@exit 1

# Compile VMM source
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Embed EDK2 firmware as a binary blob
$(EDK2_OBJ): $(EDK2_FD) | $(BUILD_DIR)
	$(OBJCOPY) \
	    -I binary \
	    -O elf64-littleaarch64 \
	    -B aarch64 \
	    --rename-section .data=.rodata,CONTENTS,ALLOC,LOAD,READONLY,DATA \
	    $< $@

# Compile DTS → DTB (requires dtc)
$(FREEBSD_DTB): freebsd-vmm.dts | $(BUILD_DIR)
	dtc -I dts -O dtb -o $@ $<

# Embed DTB as a binary blob
$(DTB_OBJ): $(FREEBSD_DTB) | $(BUILD_DIR)
	$(OBJCOPY) \
	    -I binary \
	    -O elf64-littleaarch64 \
	    -B aarch64 \
	    --rename-section .data=.rodata,CONTENTS,ALLOC,LOAD,READONLY,DATA \
	    $< $@

# Link final VMM ELF
$(BUILD_DIR)/freebsd_vmm.elf: $(OBJS) $(EDK2_OBJ) $(DTB_OBJ)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR):
	mkdir -p $@

dtb: $(FREEBSD_DTB)

clean:
	rm -rf $(BUILD_DIR)
