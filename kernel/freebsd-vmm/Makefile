# agentOS FreeBSD VMM PD — Makefile
#
# Builds freebsd_vmm.elf: the VMM protection domain that hosts FreeBSD
# as a seL4 Microkit virtual machine guest using libvmm.
#
# Prerequisites:
#   - MICROKIT_SDK pointing to the Microkit SDK (aarch64 linux variant)
#   - LIBVMM_DIR pointing to libs/libvmm checkout
#   - EDK2_FD pointing to the edk2-aarch64-code.fd firmware image
#   - FREEBSD_DTB pointing to the compiled freebsd-guest.dtb (optional)
#   - aarch64-linux-gnu-gcc (cross compiler)

BOARD        ?= qemu_virt_aarch64
MICROKIT_SDK ?= ../../microkit-sdk-2.1.0
LIBVMM_DIR   ?= ../../libs/libvmm
BUILD_DIR    ?= build
EDK2_FD      ?= ../../guest-images/edk2-aarch64-code.fd
FREEBSD_DTB  ?= $(BUILD_DIR)/freebsd-guest.dtb

MICROKIT_INCLUDE := $(MICROKIT_SDK)/board/$(BOARD)/include
MICROKIT_LIB     := $(MICROKIT_SDK)/board/$(BOARD)/lib
LIBVMM_INCLUDE   := $(LIBVMM_DIR)/include
LIBVMM_SRC       := $(LIBVMM_DIR)/src

CC      := aarch64-linux-gnu-gcc
LD      := aarch64-linux-gnu-ld
OBJCOPY := aarch64-linux-gnu-objcopy

CFLAGS := \
    -target aarch64-none-elf \
    -mstrict-align \
    -ffreestanding \
    -nostdlib \
    -O2 \
    -Wall -Wextra \
    -I$(MICROKIT_INCLUDE) \
    -I$(LIBVMM_INCLUDE) \
    -I.

LDFLAGS := \
    -L$(MICROKIT_LIB) \
    -L$(LIBVMM_DIR)/build/$(BOARD) \
    -Ttext-segment=0x800000 \
    --entry=microkit_main

LIBS := -lmicrokit -lvmm

SRCS := vmm.c

OBJS := $(addprefix $(BUILD_DIR)/, $(SRCS:.c=.o))

# Embedded binary blobs
EDK2_OBJ    := $(BUILD_DIR)/edk2_aarch64.o
DTB_OBJ     := $(BUILD_DIR)/freebsd_dtb.o

.PHONY: all clean dtb

all: $(BUILD_DIR)/freebsd_vmm.elf

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

# Embed DTB as a binary blob (stub if DTB not available)
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
