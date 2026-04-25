#
# agentOS Linux VMM Build — Sub-Makefile
#
# Invoked from the main kernel Makefile when ARCH=aarch64.
# Uses the libvmm example pattern: generates a wrapper Makefile in BUILD_DIR,
# then runs from there so vmm.mk's vpath/pattern rules resolve correctly.
#
# Required variables (from parent Makefile):
#   BUILD_DIR, AGENTOS_ARCH, AGENTOS_BOARD
#

# ─── Paths ────────────────────────────────────────────────────────────────
KERNEL_SRC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AGENTOS_ROOT   := $(abspath $(KERNEL_SRC_DIR)/../..)
LIBVMM_ABS     := $(AGENTOS_ROOT)/libvmm
SDDF_ABS       := $(LIBVMM_ABS)/dep/sddf
DTC            := dtc

# BOARD_DIR: seL4 SDK board package containing include/ and lib/.
# Default matches the SDK bundled in the repo; override when invoking vmm.mk
# directly with a different SDK installation.
BOARD_DIR ?= $(AGENTOS_ROOT)/microkit-sdk-2.1.0/board/$(AGENTOS_BOARD)/debug

# Guest OS selection: buildroot (default) or ubuntu
GUEST_OS ?= buildroot

# Buildroot guest: download libvmm example images (kernel + initrd)
BUILDROOT_LINUX_IMAGE  := 85000f3f42a882e4476e57003d53f2bbec8262b0-linux
BUILDROOT_INITRD_IMAGE := 6dcd1debf64e6d69b178cd0f46b8c4ae7cebe2a5-rootfs.cpio.gz
IMAGES_URL             := https://trustworthy.systems/Downloads/libvmm/images

# Ubuntu guest: pre-extracted kernel from ubuntu-24.04-arm64.raw
UBUNTU_KERNEL := $(AGENTOS_ROOT)/guest-images/ubuntu-kernel-6.8.0-Image
UBUNTU_EMPTY_INITRD := $(BUILD_DIR)/ubuntu-empty-initrd.cpio.gz

ifeq ($(GUEST_OS),ubuntu)
LINUX_IMAGE  := $(UBUNTU_KERNEL)
INITRD_IMAGE := $(UBUNTU_EMPTY_INITRD)
DTS_OVERLAY  := ubuntu-overlay.dts
else
LINUX_IMAGE  := $(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE)
INITRD_IMAGE := $(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE)
DTS_OVERLAY  := overlay.dts
endif

# DTS + tools
DTS_DIR := $(LIBVMM_ABS)/examples/simple/board/qemu_virt_aarch64
DTSCAT  := $(LIBVMM_ABS)/tools/dtscat
PKG_IMG := $(LIBVMM_ABS)/tools/package_guest_images.S

# ─── VMM CFLAGS (used for linux_vmm.c compilation) ───────────────────────
VMM_CFLAGS := \
    -mstrict-align \
    -ffreestanding \
    -g3 -O3 -Wall \
    -Wno-unused-function \
    -DARCH_AARCH64 \
    -DBOARD_qemu_virt_aarch64 \
    -D__thread= \
    -I$(BOARD_DIR)/include \
    -I$(LIBVMM_ABS)/include \
    -I$(SDDF_ABS)/include \
    -I$(SDDF_ABS)/include/sddf/util/custom_libc \
    -I$(SDDF_ABS)/include/microkit \
    -I$(KERNEL_SRC_DIR)/include \
    -MD -MP \
    -target aarch64-none-elf

.PHONY: vmm-all vmm-clean FORCE

vmm-all: $(BUILD_DIR)/linux_vmm.elf

# ─── Ubuntu kernel: fetch via xtask (downloads .deb, extracts Image) ─────
ifeq ($(GUEST_OS),ubuntu)
$(UBUNTU_KERNEL):
	@echo "[VMM] Fetching Ubuntu kernel binary (via xtask fetch-guest)..."
	cargo xtask fetch-guest --os ubuntu
endif

# ─── Download buildroot guest images ─────────────────────────────────────
ifneq ($(GUEST_OS),ubuntu)
$(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE):
	@echo "[VMM] Downloading Linux kernel image..."
	@mkdir -p $(BUILD_DIR)
	curl -fSL $(IMAGES_URL)/$(BUILDROOT_LINUX_IMAGE).tar.gz -o $(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE).tar.gz
	mkdir -p $(BUILD_DIR)/linux_dl
	tar -xf $(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE).tar.gz -C $(BUILD_DIR)/linux_dl
	cp $(BUILD_DIR)/linux_dl/$(BUILDROOT_LINUX_IMAGE)/linux $(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE)
	rm -rf $(BUILD_DIR)/linux_dl $(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE).tar.gz

$(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE):
	@echo "[VMM] Downloading initrd..."
	@mkdir -p $(BUILD_DIR)
	curl -fSL $(IMAGES_URL)/$(BUILDROOT_INITRD_IMAGE).tar.gz -o $(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE).tar.gz
	mkdir -p $(BUILD_DIR)/initrd_dl
	tar -xf $(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE).tar.gz -C $(BUILD_DIR)/initrd_dl
	cp $(BUILD_DIR)/initrd_dl/$(BUILDROOT_INITRD_IMAGE)/rootfs.cpio.gz $(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE)
	rm -rf $(BUILD_DIR)/initrd_dl $(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE).tar.gz
endif

# ─── Ubuntu guest: empty initrd (Ubuntu uses initrdless boot via PARTUUID) ─
# A zero-byte gzip stream satisfies package_guest_images.S (non-empty file
# required) but is ignored by the Ubuntu kernel since ubuntu-overlay.dts
# has no linux,initrd-start / linux,initrd-end entries in /chosen.
$(UBUNTU_EMPTY_INITRD):
	@echo "[VMM] Creating empty initrd for Ubuntu initrdless boot..."
	@mkdir -p $(BUILD_DIR)
	printf '' | gzip > $@

# ─── Device tree ──────────────────────────────────────────────────────────
$(BUILD_DIR)/vm.dts: $(DTS_DIR)/linux.dts $(DTS_DIR)/$(DTS_OVERLAY)
	@mkdir -p $(BUILD_DIR)
	$(DTSCAT) $^ > $@

$(BUILD_DIR)/vm.dtb: $(BUILD_DIR)/vm.dts
	$(DTC) -q -I dts -O dtb $< > $@

# ─── Generate wrapper Makefile in BUILD_DIR ───────────────────────────────
# vmm.mk uses vpath and is designed to be included, not invoked via -f.
# We generate a wrapper Makefile in BUILD_DIR and run make from there.
$(BUILD_DIR)/vmm_wrapper.mk: $(KERNEL_SRC_DIR)/vmm_wrapper_template.mk
	@mkdir -p $(BUILD_DIR)
	sed \
		-e 's|@LIBVMM@|$(LIBVMM_ABS)|g' \
		-e 's|@SDDF@|$(SDDF_ABS)|g' \
		-e 's|@BOARD_DIR@|$(BOARD_DIR)|g' \
		-e 's|@KERNEL_SRC_DIR@|$(KERNEL_SRC_DIR)|g' \
		$< > $@
	@echo "[VMM] Generated wrapper Makefile ✓"

# ─── Build libvmm.a + libsddf_util_debug.a ───────────────────────────────
# Run make FROM BUILD_DIR so vmm.mk's relative paths work
$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a: $(BUILD_DIR)/vmm_wrapper.mk
	@echo "[VMM] Building libvmm.a and libsddf_util_debug.a (from $(BUILD_DIR))..."
	$(MAKE) -C $(BUILD_DIR) -f vmm_wrapper.mk vmm-libs
	@echo "[VMM] Libraries built ✓"

# ─── Package guest images ─────────────────────────────────────────────────
$(BUILD_DIR)/images.o: $(PKG_IMG) \
                       $(LINUX_IMAGE) \
                       $(INITRD_IMAGE) \
                       $(BUILD_DIR)/vm.dtb
	@echo "[VMM] Packaging guest images (GUEST_OS=$(GUEST_OS))..."
	clang -c -g3 -x assembler-with-cpp \
		-DGUEST_KERNEL_IMAGE_PATH=\"$(LINUX_IMAGE)\" \
		-DGUEST_DTB_IMAGE_PATH=\"$(BUILD_DIR)/vm.dtb\" \
		-DGUEST_INITRD_IMAGE_PATH=\"$(INITRD_IMAGE)\" \
		-target aarch64-none-elf \
		$(PKG_IMG) -o $@

# ─── Compile linux_vmm.c + gpu_shmem.c ──────────────────────────────────
$(BUILD_DIR)/linux_vmm.o: $(KERNEL_SRC_DIR)/src/linux_vmm.c
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling linux_vmm.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(BUILD_DIR)/gpu_shmem.o: $(KERNEL_SRC_DIR)/src/gpu_shmem.c
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling gpu_shmem.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

# ─── Link linux_vmm.elf ──────────────────────────────────────────────────
$(BUILD_DIR)/linux_vmm.elf: $(BUILD_DIR)/linux_vmm.o \
                             $(BUILD_DIR)/gpu_shmem.o \
                             $(BUILD_DIR)/images.o \
                             $(BUILD_DIR)/libvmm.a \
                             $(BUILD_DIR)/libsddf_util_debug.a
	@echo "[VMM] Linking linux_vmm.elf..."
	ld.lld -T$(BOARD_DIR)/lib/microkit.ld \
		-L$(BOARD_DIR)/lib \
		$(BUILD_DIR)/linux_vmm.o $(BUILD_DIR)/gpu_shmem.o $(BUILD_DIR)/images.o \
		--start-group \
		$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a \
		--end-group \
		-o $@
	@echo "[VMM] linux_vmm.elf ✓"

vmm-clean:
	rm -f $(BUILD_DIR)/linux_vmm.o $(BUILD_DIR)/gpu_shmem.o $(BUILD_DIR)/linux_vmm.elf
	rm -f $(BUILD_DIR)/images.o $(BUILD_DIR)/vm.dts $(BUILD_DIR)/vm.dtb
	rm -f $(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a
	rm -f $(BUILD_DIR)/vmm_wrapper.mk

FORCE:
