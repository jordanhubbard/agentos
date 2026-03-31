#
# agentOS Linux VMM Build — Sub-Makefile
#
# Invoked from the main kernel Makefile when ARCH=aarch64.
# Uses the libvmm example pattern: generates a wrapper Makefile in BUILD_DIR,
# then runs from there so vmm.mk's vpath/pattern rules resolve correctly.
#
# Required variables (from parent Makefile):
#   BUILD_DIR, MICROKIT_SDK, MICROKIT_BOARD, MICROKIT_CONFIG, BOARD_DIR
#

# ─── Paths ────────────────────────────────────────────────────────────────
KERNEL_SRC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
AGENTOS_ROOT   := $(abspath $(KERNEL_SRC_DIR)/../..)
LIBVMM_ABS     := $(AGENTOS_ROOT)/libvmm
SDDF_ABS       := $(LIBVMM_ABS)/dep/sddf
DTC            := dtc

# Guest image identifiers (from libvmm examples/simple/simple.mk)
LINUX_IMAGE  := 85000f3f42a882e4476e57003d53f2bbec8262b0-linux
INITRD_IMAGE := 6dcd1debf64e6d69b178cd0f46b8c4ae7cebe2a5-rootfs.cpio.gz
IMAGES_URL   := https://trustworthy.systems/Downloads/libvmm/images

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
    -DMICROKIT_CONFIG_$(MICROKIT_CONFIG) \
    -DBOARD_qemu_virt_aarch64 \
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

# ─── Download guest images ────────────────────────────────────────────────
$(BUILD_DIR)/$(LINUX_IMAGE):
	@echo "[VMM] Downloading Linux kernel image..."
	@mkdir -p $(BUILD_DIR)
	curl -fSL $(IMAGES_URL)/$(LINUX_IMAGE).tar.gz -o $(BUILD_DIR)/$(LINUX_IMAGE).tar.gz
	mkdir -p $(BUILD_DIR)/linux_dl
	tar -xf $(BUILD_DIR)/$(LINUX_IMAGE).tar.gz -C $(BUILD_DIR)/linux_dl
	cp $(BUILD_DIR)/linux_dl/$(LINUX_IMAGE)/linux $(BUILD_DIR)/$(LINUX_IMAGE)
	rm -rf $(BUILD_DIR)/linux_dl $(BUILD_DIR)/$(LINUX_IMAGE).tar.gz

$(BUILD_DIR)/$(INITRD_IMAGE):
	@echo "[VMM] Downloading initrd..."
	@mkdir -p $(BUILD_DIR)
	curl -fSL $(IMAGES_URL)/$(INITRD_IMAGE).tar.gz -o $(BUILD_DIR)/$(INITRD_IMAGE).tar.gz
	mkdir -p $(BUILD_DIR)/initrd_dl
	tar -xf $(BUILD_DIR)/$(INITRD_IMAGE).tar.gz -C $(BUILD_DIR)/initrd_dl
	cp $(BUILD_DIR)/initrd_dl/$(INITRD_IMAGE)/rootfs.cpio.gz $(BUILD_DIR)/$(INITRD_IMAGE)
	rm -rf $(BUILD_DIR)/initrd_dl $(BUILD_DIR)/$(INITRD_IMAGE).tar.gz

# ─── Device tree ──────────────────────────────────────────────────────────
$(BUILD_DIR)/vm.dts: $(DTS_DIR)/linux.dts $(DTS_DIR)/overlay.dts
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
		-e 's|@MICROKIT_CONFIG@|$(MICROKIT_CONFIG)|g' \
		$< > $@
	@echo "[VMM] Generated wrapper Makefile ✓"

# ─── Build libvmm.a + libsddf_util_debug.a ───────────────────────────────
# Run make FROM BUILD_DIR so vmm.mk's relative paths work
$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a: $(BUILD_DIR)/vmm_wrapper.mk FORCE
	@echo "[VMM] Building libvmm.a and libsddf_util_debug.a (from $(BUILD_DIR))..."
	$(MAKE) -C $(BUILD_DIR) -f vmm_wrapper.mk vmm-libs
	@echo "[VMM] Libraries built ✓"

# ─── Package guest images ─────────────────────────────────────────────────
$(BUILD_DIR)/images.o: $(PKG_IMG) \
                       $(BUILD_DIR)/$(LINUX_IMAGE) \
                       $(BUILD_DIR)/$(INITRD_IMAGE) \
                       $(BUILD_DIR)/vm.dtb
	@echo "[VMM] Packaging guest images..."
	clang -c -g3 -x assembler-with-cpp \
		-DGUEST_KERNEL_IMAGE_PATH=\"$(BUILD_DIR)/$(LINUX_IMAGE)\" \
		-DGUEST_DTB_IMAGE_PATH=\"$(BUILD_DIR)/vm.dtb\" \
		-DGUEST_INITRD_IMAGE_PATH=\"$(BUILD_DIR)/$(INITRD_IMAGE)\" \
		-target aarch64-none-elf \
		$(PKG_IMG) -o $@

# ─── Compile linux_vmm.c ─────────────────────────────────────────────────
$(BUILD_DIR)/linux_vmm.o: $(KERNEL_SRC_DIR)/src/linux_vmm.c
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling linux_vmm.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

# ─── Link linux_vmm.elf ──────────────────────────────────────────────────
$(BUILD_DIR)/linux_vmm.elf: $(BUILD_DIR)/linux_vmm.o \
                             $(BUILD_DIR)/images.o \
                             $(BUILD_DIR)/libvmm.a \
                             $(BUILD_DIR)/libsddf_util_debug.a
	@echo "[VMM] Linking linux_vmm.elf..."
	ld.lld -L$(BOARD_DIR)/lib \
		$(BUILD_DIR)/linux_vmm.o $(BUILD_DIR)/images.o \
		--start-group -lmicrokit -Tmicrokit.ld \
		$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a \
		--end-group \
		-o $@
	@echo "[VMM] linux_vmm.elf ✓"

vmm-clean:
	rm -f $(BUILD_DIR)/linux_vmm.o $(BUILD_DIR)/linux_vmm.elf
	rm -f $(BUILD_DIR)/images.o $(BUILD_DIR)/vm.dts $(BUILD_DIR)/vm.dtb
	rm -f $(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a
	rm -f $(BUILD_DIR)/vmm_wrapper.mk

FORCE:
