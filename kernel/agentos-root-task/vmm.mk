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
SEL4_PROFILE ?= release
BOARD_DIR ?= $(AGENTOS_ROOT)/microkit-sdk-2.1.0/board/$(AGENTOS_BOARD)/$(SEL4_PROFILE)

# Guest OS selection: buildroot (default) or ubuntu
GUEST_OS ?= buildroot
VMM_DUAL_GUEST ?= 0
AGENTOS_IMAGES ?= $(AGENTOS_ROOT)/build/guest-images

# Buildroot guest: download libvmm example images (kernel + initrd)
BUILDROOT_LINUX_IMAGE  := 85000f3f42a882e4476e57003d53f2bbec8262b0-linux
BUILDROOT_INITRD_IMAGE := 6dcd1debf64e6d69b178cd0f46b8c4ae7cebe2a5-rootfs.cpio.gz
IMAGES_URL             := https://trustworthy.systems/Downloads/libvmm/images

# Ubuntu guest: local Ubuntu 26.04 live ISO assets staged by xtask fetch-guest.
UBUNTU_KERNEL := $(AGENTOS_IMAGES)/ubuntu-26.04-aarch64-Image
UBUNTU_INITRD := $(AGENTOS_IMAGES)/ubuntu-26.04-aarch64-initrd
UBUNTU_DTS_OVERLAY := $(BUILD_DIR)/ubuntu-26.04-overlay.dts
ifeq ($(VMM_DUAL_GUEST),1)
UBUNTU_RAM_BASE := 0xc0000000
UBUNTU_RAM_NODE := c0000000
UBUNTU_INITRD_START := 0xd0000000
else
UBUNTU_RAM_BASE := 0x40000000
UBUNTU_RAM_NODE := 40000000
UBUNTU_INITRD_START := 0x50000000
endif
UBUNTU_BOOTARGS := earlycon=pl011,0x9000000 console=ttyAMA0,115200n8 rdinit=/init panic=-1

ifeq ($(GUEST_OS),ubuntu)
LINUX_IMAGE  := $(UBUNTU_KERNEL)
INITRD_IMAGE := $(UBUNTU_INITRD)
DTS_OVERLAY_FILE := $(UBUNTU_DTS_OVERLAY)
else
LINUX_IMAGE  := $(BUILD_DIR)/$(BUILDROOT_LINUX_IMAGE)
INITRD_IMAGE := $(BUILD_DIR)/$(BUILDROOT_INITRD_IMAGE)
DTS_OVERLAY_FILE := $(BUILD_DIR)/buildroot-overlay.dts
BUILDROOT_INITRD_START := 0x50000000
endif

# DTS + tools
DTS_DIR := $(LIBVMM_ABS)/examples/simple/board/qemu_virt_aarch64
LINUX_DTS_BASE := $(DTS_DIR)/linux.dts
ifeq ($(VMM_DUAL_GUEST),1)
LINUX_DTS_BASE := $(BUILD_DIR)/linux-dual.dts
$(LINUX_DTS_BASE): $(DTS_DIR)/linux.dts $(VMM_CONFIG_STAMP) $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Generating dual-guest Linux base device tree..."
	sed \
		-e 's|memory@40000000|memory@$(UBUNTU_RAM_NODE)|g' \
		-e 's|0x00 0x40000000 0x00 0x80000000|0x00 $(UBUNTU_RAM_BASE) 0x00 0x20000000|g' \
		$< > $@
endif
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
    -I$(AGENTOS_ROOT)/platform/include \
    -MD -MP \
    -target aarch64-none-elf

ifeq ($(VMM_DUAL_GUEST),1)
VMM_CFLAGS += -DAGENTOS_GUEST_BOTH=1
endif

VMM_CONFIG_STAMP := $(BUILD_DIR)/vmm-$(GUEST_OS).stamp

$(VMM_CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@tmp="$@.tmp"; \
	printf 'GUEST_OS=%s\nVMM_DUAL_GUEST=%s\nSEL4_PROFILE=%s\n' \
		'$(GUEST_OS)' '$(VMM_DUAL_GUEST)' '$(SEL4_PROFILE)' > "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv "$$tmp" "$@"; fi

.PHONY: vmm-all vmm-clean FORCE

ifeq ($(GUEST_OS),freebsd)
vmm-all: $(BUILD_DIR)/freebsd_vmm.elf
else
vmm-all: $(BUILD_DIR)/linux_vmm.elf
endif

# ─── Ubuntu kernel/initrd: stage local ISO and extract boot assets ────────
ifeq ($(GUEST_OS),ubuntu)
$(UBUNTU_KERNEL) $(UBUNTU_INITRD):
	@echo "[VMM] Fetching Ubuntu 26.04 boot assets (via xtask fetch-guest)..."
	cargo xtask fetch-guest --os ubuntu --output-dir $(AGENTOS_IMAGES)
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

# ─── Device tree ──────────────────────────────────────────────────────────
$(UBUNTU_DTS_OVERLAY): $(KERNEL_SRC_DIR)/ubuntu-iso-overlay.dts.in $(KERNEL_SRC_DIR)/vmm.mk $(UBUNTU_INITRD) $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Generating Ubuntu 26.04 live-ISO overlay..."
	@initrd_size=$$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$(UBUNTU_INITRD)"); \
	start=$$(( $(UBUNTU_INITRD_START) )); \
	end=$$(( start + initrd_size )); \
	end_hex=$$(printf "0x%08x" $$end); \
	sed \
		-e 's|@UBUNTU_BOOTARGS@|$(UBUNTU_BOOTARGS)|g' \
		-e 's|@UBUNTU_RAM_NODE@|$(UBUNTU_RAM_NODE)|g' \
		-e 's|@UBUNTU_RAM_BASE@|0x00 $(UBUNTU_RAM_BASE)|g' \
		-e 's|@UBUNTU_INITRD_START@|0x00 $(UBUNTU_INITRD_START)|g' \
		-e "s|@UBUNTU_INITRD_END@|0x00 $$end_hex|g" \
		$< > $@

$(BUILD_DIR)/buildroot-overlay.dts: $(DTS_DIR)/overlay.dts $(KERNEL_SRC_DIR)/vmm.mk $(INITRD_IMAGE) $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Generating buildroot overlay (initrd at $(BUILDROOT_INITRD_START))..."
	@initrd_size=$$(python3 -c 'import os,sys; print(os.path.getsize(sys.argv[1]))' "$(INITRD_IMAGE)"); \
	start=$$(( $(BUILDROOT_INITRD_START) )); \
	end=$$(( start + initrd_size )); \
	end_hex=$$(printf "0x%08x" $$end); \
	sed \
		-e 's|@BUILDROOT_INITRD_START@|0x00 $(BUILDROOT_INITRD_START)|g' \
		-e "s|@BUILDROOT_INITRD_END@|0x00 $$end_hex|g" \
		$< > $@

$(BUILD_DIR)/vm.dts: FORCE $(LINUX_DTS_BASE) $(DTS_OVERLAY_FILE)
	@mkdir -p $(BUILD_DIR)
	$(DTSCAT) $(filter-out FORCE,$^) > $@

$(BUILD_DIR)/vm.dtb: FORCE $(BUILD_DIR)/vm.dts
	$(DTC) -q -I dts -O dtb $(filter-out FORCE,$^) > $@

# ─── Generate wrapper Makefile in BUILD_DIR ───────────────────────────────
# vmm.mk uses vpath and is designed to be included, not invoked via -f.
# We generate a wrapper Makefile in BUILD_DIR and run make from there.
$(BUILD_DIR)/vmm_wrapper.mk: $(KERNEL_SRC_DIR)/vmm_wrapper_template.mk $(VMM_CONFIG_STAMP) $(lastword $(MAKEFILE_LIST))
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
LIBVMM_INPUTS := $(shell find $(LIBVMM_ABS)/src $(LIBVMM_ABS)/include \
	$(LIBVMM_ABS)/dep/sddf/include $(LIBVMM_ABS)/vmm.mk \
	$(KERNEL_SRC_DIR)/vmm_wrapper_template.mk \
	$(KERNEL_SRC_DIR)/include/sel4_debug_putchar_compat.h \
	-type f 2>/dev/null)

$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a: $(BUILD_DIR)/vmm_wrapper.mk $(LIBVMM_INPUTS)
	@echo "[VMM] Building libvmm.a and libsddf_util_debug.a (from $(BUILD_DIR))..."
	$(MAKE) -C $(BUILD_DIR) -f vmm_wrapper.mk vmm-libs
	@echo "[VMM] Libraries built ✓"

# ─── Package guest images ─────────────────────────────────────────────────
$(BUILD_DIR)/images.o: FORCE \
                       $(PKG_IMG) \
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

LINUX_VMM_FULL_OBJ := $(BUILD_DIR)/linux_vmm.full.o
GPU_SHMEM_FULL_OBJ := $(BUILD_DIR)/gpu_shmem.full.o
VMM_PD_ENTRY_OBJ   := $(BUILD_DIR)/pd_entry.vmm.o
NET_VIRT_PUMP_OBJ  := $(BUILD_DIR)/net_virt_pump.o
VMM_VIRTIO_NET_OBJ := $(BUILD_DIR)/vmm_virtio_net.o
GPA_TRANSLATE_OBJ  := $(BUILD_DIR)/gpa_translate.o
BLK_VIRT_PUMP_OBJ  := $(BUILD_DIR)/blk_virt_pump.o
VMM_VIRTIO_BLK_OBJ := $(BUILD_DIR)/vmm_virtio_blk.o

# ─── Compile linux_vmm.c + gpu_shmem.c ──────────────────────────────────
#
# Use object names that are private to the libvmm build. The main kernel
# Makefile also writes $(BUILD_DIR)/linux_vmm.o for the default stub build, and
# reusing that path can silently link a stale object compiled with incompatible
# flags.
$(LINUX_VMM_FULL_OBJ): $(KERNEL_SRC_DIR)/src/linux_vmm.c $(VMM_CONFIG_STAMP) \
                      $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_net.h \
                      $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_blk.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling linux_vmm.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GPU_SHMEM_FULL_OBJ): $(KERNEL_SRC_DIR)/src/gpu_shmem.c $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling gpu_shmem.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_PD_ENTRY_OBJ): $(KERNEL_SRC_DIR)/src/pd_entry.c $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling pd_entry.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(NET_VIRT_PUMP_OBJ): $(AGENTOS_ROOT)/platform/net-virt/net_virt_pump.c \
                      $(AGENTOS_ROOT)/platform/include/platform/net_layout.h \
                      $(AGENTOS_ROOT)/platform/include/platform/net_virt_pump.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling net_virt_pump.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_VIRTIO_NET_OBJ): $(AGENTOS_ROOT)/platform/net-virt/vmm_virtio_net.c \
                       $(AGENTOS_ROOT)/platform/include/platform/net_layout.h \
                       $(AGENTOS_ROOT)/platform/include/platform/net_virt_pump.h \
                       $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_net.h \
                       $(AGENTOS_ROOT)/platform/include/platform/guest_ram.h \
                       $(LIBVMM_ABS)/include/libvmm/virtio/gpa.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling vmm_virtio_net.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GPA_TRANSLATE_OBJ): $(AGENTOS_ROOT)/platform/guest-ram/gpa_translate.c \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_ram.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling gpa_translate.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(BLK_VIRT_PUMP_OBJ): $(AGENTOS_ROOT)/platform/blk-virt/blk_virt_pump.c \
                      $(AGENTOS_ROOT)/platform/include/platform/blk_layout.h \
                      $(AGENTOS_ROOT)/platform/include/platform/blk_virt_pump.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling blk_virt_pump.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_VIRTIO_BLK_OBJ): $(AGENTOS_ROOT)/platform/blk-virt/vmm_virtio_blk.c \
                       $(AGENTOS_ROOT)/platform/include/platform/blk_layout.h \
                       $(AGENTOS_ROOT)/platform/include/platform/blk_virt_pump.h \
                       $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_blk.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling vmm_virtio_blk.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

# ─── Link linux_vmm.elf ──────────────────────────────────────────────────
$(BUILD_DIR)/linux_vmm.elf: FORCE \
	                             $(LINUX_VMM_FULL_OBJ) \
	                             $(GPU_SHMEM_FULL_OBJ) \
	                             $(VMM_PD_ENTRY_OBJ) \
	                             $(NET_VIRT_PUMP_OBJ) \
	                             $(VMM_VIRTIO_NET_OBJ) \
	                             $(GPA_TRANSLATE_OBJ) \
	                             $(BLK_VIRT_PUMP_OBJ) \
	                             $(VMM_VIRTIO_BLK_OBJ) \
	                             $(BUILD_DIR)/images.o \
	                             $(BUILD_DIR)/libvmm.a \
	                             $(BUILD_DIR)/libsddf_util_debug.a
	@echo "[VMM] Linking linux_vmm.elf..."
	ld.lld -T$(BOARD_DIR)/lib/microkit.ld \
		-L$(BOARD_DIR)/lib \
		$(VMM_PD_ENTRY_OBJ) $(LINUX_VMM_FULL_OBJ) $(GPU_SHMEM_FULL_OBJ) \
		$(NET_VIRT_PUMP_OBJ) $(VMM_VIRTIO_NET_OBJ) $(GPA_TRANSLATE_OBJ) \
		$(BLK_VIRT_PUMP_OBJ) $(VMM_VIRTIO_BLK_OBJ) $(BUILD_DIR)/images.o \
		--start-group \
		$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a \
		--end-group \
		-o $@
	@echo "[VMM] linux_vmm.elf ✓"

# ─── FreeBSD VMM: direct kernel + FDT packaging ───────────────────────────
FREEBSD_DEFAULT_IMAGE := $(AGENTOS_IMAGES)/freebsd-15.0-aarch64.iso
FREEBSD_RAW_IMAGE ?= $(if $(AGENTOS_FREEBSD_IMAGE),$(AGENTOS_FREEBSD_IMAGE),$(if $(FREEBSD_IMAGE),$(FREEBSD_IMAGE),$(FREEBSD_DEFAULT_IMAGE)))
FREEBSD_KERNEL_IMAGE := $(BUILD_DIR)/freebsd-kernel.bin
FREEBSD_DTS := $(KERNEL_SRC_DIR)/freebsd-direct.dts
FREEBSD_DTS_EFFECTIVE := $(FREEBSD_DTS)
FREEBSD_EXTRACT := $(AGENTOS_ROOT)/tools/extract_freebsd_file.py

$(FREEBSD_RAW_IMAGE):
	@echo "[VMM] Fetching FreeBSD 15.0 ISO assets (via xtask fetch-guest)..."
	cargo xtask fetch-guest --os freebsd --output-dir $(AGENTOS_IMAGES)

$(FREEBSD_KERNEL_IMAGE): $(FREEBSD_RAW_IMAGE) $(FREEBSD_EXTRACT)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Extracting FreeBSD kernel..."
	@case "$(FREEBSD_RAW_IMAGE)" in \
		*.iso) cargo xtask fetch-guest --os freebsd --output-dir $(AGENTOS_IMAGES); \
		       cp "$(AGENTOS_IMAGES)/freebsd-15.0-aarch64-kernel" $@ ;; \
		*) python3 $(FREEBSD_EXTRACT) "$(FREEBSD_RAW_IMAGE)" /boot/kernel/kernel.bin $@ || \
		   python3 $(FREEBSD_EXTRACT) "$(FREEBSD_RAW_IMAGE)" /boot/kernel/kernel $@ ;; \
	esac

$(BUILD_DIR)/freebsd-direct.dtb: $(FREEBSD_DTS_EFFECTIVE) $(VMM_CONFIG_STAMP) $(lastword $(MAKEFILE_LIST))
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling FreeBSD device tree..."
	$(DTC) -q -I dts -O dtb $< > $@

$(BUILD_DIR)/freebsd_images.o: $(PKG_IMG) $(FREEBSD_KERNEL_IMAGE) $(BUILD_DIR)/freebsd-direct.dtb
	@echo "[VMM] Packaging FreeBSD kernel + FDT images..."
	clang -c -g3 -x assembler-with-cpp \
		-DGUEST_KERNEL_IMAGE_PATH=\"$(FREEBSD_KERNEL_IMAGE)\" \
		-DGUEST_DTB_IMAGE_PATH=\"$(BUILD_DIR)/freebsd-direct.dtb\" \
		-target aarch64-none-elf \
		$(PKG_IMG) -o $@

# ─── Compile freebsd_vmm.c ───────────────────────────────────────────────
$(BUILD_DIR)/freebsd_vmm.o: $(KERNEL_SRC_DIR)/src/freebsd_vmm.c $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling freebsd_vmm.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

# ─── Link freebsd_vmm.elf ────────────────────────────────────────────────
$(BUILD_DIR)/freebsd_vmm.elf: $(BUILD_DIR)/freebsd_vmm.o \
                               $(BUILD_DIR)/freebsd_images.o \
                               $(BUILD_DIR)/libvmm.a \
                               $(BUILD_DIR)/libsddf_util_debug.a
	@echo "[VMM] Linking freebsd_vmm.elf..."
	ld.lld -T$(KERNEL_SRC_DIR)/freebsd_vmm.ld \
		-L$(BOARD_DIR)/lib \
		$(BUILD_DIR)/freebsd_vmm.o $(BUILD_DIR)/freebsd_images.o \
		--start-group \
		$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a \
		--end-group \
		-o $@
	@echo "[VMM] freebsd_vmm.elf ✓"

vmm-clean:
	rm -f $(BUILD_DIR)/linux_vmm.full.o $(BUILD_DIR)/gpu_shmem.full.o $(BUILD_DIR)/pd_entry.vmm.o $(BUILD_DIR)/linux_vmm.elf
	rm -f $(BUILD_DIR)/net_virt_pump.o $(BUILD_DIR)/vmm_virtio_net.o
	rm -f $(BUILD_DIR)/gpa_translate.o
	rm -f $(BUILD_DIR)/blk_virt_pump.o $(BUILD_DIR)/vmm_virtio_blk.o
	rm -f $(BUILD_DIR)/freebsd_vmm.o $(BUILD_DIR)/freebsd_images.o $(BUILD_DIR)/freebsd_vmm.elf
	rm -f $(BUILD_DIR)/freebsd-direct.dtb
	rm -f $(BUILD_DIR)/images.o $(BUILD_DIR)/vm.dts $(BUILD_DIR)/vm.dtb
	rm -f $(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a
	rm -f $(BUILD_DIR)/vmm_wrapper.mk

FORCE:
