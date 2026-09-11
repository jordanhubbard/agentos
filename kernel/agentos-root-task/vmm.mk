#
# agentOS profile-backed VMM build — sub-Makefile
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
SEL4_SDK_VERSION ?= 2.1.0
SEL4_SDK ?= $(HOME)/.cache/agentos/microkit-sdk-$(SEL4_SDK_VERSION)
SEL4_PROFILE ?= release
BOARD_DIR ?= $(SEL4_SDK)/board/$(AGENTOS_BOARD)/$(SEL4_PROFILE)

# One invocation prepares one profile-backed slot. Distribution names and
# artifact recipes are deliberately absent here; the bounded Rust executor
# consumes guest profile data and emits a canonical bundle.
VMM_SLOT ?= primary
GUEST_PROFILE ?= buildroot.toml
GUEST_PLACEMENT ?= default
GUEST_BUNDLE := $(BUILD_DIR)/guest-bundle-$(VMM_SLOT)
GUEST_BUNDLE_STAMP := $(GUEST_BUNDLE)/prepared.stamp
GUEST_KERNEL_IMAGE := $(GUEST_BUNDLE)/kernel.bin
GUEST_DTB_IMAGE := $(GUEST_BUNDLE)/guest.dtb
GUEST_INITRD_IMAGE := $(GUEST_BUNDLE)/initrd.bin
GUEST_PROFILE_BIN := $(GUEST_BUNDLE)/profile.bin

PKG_IMG := $(LIBVMM_ABS)/tools/package_guest_images.S
PKG_PROFILE := $(AGENTOS_ROOT)/platform/guest-vmm/package_profile.S

# ─── VMM CFLAGS (used for guest_vmm.c compilation) ───────────────────────
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
    -I$(AGENTOS_ROOT) \
    -I$(AGENTOS_ROOT)/platform/include \
    -MD -MP \
    -target aarch64-none-elf

ifneq ($(filter dual-primary dual-secondary,$(GUEST_PLACEMENT)),)
VMM_CFLAGS += -DAGENTOS_GUEST_DUAL=1
endif

ifeq ($(VMM_SLOT),primary)
VMM_CFLAGS += -DAGENTOS_GUEST_PRIMARY=1
else ifeq ($(VMM_SLOT),secondary)
VMM_CFLAGS += -DAGENTOS_GUEST_SECONDARY=1
else
$(error VMM_SLOT must be primary or secondary, got '$(VMM_SLOT)')
endif

VMM_CONFIG_STAMP := $(BUILD_DIR)/vmm-$(VMM_SLOT).stamp

$(VMM_CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@tmp="$@.tmp"; \
	printf 'VMM_SLOT=%s\nGUEST_PROFILE=%s\nGUEST_PLACEMENT=%s\nSEL4_PROFILE=%s\nVMM_CFLAGS=%s\n' \
		'$(VMM_SLOT)' '$(GUEST_PROFILE)' '$(GUEST_PLACEMENT)' '$(SEL4_PROFILE)' '$(VMM_CFLAGS)' > "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv "$$tmp" "$@"; fi

.PHONY: vmm-all vmm-clean FORCE

ifeq ($(VMM_SLOT),secondary)
vmm-all: $(BUILD_DIR)/guest_vmm_secondary.elf
else
vmm-all: $(BUILD_DIR)/guest_vmm_primary.elf
endif

$(GUEST_BUNDLE_STAMP): FORCE $(AGENTOS_ROOT)/guest-profiles/$(GUEST_PROFILE) \
				       $(KERNEL_SRC_DIR)/vmm.mk
	@cargo xtask fetch-guest --profile $(GUEST_PROFILE)
	@cargo xtask guest-profile --root $(AGENTOS_ROOT)/guest-profiles \
		--profile $(GUEST_PROFILE) --placement $(GUEST_PLACEMENT) \
		--repo-root $(AGENTOS_ROOT) --prepare-dir $(GUEST_BUNDLE)
	@touch $@

$(GUEST_KERNEL_IMAGE) $(GUEST_DTB_IMAGE) $(GUEST_INITRD_IMAGE) $(GUEST_PROFILE_BIN): $(GUEST_BUNDLE_STAMP)

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
                       $(GUEST_KERNEL_IMAGE) \
                       $(GUEST_INITRD_IMAGE) \
                       $(GUEST_DTB_IMAGE)
	@echo "[VMM] Packaging primary guest profile $(GUEST_PROFILE)..."
	clang -c -g3 -x assembler-with-cpp \
		-DGUEST_KERNEL_IMAGE_PATH=\"$(GUEST_KERNEL_IMAGE)\" \
		-DGUEST_DTB_IMAGE_PATH=\"$(GUEST_DTB_IMAGE)\" \
		-DGUEST_INITRD_IMAGE_PATH=\"$(GUEST_INITRD_IMAGE)\" \
		-target aarch64-none-elf \
		$(PKG_IMG) -o $@

$(BUILD_DIR)/guest_primary_profile.o: $(PKG_PROFILE) $(GUEST_PROFILE_BIN)
	clang -c -x assembler-with-cpp \
		-DGUEST_PROFILE_PATH=\"$(GUEST_PROFILE_BIN)\" \
		-target aarch64-none-elf $(PKG_PROFILE) -o $@

GUEST_VMM_PRIMARY_OBJ := $(BUILD_DIR)/guest_vmm_primary.full.o
# Every object compiled with VMM_CFLAGS is slot-private. A dual build invokes
# this file twice in the same BUILD_DIR; sharing these paths would let the
# second invocation silently reuse objects carrying the first slot's macros.
GPU_SHMEM_FULL_OBJ := $(BUILD_DIR)/gpu_shmem.$(VMM_SLOT).full.o
VMM_PD_ENTRY_OBJ   := $(BUILD_DIR)/pd_entry.$(VMM_SLOT).vmm.o
VMM_VIRTIO_NET_OBJ := $(BUILD_DIR)/vmm_virtio_net.$(VMM_SLOT).o
GPA_TRANSLATE_OBJ  := $(BUILD_DIR)/gpa_translate.$(VMM_SLOT).o
VMM_GUEST_RAM_OBJ  := $(BUILD_DIR)/vmm_guest_ram.$(VMM_SLOT).o
GUEST_VMM_RUNTIME_OBJ := $(BUILD_DIR)/guest_vmm_runtime.$(VMM_SLOT).o
GUEST_VMM_LOOP_OBJ := $(BUILD_DIR)/guest_vmm_loop.$(VMM_SLOT).o
GUEST_PROFILE_VALIDATE_OBJ := $(BUILD_DIR)/guest_profile_validate.$(VMM_SLOT).o
GUEST_BOOT_OBJ := $(BUILD_DIR)/guest_boot.$(VMM_SLOT).o
VMM_VIRTIO_BLK_OBJ := $(BUILD_DIR)/vmm_virtio_blk.$(VMM_SLOT).o
VMM_VIRTIO_CONSOLE_OBJ := $(BUILD_DIR)/vmm_virtio_console.$(VMM_SLOT).o

# ─── Compile guest_vmm.c + gpu_shmem.c ──────────────────────────────────
#
# Use object names that are private to the libvmm build. The main kernel
# Makefile also writes $(BUILD_DIR)/guest_vmm_primary.o for the default stub build, and
# reusing that path can silently link a stale object compiled with incompatible
# flags.
$(GUEST_VMM_PRIMARY_OBJ): $(KERNEL_SRC_DIR)/src/guest_vmm.c $(VMM_CONFIG_STAMP) \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_memory_layout.h \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_boot.h \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_profile.h \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_vmm_runtime.h \
                      $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_net.h \
                      $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_blk.h \
                      $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_console.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling guest_vmm.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GPU_SHMEM_FULL_OBJ): $(KERNEL_SRC_DIR)/src/gpu_shmem.c $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling gpu_shmem.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_PD_ENTRY_OBJ): $(KERNEL_SRC_DIR)/src/pd_entry.c $(VMM_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling pd_entry.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_VIRTIO_NET_OBJ): $(AGENTOS_ROOT)/platform/net-virt/vmm_virtio_net.c $(VMM_CONFIG_STAMP) \
                       $(KERNEL_SRC_DIR)/include/contracts/net_virt_contract.h \
                       $(AGENTOS_ROOT)/platform/include/platform/net_layout.h \
                       $(AGENTOS_ROOT)/platform/include/platform/net_host_layout.h \
                       $(AGENTOS_ROOT)/platform/include/platform/net_virt_pump.h \
                       $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_net.h \
                       $(AGENTOS_ROOT)/platform/include/platform/guest_ram.h \
                       $(LIBVMM_ABS)/include/libvmm/virtio/gpa.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling vmm_virtio_net.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GPA_TRANSLATE_OBJ): $(AGENTOS_ROOT)/platform/guest-ram/gpa_translate.c $(VMM_CONFIG_STAMP) \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_ram.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling gpa_translate.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_GUEST_RAM_OBJ): $(AGENTOS_ROOT)/platform/guest-ram/vmm_guest_ram.c $(VMM_CONFIG_STAMP) \
                      $(AGENTOS_ROOT)/platform/include/platform/guest_ram.h \
                      $(LIBVMM_ABS)/include/libvmm/virtio/gpa.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling vmm_guest_ram.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GUEST_VMM_RUNTIME_OBJ): $(AGENTOS_ROOT)/platform/guest-vmm/runtime.c $(VMM_CONFIG_STAMP) \
                         $(AGENTOS_ROOT)/platform/include/platform/guest_vmm_runtime.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling shared guest VMM runtime..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GUEST_VMM_LOOP_OBJ): $(AGENTOS_ROOT)/platform/guest-vmm/loop.c $(VMM_CONFIG_STAMP) \
			      $(AGENTOS_ROOT)/platform/include/platform/guest_vmm_loop.h \
			      $(KERNEL_SRC_DIR)/include/contracts/blk_virt_contract.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling shared guest VMM receive loop..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GUEST_PROFILE_VALIDATE_OBJ): $(AGENTOS_ROOT)/platform/guest-vmm/profile.c $(VMM_CONFIG_STAMP) \
				      $(AGENTOS_ROOT)/platform/include/platform/guest_profile.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling guest profile validator..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(GUEST_BOOT_OBJ): $(AGENTOS_ROOT)/platform/guest-vmm/boot.c $(VMM_CONFIG_STAMP) \
			  $(AGENTOS_ROOT)/platform/include/platform/guest_boot.h \
			  $(AGENTOS_ROOT)/platform/include/platform/guest_profile.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling guest-neutral boot executor..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_VIRTIO_BLK_OBJ): $(AGENTOS_ROOT)/platform/blk-virt/vmm_virtio_blk.c $(VMM_CONFIG_STAMP) \
                       $(KERNEL_SRC_DIR)/include/contracts/blk_virt_contract.h \
                       $(AGENTOS_ROOT)/platform/include/platform/blk_layout.h \
                       $(AGENTOS_ROOT)/platform/include/platform/blk_virt_pump.h \
                       $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_blk.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling vmm_virtio_blk.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

$(VMM_VIRTIO_CONSOLE_OBJ): $(AGENTOS_ROOT)/platform/serial-virt/vmm_virtio_console.c $(VMM_CONFIG_STAMP) \
                           $(AGENTOS_ROOT)/platform/include/platform/serial_layout.h \
                           $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_console.h \
                           $(LIBVMM_ABS)/include/libvmm/virtio/console.h \
                           $(LIBVMM_ABS)/include/libvmm/virtio/gpa.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling vmm_virtio_console.c..."
	clang $(VMM_CFLAGS) -c -o $@ $<

# ─── Link guest_vmm_primary.elf ──────────────────────────────────────────────────
$(BUILD_DIR)/guest_vmm_primary.elf: FORCE \
	                             $(GUEST_VMM_PRIMARY_OBJ) \
	                             $(GPU_SHMEM_FULL_OBJ) \
	                             $(VMM_PD_ENTRY_OBJ) \
	                             $(VMM_VIRTIO_NET_OBJ) \
	                             $(GPA_TRANSLATE_OBJ) \
	                             $(VMM_GUEST_RAM_OBJ) \
	                             $(GUEST_VMM_RUNTIME_OBJ) \
	                             $(GUEST_VMM_LOOP_OBJ) \
	                             $(GUEST_PROFILE_VALIDATE_OBJ) \
	                             $(GUEST_BOOT_OBJ) \
	                             $(VMM_VIRTIO_BLK_OBJ) \
	                             $(VMM_VIRTIO_CONSOLE_OBJ) \
	                             $(BUILD_DIR)/images.o \
	                             $(BUILD_DIR)/guest_primary_profile.o \
	                             $(BUILD_DIR)/libvmm.a \
	                             $(BUILD_DIR)/libsddf_util_debug.a
	@echo "[VMM] Linking guest_vmm_primary.elf..."
	ld.lld -T$(BOARD_DIR)/lib/microkit.ld \
		-L$(BOARD_DIR)/lib \
		$(VMM_PD_ENTRY_OBJ) $(GUEST_VMM_PRIMARY_OBJ) $(GPU_SHMEM_FULL_OBJ) \
		$(VMM_VIRTIO_NET_OBJ) $(GPA_TRANSLATE_OBJ) $(VMM_GUEST_RAM_OBJ) \
		$(GUEST_VMM_RUNTIME_OBJ) \
		$(GUEST_VMM_LOOP_OBJ) \
		$(GUEST_PROFILE_VALIDATE_OBJ) \
		$(GUEST_BOOT_OBJ) \
		$(VMM_VIRTIO_BLK_OBJ) \
		$(VMM_VIRTIO_CONSOLE_OBJ) $(BUILD_DIR)/images.o $(BUILD_DIR)/guest_primary_profile.o \
		--start-group \
		$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a \
		--end-group \
		-o $@
	@echo "[VMM] guest_vmm_primary.elf ✓"

# ─── Package the same canonical bundle for a secondary slot ─────────────
$(BUILD_DIR)/guest_secondary_images.o: $(PKG_IMG) $(GUEST_KERNEL_IMAGE) \
					       $(GUEST_DTB_IMAGE) $(GUEST_INITRD_IMAGE)
	@echo "[VMM] Packaging secondary guest profile $(GUEST_PROFILE)..."
	clang -c -g3 -x assembler-with-cpp \
		-DGUEST_KERNEL_IMAGE_PATH=\"$(GUEST_KERNEL_IMAGE)\" \
		-DGUEST_DTB_IMAGE_PATH=\"$(GUEST_DTB_IMAGE)\" \
		-DGUEST_INITRD_IMAGE_PATH=\"$(GUEST_INITRD_IMAGE)\" \
		-target aarch64-none-elf \
		$(PKG_IMG) -o $@

$(BUILD_DIR)/guest_secondary_profile.o: $(PKG_PROFILE) $(GUEST_PROFILE_BIN)
	clang -c -x assembler-with-cpp \
		-DGUEST_PROFILE_PATH=\"$(GUEST_PROFILE_BIN)\" \
		-target aarch64-none-elf $(PKG_PROFILE) -o $@

# ─── Compile the same profile-backed VMM source for the secondary instance ─
$(BUILD_DIR)/guest_vmm_secondary.o: $(KERNEL_SRC_DIR)/src/guest_vmm.c $(VMM_CONFIG_STAMP) \
                           $(AGENTOS_ROOT)/platform/include/platform/guest_memory_layout.h \
                           $(AGENTOS_ROOT)/platform/include/platform/guest_boot.h \
                           $(AGENTOS_ROOT)/platform/include/platform/guest_profile.h \
                           $(AGENTOS_ROOT)/platform/include/platform/guest_vmm_runtime.h \
                           $(AGENTOS_ROOT)/platform/include/platform/vmm_virtio_console.h
	@mkdir -p $(BUILD_DIR)
	@echo "[VMM] Compiling guest_vmm.c for secondary profile..."
	clang $(VMM_CFLAGS) -c -o $@ $<

# ─── Link guest_vmm_secondary.elf ────────────────────────────────────────────────
$(BUILD_DIR)/guest_vmm_secondary.elf: $(BUILD_DIR)/guest_vmm_secondary.o \
                               $(GPU_SHMEM_FULL_OBJ) \
                               $(VMM_PD_ENTRY_OBJ) \
                               $(BUILD_DIR)/guest_secondary_images.o \
                               $(BUILD_DIR)/guest_secondary_profile.o \
                               $(VMM_VIRTIO_NET_OBJ) \
                               $(GPA_TRANSLATE_OBJ) \
                               $(VMM_GUEST_RAM_OBJ) \
                               $(GUEST_VMM_RUNTIME_OBJ) \
                               $(GUEST_VMM_LOOP_OBJ) \
                               $(GUEST_PROFILE_VALIDATE_OBJ) \
                               $(GUEST_BOOT_OBJ) \
                               $(VMM_VIRTIO_BLK_OBJ) \
                               $(VMM_VIRTIO_CONSOLE_OBJ) \
                               $(BUILD_DIR)/libvmm.a \
                               $(BUILD_DIR)/libsddf_util_debug.a
	@echo "[VMM] Linking guest_vmm_secondary.elf..."
	ld.lld -T$(KERNEL_SRC_DIR)/guest_vmm_secondary.ld \
		-L$(BOARD_DIR)/lib \
		$(VMM_PD_ENTRY_OBJ) $(BUILD_DIR)/guest_vmm_secondary.o $(GPU_SHMEM_FULL_OBJ) \
		$(BUILD_DIR)/guest_secondary_images.o \
		$(VMM_VIRTIO_NET_OBJ) \
		$(GPA_TRANSLATE_OBJ) $(VMM_GUEST_RAM_OBJ) \
		$(GUEST_VMM_RUNTIME_OBJ) \
		$(GUEST_VMM_LOOP_OBJ) \
		$(GUEST_PROFILE_VALIDATE_OBJ) $(BUILD_DIR)/guest_secondary_profile.o \
		$(GUEST_BOOT_OBJ) \
		$(VMM_VIRTIO_BLK_OBJ) \
		$(VMM_VIRTIO_CONSOLE_OBJ) \
		--start-group \
		$(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a \
		--end-group \
		-o $@
	@echo "[VMM] guest_vmm_secondary.elf ✓"

vmm-clean:
	rm -f $(BUILD_DIR)/guest_vmm_primary.full.o $(BUILD_DIR)/gpu_shmem.full.o $(BUILD_DIR)/gpu_shmem.*.full.o $(BUILD_DIR)/pd_entry.vmm.o $(BUILD_DIR)/pd_entry.*.vmm.o $(BUILD_DIR)/guest_vmm_primary.elf
	rm -f $(BUILD_DIR)/net_virt_pump.o $(BUILD_DIR)/net_virt_pump.*.o $(BUILD_DIR)/vmm_virtio_net.o $(BUILD_DIR)/vmm_virtio_net.*.o
	rm -f $(BUILD_DIR)/gpa_translate.o $(BUILD_DIR)/gpa_translate.*.o
	rm -f $(BUILD_DIR)/vmm_guest_ram.o $(BUILD_DIR)/vmm_guest_ram.*.o
	rm -f $(BUILD_DIR)/guest_vmm_runtime.o $(BUILD_DIR)/guest_vmm_runtime.*.o $(BUILD_DIR)/guest_vmm_loop.*.o
	rm -f $(BUILD_DIR)/guest_profile_validate.*.o $(BUILD_DIR)/*guest_profile.o
	rm -f $(BUILD_DIR)/guest_profile_validate.o $(BUILD_DIR)/guest_boot.o $(BUILD_DIR)/guest_boot.*.o
	rm -f $(BUILD_DIR)/blk_virt_pump.o $(BUILD_DIR)/blk_virt_pump.*.o $(BUILD_DIR)/vmm_virtio_blk.o $(BUILD_DIR)/vmm_virtio_blk.*.o
	rm -f $(BUILD_DIR)/vmm_virtio_console.o $(BUILD_DIR)/vmm_virtio_console.*.o
	rm -f $(BUILD_DIR)/guest_vmm_secondary.o $(BUILD_DIR)/guest_secondary_images.o $(BUILD_DIR)/guest_vmm_secondary.elf
	rm -f $(BUILD_DIR)/images.o $(BUILD_DIR)/vm.dts $(BUILD_DIR)/vm.dtb
	rm -rf $(BUILD_DIR)/guest-bundle-primary $(BUILD_DIR)/guest-bundle-secondary
	rm -f $(BUILD_DIR)/libvmm.a $(BUILD_DIR)/libsddf_util_debug.a
	rm -f $(BUILD_DIR)/vmm_wrapper.mk

FORCE:
