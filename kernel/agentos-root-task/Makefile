# agentOS Root Task Makefile
# This builds the agentOS personality layer that boots on top of seL4.
# Image packing uses cargo xtask gen-image (no external Microkit SDK required).

ifeq ($(strip $(BUILD_DIR)),)
$(error BUILD_DIR must be specified)
endif

# AGENTOS_ARCH: target architecture; derived from AGENTOS_BOARD when not set
AGENTOS_ARCH  ?= riscv64
AGENTOS_BOARD ?= qemu_virt_riscv64

# seL4 kernel headers: shipped in the Microkit SDK board package.
# We use these headers only — libmicrokit is NOT linked.
# The board package provides arch-specific seL4 API headers (sel4/sel4.h, etc.)
REPO_ROOT_MK  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SEL4_SDK_VERSION ?= $(strip $(shell cat "$(REPO_ROOT_MK)/tools/sdk/default-version"))
SEL4_SDK      ?= $(HOME)/.cache/agentos/microkit-sdk-$(SEL4_SDK_VERSION)
SEL4_PROFILE  ?= release
SEL4_INC_DIR  := $(SEL4_SDK)/board/$(AGENTOS_BOARD)/$(SEL4_PROFILE)/include

# Linker script: agentOS-owned replacement for microkit.ld
AGENTOS_LD    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../tools/ld/agentos.ld)

ARCH := $(AGENTOS_ARCH)

ifeq ($(ARCH),riscv64)
  CC     := clang -target riscv64-unknown-elf
  LD     := ld.lld
  CFLAGS_ARCH := -march=rv64imafdc_zicsr_zifencei -mabi=lp64d
else ifeq ($(ARCH),x86_64)
  CC     := clang -target x86_64-unknown-elf
  LD     := ld.lld
  CFLAGS_ARCH := -march=x86-64
else ifeq ($(ARCH),aarch64)
  CC     := clang -target aarch64-unknown-elf
  LD     := ld.lld
  CFLAGS_ARCH := -mstrict-align
else
  $(error Unsupported arch: $(ARCH))
endif

CFLAGS := -nostdlib -ffreestanding -g -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
           -Iinclude -Ilwip/include -I$(REPO_ROOT_MK) -I$(REPO_ROOT_MK)/platform/include -I$(SEL4_INC_DIR) \
           $(CFLAGS_ARCH) \
           -DCONFIG_KERNEL_MCS \
           -D__thread= \
           -MMD -MP

# wasm3-specific flags: suppress warnings from vendored code, include bare-metal shim
CFLAGS_WASM3 := $(CFLAGS) -Iwasm3 -include $(CURDIR)/include/m3_bare_metal.h \
                -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers \
                -Wno-cast-align -Wno-implicit-fallthrough -Dd_m3HasWASI=0

# Swap slot needs wasm3 headers but NOT the bare-metal shim pre-include
# (the shim is only for vendored wasm3 sources that lack seL4 headers)
CFLAGS_SWAPSLOT := $(CFLAGS) -Iwasm3

LDFLAGS :=
LIBS    := -T$(AGENTOS_LD)

# Repository root — two levels above this Makefile (kernel/agentos-root-task/)
REPO_ROOT    := $(abspath $(CURDIR)/../..)
SERVICES_DIR := $(REPO_ROOT)/services

# Extended CFLAGS for service PDs that reference top-level contracts/ headers
# (e.g. "contracts/vibeos/interface.h" resolved from repo root)
CFLAGS_SERVICES := $(CFLAGS) -I$(REPO_ROOT)

# AArch64 loader and custom root_task linker script
LOADER_DIR      := $(REPO_ROOT)/kernel/loader
ROOT_TASK_LD    := $(REPO_ROOT)/tools/ld/root_task.ld
# Resolve llvm-objcopy: prefer the same LLVM installation that provides CC/LD.
# On macOS, llvm-objcopy lives alongside clang in the Homebrew LLVM keg.
ifeq ($(UNAME_S),Darwin)
  UNAME_S := $(shell uname -s)
  _BREW_LLVM := $(shell \
    for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin \
              /opt/homebrew/Cellar/llvm/*/bin /usr/local/Cellar/llvm/*/bin; do \
      [ -x "$$d/llvm-objcopy" ] && echo "$$d/llvm-objcopy" && break; \
    done 2>/dev/null)
  ifneq ($(_BREW_LLVM),)
    OBJCOPY := $(_BREW_LLVM)
  else
    OBJCOPY ?= llvm-objcopy
  endif
else
  OBJCOPY ?= llvm-objcopy
endif

# ─── Root task seL4 boot binary ──────────────────────────────────────────────
# main.c is the seL4 root task entry point and boot sequence orchestrator.
# It is NOT a Microkit PD — it links without libmicrokit and uses its own
# minimal linker flags.  The resulting root_task.elf is loaded directly by
# the seL4 kernel as the first user-mode process.
#
# Board-specific defines passed to main.c so it selects the correct
# system_desc_<arch>.c at compile time:
#   -DBOARD_qemu_virt_aarch64  (for AArch64 QEMU)
#   (default: RISC-V — no define needed; system_desc_riscv64 is the fallback)
ROOT_TASK_SRCS := src/main.c src/ep_alloc.c src/cap_accounting.c \
                  src/pd_tcb.c src/m3_bare_metal.c \
                  src/sel4_crt.c src/log.c \
                  src/ut_alloc.c src/pd_vspace.c src/cap_audit.c \
                  src/vos_create.c src/pd_bundle_stub.c

ifeq ($(ARCH),aarch64)
CFLAGS += -DAGENTOS_LOG_RINGS=1
ROOT_TASK_SRCS += src/system_desc_aarch64.c
CFLAGS_ROOT_TASK := $(CFLAGS) -DBOARD_$(AGENTOS_BOARD)
ROOT_TASK_ASM_OBJS := $(BUILD_DIR)/rt_start_aarch64.o
else ifeq ($(ARCH),x86_64)
ROOT_TASK_SRCS += src/system_desc_x86_64.c src/x86_host_pci.c
CFLAGS_ROOT_TASK := $(CFLAGS)
ROOT_TASK_ASM_OBJS := $(BUILD_DIR)/rt_start_x86_64.o
else
ROOT_TASK_SRCS += src/system_desc_riscv64.c
CFLAGS_ROOT_TASK := $(CFLAGS)
ROOT_TASK_ASM_OBJS :=
endif

ROOT_TASK_OBJS := $(patsubst src/%.c, $(BUILD_DIR)/rt_%.o, $(ROOT_TASK_SRCS))
ROOT_TASK_OBJS += $(BUILD_DIR)/rt_inspect_snapshot.o
ifeq ($(ARCH),x86_64)
ROOT_TASK_OBJS += $(BUILD_DIR)/rt_virtio_pci_caps.o
endif

ifeq ($(AGENTOS_BOARD),x86_64_generic_vtx)
  # Build each coordinator and its canonical adapters in a separate directory.
  # This selector does not add a secondary PD to the system descriptor.
  X86_VMM_SLOT ?= primary
  ifeq ($(X86_VMM_SLOT),secondary)
    CFLAGS += -DAGENTOS_GUEST_SECONDARY=1
  else ifneq ($(X86_VMM_SLOT),primary)
    $(error X86_VMM_SLOT must be primary or secondary)
  endif
  CFLAGS += -DAGENTOS_X86_VTX=1
  CFLAGS_ROOT_TASK += -DAGENTOS_X86_VTX=1
  ifeq ($(X86_DUAL_GUEST),1)
    ifneq ($(X86_VMM_SLOT),primary)
      $(error X86_DUAL_GUEST requires the primary outer build slot)
    endif
    ifneq ($(X86_CC_PCI)$(X86_SECONDARY_BLOCK),11)
      $(error X86_DUAL_GUEST requires X86_CC_PCI=1 and X86_SECONDARY_BLOCK=1)
    endif
    ifneq ($(X86_LINUX_LOGIN),1)
      $(error X86_DUAL_GUEST requires X86_LINUX_LOGIN=1 and prepared Linux profiles)
    endif
    ifeq ($(strip $(X86_SECONDARY_BOOT_PROFILE_BIN)),)
      $(error X86_DUAL_GUEST requires X86_SECONDARY_BOOT_PROFILE_BIN)
    endif
    ifeq ($(strip $(X86_SECONDARY_BOOT_PROFILE_SHA256)),)
      $(error X86_DUAL_GUEST requires X86_SECONDARY_BOOT_PROFILE_SHA256)
    endif
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_DUAL_GUEST=1
    $(BUILD_DIR)/vm_manager.o $(BUILD_DIR)/cc_pd.o $(BUILD_DIR)/net_virt.o $(BUILD_DIR)/blk_virt.o $(BUILD_DIR)/serial_virt.pd.o: CFLAGS += -DAGENTOS_GUEST_SECONDARY=1
  endif
  ifeq ($(X86_SECONDARY_BLOCK),1)
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_SECONDARY_BLOCK=1
  endif
  ifeq ($(X86_LINUX_LOGIN),1)
    ifneq ($(X86_FIRMWARE_RESET),1)
      $(error X86_LINUX_LOGIN requires X86_FIRMWARE_RESET=1)
    endif
    ifeq ($(strip $(X86_BOOT_KERNEL)),)
      $(error X86_LINUX_LOGIN requires X86_BOOT_KERNEL)
    endif
    ifeq ($(strip $(X86_BOOT_INITRD)),)
      $(error X86_LINUX_LOGIN requires X86_BOOT_INITRD)
    endif
    ifeq ($(strip $(X86_BOOT_CMDLINE_FILE)),)
      $(error X86_LINUX_LOGIN requires X86_BOOT_CMDLINE_FILE)
    endif
    ifeq ($(X86_USERSPACE_PROOF),1)
      $(error X86_LINUX_LOGIN conflicts with X86_USERSPACE_PROOF)
    endif
    CFLAGS += -DAGENTOS_X86_LINUX_LOGIN=1
    ifneq ($(strip $(X86_BOOT_SNAPSHOT_SECONDS)),)
      CFLAGS += -DAOS_X86_BOOT_SNAPSHOT_SECONDS=$(X86_BOOT_SNAPSHOT_SECONDS)
    endif
  endif
  ifeq ($(X86_USERSPACE_PROOF),1)
    ifneq ($(X86_FIRMWARE_RESET),1)
      $(error X86_USERSPACE_PROOF requires X86_FIRMWARE_RESET=1)
    endif
    ifeq ($(strip $(X86_BOOT_KERNEL)),)
      $(error X86_USERSPACE_PROOF requires X86_BOOT_KERNEL)
    endif
    ifeq ($(strip $(X86_BOOT_INITRD)),)
      $(error X86_USERSPACE_PROOF requires X86_BOOT_INITRD)
    endif
    CFLAGS += -DAGENTOS_X86_USERSPACE_PROOF=1
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_USERSPACE_PROOF=1
    X86_MANAGED_START := 1
    ifeq ($(X86_LIFECYCLE_TRACE),1)
      CFLAGS += -DAGENTOS_X86_LIFECYCLE_TRACE=1
    endif
    ifeq ($(X86_LIFECYCLE_WITNESS),1)
      CFLAGS += -DAGENTOS_X86_LIFECYCLE_WITNESS=1
    endif
  endif
  ifeq ($(X86_CC_PCI),1)
    X86_MANAGED_START := 1
    ifeq ($(X86_USERSPACE_PROOF),1)
      $(error X86_CC_PCI conflicts with the native X86_USERSPACE_PROOF client)
    endif
    CFLAGS += -DAGENTOS_X86_CC_PCI=1
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_CC_PCI=1
    # CC exposes a dynamically created primary guest, never an auto-boot entry.
    $(BUILD_DIR)/cc_pd.o: CFLAGS += -DAGENTOS_GUEST_PRIMARY=1 -DAGENTOS_GUEST_MANAGED_BOOT=1
  endif
  ifeq ($(X86_MANAGED_START),1)
    ifneq ($(X86_FIRMWARE_RESET),1)
      $(error X86_MANAGED_START requires X86_FIRMWARE_RESET=1)
    endif
    CFLAGS += -DAGENTOS_X86_MANAGED_START=1
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_MANAGED_START=1
  endif
  ifeq ($(X86_GUEST_FAULT_PROOF),1)
    CFLAGS += -DAGENTOS_X86_GUEST_FAULT_PROOF=1
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_GUEST_FAULT_PROOF=1
    ROOT_TASK_OBJS += $(BUILD_DIR)/x86_fault_guest_blob.o
  endif
  ifeq ($(X86_FIRMWARE_MODES),1)
    CFLAGS += -DAGENTOS_X86_FIRMWARE_MODES=1
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_FIRMWARE_MODES=1
  endif
  ifeq ($(X86_FIRMWARE_RESET),1)
    CFLAGS += -DAGENTOS_X86_FIRMWARE_MODES=1 -DAGENTOS_X86_FIRMWARE_RESET=1
    CFLAGS_ROOT_TASK += -DAGENTOS_X86_FIRMWARE_RESET=1
    ROOT_TASK_OBJS += $(BUILD_DIR)/x86_firmware.o
    ifneq ($(strip $(X86_BOOT_KERNEL)),)
      CFLAGS += -DAGENTOS_X86_BOOT_KERNEL=1
    endif
    ifneq ($(strip $(X86_BOOT_KERNEL)$(filter 1,$(X86_CC_PCI))),)
      X86_BOOT_RAM_BYTES ?= 0x10000000u
      CFLAGS += -DAOS_X86_FIRMWARE_RAM=$(X86_BOOT_RAM_BYTES)
      CFLAGS_ROOT_TASK += -DAOS_X86_FIRMWARE_RAM=$(X86_BOOT_RAM_BYTES)
      ifneq ($(X86_BOOT_RAM_BYTES),0x10000000u)
        CFLAGS += -DAOS_X86_FIRMWARE_ROM_VA=0x100000000ull
        CFLAGS_ROOT_TASK += -DAOS_X86_FIRMWARE_ROM_VA=0x100000000ull
      endif
    endif
  endif
endif

ifeq ($(X86_GUEST_FAULT_PROOF),1)
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_event.o
$(BUILD_DIR)/x86_event.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_event.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/x86_fault_guest_code.o: $(REPO_ROOT_MK)/tests/platform/x86_fault_guest.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -c -target x86_64-unknown-elf $< -o $@
$(BUILD_DIR)/x86_fault_guest.bin: $(BUILD_DIR)/x86_fault_guest_code.o
	$(LD) --image-base=0 -Ttext=0x1000 --oformat=binary $< -o $@
$(BUILD_DIR)/x86_fault_guest_blob.o: $(BUILD_DIR)/x86_fault_guest.bin
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 --rename-section .data=.rodata,alloc,load,readonly,data,contents x86_fault_guest.bin x86_fault_guest_blob.o
endif

ifeq ($(X86_FIRMWARE_RESET),1)
ifneq ($(strip $(X86_BOOT_PROFILE_BIN)),)
ifeq ($(strip $(X86_BOOT_KERNEL)),)
$(error X86_BOOT_PROFILE_BIN requires X86_BOOT_KERNEL)
endif
ifeq ($(strip $(X86_BOOT_INITRD)),)
$(error X86_BOOT_PROFILE_BIN requires X86_BOOT_INITRD)
endif
ifeq ($(strip $(X86_BOOT_CMDLINE_FILE)),)
$(error X86_BOOT_PROFILE_BIN requires X86_BOOT_CMDLINE_FILE)
endif
CFLAGS += -DAGENTOS_X86_BOOT_PROFILE=1
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_boot_profile.o $(BUILD_DIR)/x86_profile.o $(BUILD_DIR)/x86_profile_validate.o $(BUILD_DIR)/x86_profile_sha256.o
$(BUILD_DIR)/x86_boot_profile.o: FORCE
	@mkdir -p $(BUILD_DIR)
	@cp "$(X86_BOOT_PROFILE_BIN)" "$(BUILD_DIR)/x86_boot_profile.bin"
	@test "$$(wc -c < "$(BUILD_DIR)/x86_boot_profile.bin")" -eq 640
	@printf '%s  %s\n' '$(X86_BOOT_PROFILE_SHA256)' '$(BUILD_DIR)/x86_boot_profile.bin' | sha256sum -c -
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents x86_boot_profile.bin x86_boot_profile.o
$(BUILD_DIR)/x86_profile.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_profile.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/x86_profile_validate.o: $(REPO_ROOT_MK)/platform/guest-vmm/profile.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/x86_profile_sha256.o: $(REPO_ROOT_MK)/libs/pd-support/sha256_mini.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
endif
ifneq ($(strip $(X86_BOOT_KERNEL)),)
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_boot_kernel.o
$(BUILD_DIR)/x86_boot_kernel.o: FORCE
	@mkdir -p $(BUILD_DIR)
	@test -n "$(X86_BOOT_KERNEL_SHA256)" || { echo 'Set X86_BOOT_KERNEL_SHA256'; exit 1; }
	@cp "$(X86_BOOT_KERNEL)" "$(BUILD_DIR)/x86_boot_kernel.bin"
	@test "$$(wc -c < "$(BUILD_DIR)/x86_boot_kernel.bin")" -gt 0 -a "$$(wc -c < "$(BUILD_DIR)/x86_boot_kernel.bin")" -le 67108864
	@printf '%s  %s\n' '$(X86_BOOT_KERNEL_SHA256)' '$(BUILD_DIR)/x86_boot_kernel.bin' | sha256sum -c -
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents x86_boot_kernel.bin x86_boot_kernel.o
ifneq ($(strip $(X86_BOOT_INITRD)),)
  CFLAGS += -DAGENTOS_X86_BOOT_INITRD=1
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_boot_initrd.o
$(BUILD_DIR)/x86_boot_initrd.o: FORCE
	@mkdir -p $(BUILD_DIR)
	@test -n "$(X86_BOOT_INITRD_SHA256)" || { echo 'Set X86_BOOT_INITRD_SHA256'; exit 1; }
	@cp "$(X86_BOOT_INITRD)" "$(BUILD_DIR)/x86_boot_initrd.bin"
	@test "$$(wc -c < "$(BUILD_DIR)/x86_boot_initrd.bin")" -gt 0 -a "$$(wc -c < "$(BUILD_DIR)/x86_boot_initrd.bin")" -le 67108864
	@printf '%s  %s\n' '$(X86_BOOT_INITRD_SHA256)' '$(BUILD_DIR)/x86_boot_initrd.bin' | sha256sum -c -
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents x86_boot_initrd.bin x86_boot_initrd.o
endif
ifneq ($(strip $(X86_BOOT_CMDLINE_FILE)),)
  CFLAGS += -DAGENTOS_X86_BOOT_CMDLINE=1
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_boot_cmdline.o
$(BUILD_DIR)/x86_boot_cmdline.o: FORCE
	@mkdir -p $(BUILD_DIR)
	@test -n "$(X86_BOOT_CMDLINE_SHA256)" || { echo 'Set X86_BOOT_CMDLINE_SHA256'; exit 1; }
	@cp "$(X86_BOOT_CMDLINE_FILE)" "$(BUILD_DIR)/x86_boot_cmdline.bin"
	@test "$$(wc -c < "$(BUILD_DIR)/x86_boot_cmdline.bin")" -gt 0 -a "$$(wc -c < "$(BUILD_DIR)/x86_boot_cmdline.bin")" -le 4096
	@printf '%s  %s\n' '$(X86_BOOT_CMDLINE_SHA256)' '$(BUILD_DIR)/x86_boot_cmdline.bin' | sha256sum -c -
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents x86_boot_cmdline.bin x86_boot_cmdline.o
endif
else
ifneq ($(strip $(X86_BOOT_INITRD)$(X86_BOOT_CMDLINE_FILE)),)
$(error X86_BOOT_INITRD and X86_BOOT_CMDLINE_FILE require X86_BOOT_KERNEL)
endif
endif
$(BUILD_DIR)/x86_cpu.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_cpu.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_cpu.o
$(BUILD_DIR)/x86_config.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_config.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_config.o
$(BUILD_DIR)/x86_rtc.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_rtc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_rtc.o
$(BUILD_DIR)/x86_apic.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_apic.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_memory.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_memory.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_apic.o $(BUILD_DIR)/x86_memory.o
$(BUILD_DIR)/x86_ioapic.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_ioapic.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_ioapic.o
# The firmware VMM uses the shared transport core, with diagnostics routed
# through the existing freestanding printf implementation in m3_bare_metal.
$(BUILD_DIR)/x86_virtio.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_virtio.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only -I$(REPO_ROOT_MK)/libvmm/include $< -o $@
$(BUILD_DIR)/x86_virtio_mmio.o: $(REPO_ROOT_MK)/libvmm/src/virtio/mmio.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only -I$(REPO_ROOT_MK)/libvmm/include $< -o $@
$(BUILD_DIR)/x86_virtio_gpa.o: $(REPO_ROOT_MK)/libvmm/src/virtio/gpa.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -I$(REPO_ROOT_MK)/libvmm/include $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_virtio.o $(BUILD_DIR)/x86_virtio_mmio.o $(BUILD_DIR)/x86_virtio_gpa.o
# The VCPU can be revoked while this native thread continues serving IPC.
# Do not emit FPU/SIMD instructions in that path, even for integer copies.
# Track this Makefile so changes to these per-object flags rebuild the code.
$(BUILD_DIR)/x86_teardown.o: $(REPO_ROOT_MK)/platform/guest-vmm/teardown.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_release_memory.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_release_memory.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_teardown.o $(BUILD_DIR)/x86_release_memory.o
$(BUILD_DIR)/x86_rebuild_memory.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_rebuild_memory.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_rebuild_memory.o
$(BUILD_DIR)/x86_recreate.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_recreate.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_recreate.o
$(BUILD_DIR)/x86_serial_rebind.o: $(REPO_ROOT_MK)/platform/serial-virt/rebind_client.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_serial_rebind.o
$(BUILD_DIR)/x86_blk_rebind.o: $(REPO_ROOT_MK)/platform/blk-virt/rebind_client.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_blk_rebind.o
$(BUILD_DIR)/x86_net_rebind.o: $(REPO_ROOT_MK)/platform/net-virt/rebind_client.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_net_rebind.o
# Retain the verified native firmware source for managed guest reconstruction.
ifeq ($(X86_MANAGED_START),1)
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_firmware.o
endif
$(BUILD_DIR)/x86_control.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_control.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_runtime.o: $(REPO_ROOT_MK)/platform/guest-vmm/runtime.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_control.o $(BUILD_DIR)/x86_runtime.o
$(BUILD_DIR)/x86_lifecycle_probe.o: $(REPO_ROOT_MK)/tests/platform/x86_lifecycle_probe.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/x86_lifecycle_probe.elf: $(BUILD_DIR)/x86_lifecycle_probe.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/m3_bare_metal.o $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: LDFLAGS += --defsym=printf_=printf
# Keep the MMIO console path; unused PCI entry points are separate sections.
X86_SERIAL_CFLAGS = $(CFLAGS) -Wno-sign-compare \
    -I$(REPO_ROOT_MK)/libvmm/include \
    -I$(REPO_ROOT_MK)/libvmm/dep/sddf/include \
    -I$(REPO_ROOT_MK)/libvmm/dep/sddf/include/microkit \
    -I$(REPO_ROOT_MK)/libvmm/dep/sddf/include/sddf/util/custom_libc
$(BUILD_DIR)/x86_console_backend.o: $(REPO_ROOT_MK)/libvmm/src/virtio/console.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only -ffunction-sections -fdata-sections $< -o $@
$(BUILD_DIR)/x86_vmm_console.o: $(REPO_ROOT_MK)/platform/serial-virt/vmm_virtio_console.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_serial_endpoint.o: $(REPO_ROOT_MK)/platform/serial-virt/endpoint.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_console_backend.o $(BUILD_DIR)/x86_vmm_console.o $(BUILD_DIR)/x86_serial_endpoint.o $(BUILD_DIR)/serial_pump.pd.o
$(BUILD_DIR)/x86_block_backend.o: $(REPO_ROOT_MK)/libvmm/src/virtio/block.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only -ffunction-sections -fdata-sections $< -o $@
$(BUILD_DIR)/x86_vmm_block.o: $(REPO_ROOT_MK)/platform/blk-virt/vmm_virtio_blk.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only -ffunction-sections -fdata-sections $< -o $@
$(BUILD_DIR)/x86_net_backend.o: $(REPO_ROOT_MK)/libvmm/src/virtio/net.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only -ffunction-sections -fdata-sections $< -o $@
$(BUILD_DIR)/x86_vmm_net.o: $(REPO_ROOT_MK)/platform/net-virt/vmm_virtio_net.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only -ffunction-sections -fdata-sections $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_net_backend.o $(BUILD_DIR)/x86_vmm_net.o
$(BUILD_DIR)/x86_fsmalloc.o: $(REPO_ROOT_MK)/libvmm/dep/sddf/util/fsmalloc.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_bitarray.o: $(REPO_ROOT_MK)/libvmm/dep/sddf/util/bitarray.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(X86_SERIAL_CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_block_backend.o $(BUILD_DIR)/x86_vmm_block.o $(BUILD_DIR)/x86_fsmalloc.o $(BUILD_DIR)/x86_bitarray.o
$(BUILD_DIR)/guest_vmm_primary.elf: LDFLAGS += --gc-sections
$(BUILD_DIR)/x86_acpi.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_acpi.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_acpi.o
$(BUILD_DIR)/x86_event.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_event.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_event.o
$(BUILD_DIR)/x86_string.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_string.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_string.o
$(BUILD_DIR)/x86_firmware_vmm.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_firmware.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only -I$(REPO_ROOT_MK)/libvmm/include $< -o $@
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_firmware_vmm.o
.PHONY: verify-x86-firmware
verify-x86-firmware:
	@test -n "$(X86_FIRMWARE_IMAGE)" -a -n "$(X86_FIRMWARE_SHA256)" || \
		{ echo 'Set X86_FIRMWARE_IMAGE and X86_FIRMWARE_SHA256'; exit 1; }
	@test "$$(wc -c < "$(X86_FIRMWARE_IMAGE)")" -eq 4194304 || \
		{ echo 'Firmware reset currently requires a full 4 MiB OVMF image'; exit 1; }
	@printf '%s  %s\n' '$(X86_FIRMWARE_SHA256)' '$(X86_FIRMWARE_IMAGE)' | sha256sum -c -

$(BUILD_DIR)/x86_firmware.o: verify-x86-firmware
	@mkdir -p $(BUILD_DIR)
	@cp "$(X86_FIRMWARE_IMAGE)" "$(BUILD_DIR)/x86_firmware.bin"
	cd $(BUILD_DIR) && $(OBJCOPY) -I binary -O elf64-x86-64 -B i386:x86-64 \
		--rename-section .data=.rodata,alloc,load,readonly,data,contents \
		x86_firmware.bin x86_firmware.o
endif

$(BUILD_DIR)/rt_inspect_snapshot.o: $(REPO_ROOT_MK)/platform/inspect/inspect_snapshot.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/rt_virtio_pci_caps.o: $(REPO_ROOT_MK)/platform/blk-virt/virtio_pci_caps.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_ROOT_TASK) -c $< -o $@

$(BUILD_DIR)/inspect_snapshot.pd.o: $(REPO_ROOT_MK)/platform/inspect/inspect_snapshot.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile root task objects with rt_ prefix to avoid colliding with PD objects
$(BUILD_DIR)/rt_%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_ROOT_TASK) $< -o $@

# Assemble AArch64 startup stub
$(BUILD_DIR)/rt_start_aarch64.o: src/start_aarch64.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -c -target aarch64-unknown-elf $< -o $@

# Assemble x86_64 startup stub
$(BUILD_DIR)/rt_start_x86_64.o: src/start_x86_64.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -c -target x86_64-unknown-elf $< -o $@

# Embedded PD bundle used by root_task on targets whose boot path does not
# provide agentOS extra BootInfo chunks.
PD_BUNDLE       := $(BUILD_DIR)/agentos_pd_bundle.bin
PD_BUNDLE_OBJ   := $(BUILD_DIR)/agentos_pd_bundle.o
# Deferred reference: SYSTEM_TOML is set later in this Makefile.
SYSTEM_TOML_FOR_BUNDLE = $(SYSTEM_TOML)

# ─── AArch64: root_task uses custom LD script + embedded PD bundle ───────────
# On AArch64 the loader (kernel/loader/) loads root_task.elf from DRAM at
# 0x41000000.  The custom root_task.ld places it there and provides the
# .pd_bundle section that holds embedded PD ELFs.
#
# Build sequence:
#   1. Build loader.elf (kernel/loader/)
#   2. Generate PD bundle (xtask gen-pd-bundle) — requires all PD ELFs
#   3. Convert bundle to a linkable .o (objcopy --input binary)
#   4. Link root_task.elf with root_task.ld + bundle .o
#
ifeq ($(ARCH),aarch64)

# Step 1: Build the loader
$(BUILD_DIR)/loader.elf: FORCE
	$(MAKE) -C $(LOADER_DIR) \
		BUILD_DIR=$(BUILD_DIR) \
		AGENTOS_ARCH=$(AGENTOS_ARCH)

# Step 4: Link root_task.elf with the custom AArch64 LD script + bundle object.
# The PD bundle rules are declared after IMAGES/SYSTEM_TOML are finalized so
# Make records the real PD ELF prerequisite list.
$(BUILD_DIR)/root_task.elf: $(ROOT_TASK_ASM_OBJS) $(ROOT_TASK_OBJS) $(PD_BUNDLE_OBJ) $(ROOT_TASK_LD)
	$(LD) $(LDFLAGS) -T$(ROOT_TASK_LD) $(filter-out $(ROOT_TASK_LD),$^) -o $@

else ifeq ($(ARCH),x86_64)

# x86_64 boots via seL4's multiboot path, with root_task.elf passed as the
# initial module.  Embed the PD bundle in root_task.elf so it can launch PDs
# without an agentOS-specific boot-info extension.
$(BUILD_DIR)/root_task.elf: $(ROOT_TASK_ASM_OBJS) $(ROOT_TASK_OBJS) $(PD_BUNDLE_OBJ) $(AGENTOS_LD)
	$(LD) $(LDFLAGS) -T$(AGENTOS_LD) $(filter-out $(AGENTOS_LD),$^) -o $@

else

# RISC-V: simple link with the standard PD linker script.  The current RISC-V
# path still expects PD ELFs via extra BootInfo.
$(BUILD_DIR)/root_task.elf: $(ROOT_TASK_OBJS) $(AGENTOS_LD)
	$(LD) $(LDFLAGS) -T$(AGENTOS_LD) $(filter-out $(AGENTOS_LD),$^) -o $@

endif

# ─── Board identification (passed from top-level Makefile) ───────────────────
BOARD_NAME      ?=
BOARD_NATIVE    ?= 0
BOARD_UART_PHYS ?=
BOARD_UART_SIZE ?=
BOARD_UART_TYPE ?=
BOARD_UART_IRQ  ?=

# Protection Domain ELFs
# monitor/controller: includes vibe_swap for the hot-swap controller-side logic
# m3_bare_metal.c provides memset/memcpy/memmove needed by compiler-generated code
PD_MONITOR_SRCS   := $(REPO_ROOT_MK)/services/legacy-pds/monitor.c $(REPO_ROOT_MK)/services/legacy-pds/cap_broker.c $(REPO_ROOT_MK)/services/legacy-pds/agent_pool.c $(REPO_ROOT_MK)/services/legacy-pds/vibe_swap.c src/m3_bare_metal.c src/log.c $(REPO_ROOT_MK)/services/legacy-pds/boot_integrity.c $(REPO_ROOT_MK)/libs/pd-support/ed25519_verify.c $(REPO_ROOT_MK)/libs/pd-support/monocypher.c $(REPO_ROOT_MK)/services/legacy-pds/cap_policy.c $(REPO_ROOT_MK)/services/legacy-pds/verify.c src/pd_tcb.c src/ut_alloc.c
PD_EVENTBUS_SRCS  := src/m3_bare_metal.c src/log.c   # event_bus.c → services/event-bus/ (E9-S1)
PD_INITAGENT_SRCS := $(REPO_ROOT_MK)/services/legacy-pds/init_agent.c $(REPO_ROOT_MK)/services/legacy-pds/native_net_client.c src/m3_bare_metal.c src/log.c
PD_WORKER_SRCS    := $(REPO_ROOT_MK)/services/legacy-pds/worker.c src/m3_bare_metal.c src/log.c
# agentfs needs m3_bare_metal for memset/sqrtf (no wasm3 headers needed)
PD_AGENTFS_SRCS      := src/m3_bare_metal.c src/log.c   # agentfs.c → services/agentfs/ (E9-S1)
PD_VIBEENGINE_SRCS   := $(REPO_ROOT_MK)/services/legacy-pds/cap_policy.c src/m3_bare_metal.c src/log.c   # vibe_engine.c → services/vibe-engine/ (E9-S1)
PD_SWAPSLOT_SRCS     := $(REPO_ROOT_MK)/services/legacy-pds/swap_slot.c $(REPO_ROOT_MK)/services/legacy-pds/wasm3_host.c src/m3_bare_metal.c src/log.c
PD_LOG_DRAIN_SRCS    := $(REPO_ROOT_MK)/services/log-drain/log_drain.c src/m3_bare_metal.c src/log.c
PD_SERIAL_PD_SRCS    := src/m3_bare_metal.c src/log.c   # serial_pd.c → services/serial-mux/ (E9-S3)
PD_MEMPROFILER_SRCS  := $(REPO_ROOT_MK)/services/legacy-pds/mem_profiler.c src/m3_bare_metal.c src/log.c
PD_NETISOLATOR_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/net_isolator.c src/m3_bare_metal.c src/log.c
PD_SNAPSHOTSCHED_SRCS    := $(REPO_ROOT_MK)/services/legacy-pds/snapshot_sched.c src/m3_bare_metal.c src/log.c
PD_PERFCOUNTERS_SRCS     := $(REPO_ROOT_MK)/services/legacy-pds/perf_counters.c src/m3_bare_metal.c src/log.c
PD_TIMEPARTITION_SRCS    := $(REPO_ROOT_MK)/services/legacy-pds/time_partition.c src/m3_bare_metal.c src/log.c
PD_TRACERECORDER_SRCS    := $(REPO_ROOT_MK)/services/legacy-pds/trace_recorder.c src/m3_bare_metal.c src/log.c
PD_NAMESERVER_SRCS       := $(REPO_ROOT_MK)/services/nameserver/nameserver.c src/m3_bare_metal.c src/log.c
PD_VIRTIO_BLK_SRCS       := $(REPO_ROOT_MK)/services/block-driver/virtio_blk.c $(REPO_ROOT_MK)/services/block-driver/virtio_host_transport.c src/m3_bare_metal.c src/log.c
PD_VFS_SERVER_SRCS       := $(REPO_ROOT_MK)/services/legacy-pds/vfs_server.c src/m3_bare_metal.c src/log.c
PD_SPAWN_SERVER_SRCS     := $(REPO_ROOT_MK)/services/legacy-pds/spawn_server.c $(REPO_ROOT_MK)/libs/pd-support/sha256_mini.c src/m3_bare_metal.c src/log.c
PD_APP_SLOT_SRCS         := $(REPO_ROOT_MK)/services/legacy-pds/app_slot.c $(REPO_ROOT_MK)/libs/pd-support/sha256_mini.c src/m3_bare_metal.c src/log.c
PD_NET_SERVER_SRCS       := $(REPO_ROOT_MK)/services/net-service/lwip_sys.c $(REPO_ROOT_MK)/services/net-service/lwip_shim.c src/m3_bare_metal.c src/log.c   # net_server.c → services/net-server/ (E9-S3)
PD_NET_PD_SRCS           := $(REPO_ROOT_MK)/services/net-service/lwip_sys.c $(REPO_ROOT_MK)/services/net-service/lwip_shim.c $(REPO_ROOT_MK)/services/block-driver/virtio_host_transport.c src/m3_bare_metal.c src/log.c   # net_pd.c → services/net-service/ (E9-S3)
PD_NET_VIRT_SRCS         := src/m3_bare_metal.c src/log.c   # net_virt.c + net_virt_pump.c → platform/net-virt/
PD_BLK_VIRT_SRCS         := src/m3_bare_metal.c src/log.c   # blk_virt.c + blk_virt_pump.c → platform/blk-virt/
PD_SERIAL_VIRT_SRCS      := src/m3_bare_metal.c src/log.c
PD_WG_NET_SRCS           := $(REPO_ROOT_MK)/services/legacy-pds/wg_net.c $(REPO_ROOT_MK)/libs/pd-support/monocypher.c src/m3_bare_metal.c src/log.c
PD_APP_MANAGER_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/app_manager.c src/m3_bare_metal.c src/log.c
PD_HTTP_SVC_SRCS         := $(REPO_ROOT_MK)/services/legacy-pds/http_svc.c src/m3_bare_metal.c src/log.c
PD_VM_MANAGER_SRCS       := $(REPO_ROOT_MK)/services/vm-manager/vm_manager.c $(REPO_ROOT_MK)/platform/guest-vmm/vmm_mux_stub.c src/m3_bare_metal.c src/log.c
PD_AUTH_SERVER_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/auth_server.c src/m3_bare_metal.c src/log.c
PD_EXT2FS_SRCS           := $(REPO_ROOT_MK)/services/legacy-pds/ext2fs.c src/m3_bare_metal.c src/log.c
PD_PROC_SERVER_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/proc_server.c src/m3_bare_metal.c src/log.c
PD_VM_SNAPSHOT_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/vm_snapshot.c src/m3_bare_metal.c src/log.c
PD_PFLOCAL_SERVER_SRCS   := $(REPO_ROOT_MK)/services/legacy-pds/pflocal_server.c src/m3_bare_metal.c src/log.c
PD_PFLOCAL_SRCS          := $(REPO_ROOT_MK)/services/legacy-pds/pflocal_server.c src/m3_bare_metal.c src/log.c
PD_EXEC_SERVER_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/exec_server.c src/m3_bare_metal.c src/log.c
PD_TERM_SERVER_SRCS      := $(REPO_ROOT_MK)/services/legacy-pds/term_server.c src/m3_bare_metal.c src/log.c
PD_BLOCK_PD_SRCS         := $(REPO_ROOT_MK)/services/block-driver/block_pd.c src/m3_bare_metal.c src/log.c
PD_FB_PD_SRCS            := $(REPO_ROOT_MK)/services/legacy-pds/framebuffer_pd.c src/m3_bare_metal.c src/log.c
PD_USB_PD_SRCS           := $(REPO_ROOT_MK)/services/legacy-pds/usb_pd.c src/m3_bare_metal.c src/log.c
PD_CC_PD_SRCS            := $(REPO_ROOT_MK)/services/command-console/cc_pd.c $(REPO_ROOT_MK)/services/command-console/cc_vm_client.c $(REPO_ROOT_MK)/services/command-console/cc_retry_cache.c $(REPO_ROOT_MK)/services/command-console/cc_serial_control.c src/m3_bare_metal.c src/log.c
PD_GPU_SCHED_SRCS        := $(REPO_ROOT_MK)/services/legacy-pds/gpu_sched.c $(REPO_ROOT_MK)/services/legacy-pds/gpu_shmem.c src/m3_bare_metal.c src/log.c
PD_IRQ_PD_SRCS           := $(REPO_ROOT_MK)/services/legacy-pds/irq_pd.c src/m3_bare_metal.c src/log.c
PD_TIMER_PD_SRCS         := $(REPO_ROOT_MK)/services/legacy-pds/timer_pd.c src/m3_bare_metal.c src/log.c
PD_FAULT_HANDLER_SRCS    := $(REPO_ROOT_MK)/services/fault-handler/fault_handler.c src/m3_bare_metal.c src/log.c
PD_FAULT_INJECT_SRCS     := $(REPO_ROOT_MK)/tests/fault-inject/fault_inject.c src/m3_bare_metal.c src/log.c

# ─── Inject seL4 CRT into every service PD ────────────────────────────────────
# sel4_crt.c provides __sel4_ipc_buffer (libmicrokit replacement) needed by
# any PD that includes agentos.h → log_drain_write → seL4_SetMR.
_PD_SRC_VARS := \
    PD_MONITOR_SRCS PD_EVENTBUS_SRCS PD_INITAGENT_SRCS PD_WORKER_SRCS \
    PD_AGENTFS_SRCS PD_VIBEENGINE_SRCS PD_SWAPSLOT_SRCS PD_LOG_DRAIN_SRCS \
    PD_SERIAL_PD_SRCS PD_MEMPROFILER_SRCS PD_NETISOLATOR_SRCS PD_SNAPSHOTSCHED_SRCS \
    PD_PERFCOUNTERS_SRCS PD_TIMEPARTITION_SRCS PD_TRACERECORDER_SRCS PD_NAMESERVER_SRCS \
    PD_VIRTIO_BLK_SRCS PD_VFS_SERVER_SRCS PD_SPAWN_SERVER_SRCS PD_APP_SLOT_SRCS \
    PD_NET_SERVER_SRCS PD_NET_PD_SRCS PD_NET_VIRT_SRCS PD_BLK_VIRT_SRCS PD_SERIAL_VIRT_SRCS PD_WG_NET_SRCS PD_APP_MANAGER_SRCS \
    PD_HTTP_SVC_SRCS PD_VM_MANAGER_SRCS PD_AUTH_SERVER_SRCS PD_EXT2FS_SRCS \
    PD_PROC_SERVER_SRCS PD_VM_SNAPSHOT_SRCS PD_PFLOCAL_SERVER_SRCS PD_PFLOCAL_SRCS \
    PD_EXEC_SERVER_SRCS PD_TERM_SERVER_SRCS PD_BLOCK_PD_SRCS PD_FB_PD_SRCS \
    PD_USB_PD_SRCS PD_CC_PD_SRCS PD_GPU_SCHED_SRCS PD_IRQ_PD_SRCS PD_TIMER_PD_SRCS \
    PD_FAULT_HANDLER_SRCS PD_FAULT_INJECT_SRCS
$(foreach v,$(_PD_SRC_VARS),$(eval $(v) += src/sel4_crt.c))
# pd_entry.c provides _start (ELF entry) + IPC buffer fix for all service PDs.
# It is NOT added to ROOT_TASK_SRCS — the root task has its own _start in
# start_aarch64.S.
$(foreach v,$(_PD_SRC_VARS),$(eval $(v) += $(REPO_ROOT_MK)/libs/pd-support/pd_entry.c))

# wasm3 vendored sources (core interpreter only)
WASM3_SRCS := wasm3/m3_core.c wasm3/m3_env.c wasm3/m3_code.c wasm3/m3_compile.c \
              wasm3/m3_exec.c wasm3/m3_bind.c wasm3/m3_parse.c wasm3/m3_module.c \
              wasm3/m3_function.c wasm3/m3_info.c

pd_objects = $(addprefix $(BUILD_DIR)/,$(notdir $(patsubst %.c,%.o,$(1))))

CONTROLLER_OBJS   := $(call pd_objects,$(PD_MONITOR_SRCS))
EVENTBUS_OBJS     := $(BUILD_DIR)/event_bus.o \
                    $(call pd_objects,$(PD_EVENTBUS_SRCS))
INITAGENT_OBJS    := $(call pd_objects,$(PD_INITAGENT_SRCS))
WORKER_OBJS       := $(call pd_objects,$(PD_WORKER_SRCS))
AGENTFS_OBJS      := $(BUILD_DIR)/agentfs.o \
                    $(call pd_objects,$(PD_AGENTFS_SRCS))
VIBEENGINE_OBJS   := $(BUILD_DIR)/vibe_engine.o \
                    $(call pd_objects,$(PD_VIBEENGINE_SRCS))
LOG_DRAIN_OBJS     := $(call pd_objects,$(PD_LOG_DRAIN_SRCS))
SERIAL_PD_OBJS     := $(BUILD_DIR)/serial_pd.o \
                    $(call pd_objects,$(PD_SERIAL_PD_SRCS))
SWAPSLOT_OBJS      := $(call pd_objects,$(PD_SWAPSLOT_SRCS))
MEMPROFILER_OBJS   := $(call pd_objects,$(PD_MEMPROFILER_SRCS))
NETISOLATOR_OBJS      := $(call pd_objects,$(PD_NETISOLATOR_SRCS))
SNAPSHOTSCHED_OBJS    := $(call pd_objects,$(PD_SNAPSHOTSCHED_SRCS))
PERFCOUNTERS_OBJS     := $(call pd_objects,$(PD_PERFCOUNTERS_SRCS))
TIMEPARTITION_OBJS    := $(call pd_objects,$(PD_TIMEPARTITION_SRCS))
TRACERECORDER_OBJS    := $(call pd_objects,$(PD_TRACERECORDER_SRCS))
NAMESERVER_OBJS       := $(call pd_objects,$(PD_NAMESERVER_SRCS))
VIRTIO_BLK_OBJS       := $(call pd_objects,$(PD_VIRTIO_BLK_SRCS))
VFS_SERVER_OBJS       := $(call pd_objects,$(PD_VFS_SERVER_SRCS))
SPAWN_SERVER_OBJS     := $(call pd_objects,$(PD_SPAWN_SERVER_SRCS))
APP_SLOT_OBJS         := $(call pd_objects,$(PD_APP_SLOT_SRCS))
NET_SERVER_OBJS       := $(BUILD_DIR)/net_server.o \
                    $(call pd_objects,$(PD_NET_SERVER_SRCS))
NET_PD_OBJS           := $(BUILD_DIR)/net_pd.o \
                    $(call pd_objects,$(PD_NET_PD_SRCS))
NET_VIRT_OBJS         := $(BUILD_DIR)/net_virt.o $(BUILD_DIR)/net_virt_pump.pd.o \
                    $(call pd_objects,$(PD_NET_VIRT_SRCS))
BLK_VIRT_OBJS         := $(BUILD_DIR)/blk_virt.o $(BUILD_DIR)/blk_virt_pump.pd.o \
                    $(call pd_objects,$(PD_BLK_VIRT_SRCS))
SERIAL_VIRT_OBJS      := $(BUILD_DIR)/serial_virt.pd.o $(BUILD_DIR)/serial_pump.pd.o \
                       $(BUILD_DIR)/serial_service.pd.o \
                       $(call pd_objects,$(PD_SERIAL_VIRT_SRCS))
WG_NET_OBJS           := $(call pd_objects,$(PD_WG_NET_SRCS))
APP_MANAGER_OBJS      := $(call pd_objects,$(PD_APP_MANAGER_SRCS))
HTTP_SVC_OBJS         := $(call pd_objects,$(PD_HTTP_SVC_SRCS))
VM_MANAGER_OBJS       := $(call pd_objects,$(PD_VM_MANAGER_SRCS))
AUTH_SERVER_OBJS      := $(call pd_objects,$(PD_AUTH_SERVER_SRCS))
EXT2FS_OBJS           := $(call pd_objects,$(PD_EXT2FS_SRCS))
PROC_SERVER_OBJS      := $(call pd_objects,$(PD_PROC_SERVER_SRCS))
VM_SNAPSHOT_OBJS      := $(call pd_objects,$(PD_VM_SNAPSHOT_SRCS))
PFLOCAL_SERVER_OBJS   := $(call pd_objects,$(PD_PFLOCAL_SERVER_SRCS))
PFLOCAL_OBJS          := $(call pd_objects,$(PD_PFLOCAL_SRCS))
EXEC_SERVER_OBJS      := $(call pd_objects,$(PD_EXEC_SERVER_SRCS))
TERM_SERVER_OBJS      := $(call pd_objects,$(PD_TERM_SERVER_SRCS))
WASM3_OBJS            := $(patsubst wasm3/%.c, $(BUILD_DIR)/wasm3_%.o, $(WASM3_SRCS))
BLOCK_PD_OBJS         := $(call pd_objects,$(PD_BLOCK_PD_SRCS))
FB_PD_OBJS            := $(call pd_objects,$(PD_FB_PD_SRCS))
USB_PD_OBJS           := $(call pd_objects,$(PD_USB_PD_SRCS))
CC_PD_OBJS            := $(call pd_objects,$(PD_CC_PD_SRCS))
GPU_SCHED_OBJS        := $(call pd_objects,$(PD_GPU_SCHED_SRCS))
IRQ_PD_OBJS           := $(call pd_objects,$(PD_IRQ_PD_SRCS))
TIMER_PD_OBJS         := $(call pd_objects,$(PD_TIMER_PD_SRCS))
FAULT_HANDLER_OBJS    := $(call pd_objects,$(PD_FAULT_HANDLER_SRCS))
FAULT_INJECT_OBJS     := $(call pd_objects,$(PD_FAULT_INJECT_SRCS))

IMAGES := controller.elf event_bus.elf init_agent.elf worker.elf agentfs.elf vibe_engine.elf vibeos.elf log_drain.elf serial_pd.elf swap_slot.elf mem_profiler.elf net_isolator.elf perf_counters.elf time_partition.elf trace_recorder.elf nameserver.elf virtio_blk.elf vfs_server.elf spawn_server.elf app_slot.elf net_server.elf net_pd.elf net_virt.elf blk_virt.elf wg_net.elf app_manager.elf http_svc.elf vm_manager.elf auth_server.elf ext2fs.elf proc_server.elf vm_snapshot.elf pflocal_server.elf exec_server.elf term_server.elf block_pd.elf framebuffer_pd.elf usb_pd.elf cc_pd.elf gpu_sched.elf timer_svc.elf entropy_svc.elf block_svc.elf usb_svc.elf model_svc.elf tool_svc.elf irq_pd.elf timer_pd.elf fault_handler.elf

# Optional: periodic WASM slot snapshot scheduler
ifdef AGENTOS_SNAPSHOT_SCHED
  CFLAGS += -DAGENTOS_SNAPSHOT_SCHED=1
  IMAGES += snapshot_sched.elf
endif

ifdef FAULT_INJECT
  CFLAGS += -DAGENTOS_FAULT_INJECT=1
  CFLAGS_ROOT_TASK += -DAGENTOS_FAULT_INJECT=1
  IMAGES += fault_inject.elf
endif

ifdef SEL4_TEST_IMAGE
  CFLAGS_ROOT_TASK += -DAGENTOS_SEL4_TEST_IMAGE=1
  # agentos-8f5: on-target contract TAP runner PD (test image only).
  IMAGES += test_runner.elf
endif

ifdef GUEST_INPUT
  ifneq ($(ARCH),aarch64)
    $(error GUEST_INPUT requires aarch64)
  endif
  ifneq ($(strip $(FRAMEBUFFER_TEST)$(NATIVE_RUST_TEST)$(SEL4_TEST_IMAGE)),)
    $(error GUEST_INPUT cannot be combined with focused test images)
  endif
  ifeq ($(strip $(GUEST_PRIMARY_PROFILE)$(GUEST_SECONDARY_PROFILE)),)
    $(error GUEST_INPUT requires a guest profile)
  endif
  CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_INPUT=1
  CFLAGS += -DAGENTOS_GUEST_INPUT=1
  IMAGES += input_virt.elf
endif

ifdef GUEST_GRAPHICS
  ifneq ($(ARCH),aarch64)
    $(error GUEST_GRAPHICS requires aarch64)
  endif
  ifneq ($(strip $(FRAMEBUFFER_TEST)$(NATIVE_RUST_TEST)$(SEL4_TEST_IMAGE)),)
    $(error GUEST_GRAPHICS cannot be combined with focused test images)
  endif
  ifeq ($(strip $(GUEST_PRIMARY_PROFILE)$(GUEST_SECONDARY_PROFILE)),)
    $(error GUEST_GRAPHICS requires a guest profile)
  endif
  CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_GRAPHICS=1
  CFLAGS += -DAGENTOS_GUEST_GRAPHICS=1
  IMAGES += framebuffer_queue.elf
endif

ifdef FRAMEBUFFER_TEST
  ifneq ($(ARCH),aarch64)
    $(error FRAMEBUFFER_TEST requires aarch64)
  endif
  ifneq ($(strip $(NATIVE_RUST_TEST)$(SEL4_TEST_IMAGE)),)
    $(error FRAMEBUFFER_TEST is a separate image variant)
  endif
  CFLAGS_ROOT_TASK += -DAGENTOS_FRAMEBUFFER_TEST=1
  CFLAGS += -DAGENTOS_FRAMEBUFFER_TEST=1
  ifdef FRAMEBUFFER_ISOLATION_PROBE
    CFLAGS_ROOT_TASK += -DAGENTOS_FRAMEBUFFER_ISOLATION_PROBE=$(FRAMEBUFFER_ISOLATION_PROBE)
    CFLAGS += -DAGENTOS_FRAMEBUFFER_ISOLATION_PROBE=$(FRAMEBUFFER_ISOLATION_PROBE)
  endif
  IMAGES += framebuffer_queue.elf framebuffer_client0.elf framebuffer_client1.elf
endif

ifdef DISPLAY_RAMFB
  ifneq ($(ARCH),aarch64)
    $(error DISPLAY_RAMFB requires aarch64)
  endif
  ifeq ($(strip $(FRAMEBUFFER_TEST)$(GUEST_GRAPHICS)),)
    $(error DISPLAY_RAMFB requires FRAMEBUFFER_TEST or GUEST_GRAPHICS)
  endif
  CFLAGS_ROOT_TASK += -DAGENTOS_DISPLAY_RAMFB=1
  CFLAGS += -DAGENTOS_DISPLAY_RAMFB=1
  IMAGES += display_ramfb.elf
endif

ifdef NATIVE_RUST_TEST
  ifneq ($(ARCH),aarch64)
    $(error NATIVE_RUST_TEST currently requires aarch64)
  endif
  ifdef SEL4_TEST_IMAGE
    $(error NATIVE_RUST_TEST and SEL4_TEST_IMAGE are separate image variants)
  endif
  CFLAGS_ROOT_TASK += -DAGENTOS_NATIVE_RUST_TEST=1
  CFLAGS += -DAGENTOS_NATIVE_RUST_TEST=1
  ifdef NATIVE_NET_ISOLATION_PROBE
    CFLAGS_ROOT_TASK += -DAGENTOS_NATIVE_NET_ISOLATION_PROBE=$(NATIVE_NET_ISOLATION_PROBE)
    CFLAGS += -DAGENTOS_NATIVE_NET_ISOLATION_PROBE=$(NATIVE_NET_ISOLATION_PROBE)
  endif
  IMAGES += native_rust_probe.elf native_rust_client.elf
endif

ifdef BLK_ISOLATION_PROBE
  CFLAGS_ROOT_TASK += -DAGENTOS_BLK_ISOLATION_PROBE=$(BLK_ISOLATION_PROBE)
endif

ifdef NET_ISOLATION_PROBE
  CFLAGS_ROOT_TASK += -DAGENTOS_NET_ISOLATION_PROBE=$(NET_ISOLATION_PROBE)
endif

ifdef SERIAL_ISOLATION_PROBE
  CFLAGS_ROOT_TASK += -DAGENTOS_SERIAL_ISOLATION_PROBE=$(SERIAL_ISOLATION_PROBE)
endif

# Profile paths are the canonical build selection. The repository Makefile
# owns the legacy GUEST_OS compatibility adapter.
GUEST_PRIMARY_PROFILE ?=
GUEST_SECONDARY_PROFILE ?=
GUEST_PRIMARY_LARGE ?= 0
IMAGES += serial_virt.elf
# The shared manifest also bundles this ELF for x86; only the AArch64
# descriptor starts it and provisions its queue/observation capabilities.
IMAGES += operator_session.elf

$(BUILD_DIR)/operator_session.o: $(REPO_ROOT_MK)/platform/operator-session/operator_pd.c $(GUEST_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/operator_protocol.o: $(REPO_ROOT_MK)/platform/operator-session/session.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/operator_session.elf: $(BUILD_DIR)/operator_session.o $(BUILD_DIR)/operator_protocol.o $(BUILD_DIR)/inspect_snapshot.pd.o $(BUILD_DIR)/serial_pump.pd.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/m3_bare_metal.o $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
GUEST_CONFIG_STAMP := $(BUILD_DIR)/guest_config.stamp
# SDK paths and compile definitions are part of this stamp. Invalidate all
# existing objects when they change; new objects already compile from source.
# Otherwise an SDK switch can link old syscall/object-type definitions into
# a new root task. EXTRA_PREREQS needs concrete targets, not a pattern binding.
# EXTRA_PREREQS preserves recipes' $< / $^ source lists; private prevents the
# dependency from being inherited by the stamp's own recipe.
$(ROOT_TASK_OBJS) $(wildcard $(BUILD_DIR)/*.o): private .EXTRA_PREREQS = $(GUEST_CONFIG_STAMP)
ifeq ($(LOG_RING_TEST),1)
  CFLAGS += -DAGENTOS_LOG_RING_TEST=1
endif
ifdef LOG_ISOLATION_PROBE
  CFLAGS += -DAGENTOS_LOG_ISOLATION_PROBE=$(LOG_ISOLATION_PROBE)
  CFLAGS_ROOT_TASK += -DAGENTOS_LOG_ISOLATION_PROBE=$(LOG_ISOLATION_PROBE)
endif
ifdef GUEST_GIC_FAILURE_PROBE
  CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_GIC_FAILURE_PROBE=$(GUEST_GIC_FAILURE_PROBE)
endif
ifeq ($(OPERATOR_TEST),1)
  CFLAGS += -DAGENTOS_OPERATOR_TEST=1
endif
ifdef OPERATOR_ISOLATION_PROBE
  CFLAGS += -DAGENTOS_OPERATOR_ISOLATION_PROBE=$(OPERATOR_ISOLATION_PROBE)
  CFLAGS_ROOT_TASK += -DAGENTOS_OPERATOR_ISOLATION_PROBE=$(OPERATOR_ISOLATION_PROBE)
endif
$(BUILD_DIR)/operator_session.o: $(GUEST_CONFIG_STAMP)
ifeq ($(INSPECT_WRITE_PROBE),1)
  CFLAGS += -DAGENTOS_INSPECT_WRITE_PROBE=1
  CFLAGS_ROOT_TASK += -DAGENTOS_INSPECT_WRITE_PROBE=1
endif

# AArch64: include the configured profile-backed VMM slots.
ifeq ($(ARCH),aarch64)
  ifeq ($(GUEST_MANAGED_BOOT),1)
    CFLAGS += -DAGENTOS_GUEST_MANAGED_BOOT=1
  endif
  ifneq ($(strip $(GUEST_PRIMARY_PROFILE)),)
    CFLAGS += -DAGENTOS_GUEST_PRIMARY=1
    CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_PRIMARY=1
    IMAGES += guest_vmm_primary.elf
  endif
  ifneq ($(strip $(GUEST_SECONDARY_PROFILE)),)
    CFLAGS += -DAGENTOS_GUEST_SECONDARY=1
    CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_SECONDARY=1
    IMAGES += guest_vmm_secondary.elf
  endif
  ifneq ($(and $(strip $(GUEST_PRIMARY_PROFILE)),$(strip $(GUEST_SECONDARY_PROFILE))),)
    CFLAGS += -DAGENTOS_GUEST_DUAL=1
    CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_DUAL=1
  endif
  ifeq ($(GUEST_PRIMARY_LARGE),1)
    CFLAGS += -DAGENTOS_GUEST_PRIMARY_LARGE=1
    CFLAGS_ROOT_TASK += -DAGENTOS_GUEST_PRIMARY_LARGE=1
  endif
  ifeq ($(strip $(GUEST_PRIMARY_PROFILE)$(GUEST_SECONDARY_PROFILE)),)
    # No configured guest: retain the primary native stub referenced by the
    # static system descriptor.
    IMAGES += guest_vmm_primary.elf
  endif
else ifeq ($(ARCH),x86_64)
  # Include linux_vmm stub so the image can reference it
  IMAGES += guest_vmm_primary.elf
  ifeq ($(X86_FIRMWARE_RESET),1)
    IMAGES += x86_runner.elf x86_runner_ap.elf
    ifeq ($(X86_DUAL_GUEST),1)
      IMAGES += guest_vmm_secondary.elf x86_secondary_runner.elf x86_secondary_runner_ap.elf
    endif
  endif
  ifeq ($(X86_USERSPACE_PROOF),1)
    IMAGES += x86_lifecycle_probe.elf
  endif
else ifeq ($(ARCH),riscv64)
  # RISC-V 64: process-in-PD VMMs (no hypervisor extension)
  IMAGES += guest_vmm_primary.elf guest_vmm_secondary.elf
endif

$(GUEST_CONFIG_STAMP): FORCE
	@mkdir -p $(BUILD_DIR)
	@tmp="$@.tmp"; \
	printf 'ARCH=%s\nBOARD=%s\nGUEST_PRIMARY_PROFILE=%s\nGUEST_SECONDARY_PROFILE=%s\nGUEST_PRIMARY_LARGE=%s\nFAULT_INJECT=%s\nSEL4_TEST_IMAGE=%s\n' \
		'$(ARCH)' '$(AGENTOS_BOARD)' '$(GUEST_PRIMARY_PROFILE)' '$(GUEST_SECONDARY_PROFILE)' '$(GUEST_PRIMARY_LARGE)' '$(FAULT_INJECT)' '$(SEL4_TEST_IMAGE)' > "$$tmp"; \
	printf 'SEL4_PROFILE=%s\nCFLAGS=%s\nCFLAGS_ROOT_TASK=%s\n' \
		'$(SEL4_PROFILE)' '$(CFLAGS)' '$(CFLAGS_ROOT_TASK)' >> "$$tmp"; \
	if test -f "$@" && cmp -s "$$tmp" "$@"; then rm -f "$$tmp"; else mv "$$tmp" "$@"; fi

$(BUILD_DIR)/rt_main.o: $(GUEST_CONFIG_STAMP)
ifeq ($(ARCH),aarch64)
$(BUILD_DIR)/rt_system_desc_aarch64.o: $(GUEST_CONFIG_STAMP)
else ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/rt_system_desc_x86_64.o: $(GUEST_CONFIG_STAMP)
endif
$(BUILD_DIR)/cc_pd.o: $(GUEST_CONFIG_STAMP)
$(BUILD_DIR)/vm_manager.o: $(GUEST_CONFIG_STAMP)
# net_pd (host IRQ ownership) and net_virt (which VMM listen EPs exist) both
# read the AGENTOS_GUEST_* macros, so a GUEST_OS=none object must not be
# reused for a guest image.
$(BUILD_DIR)/net_pd.o: $(GUEST_CONFIG_STAMP)
$(BUILD_DIR)/net_virt.o: $(GUEST_CONFIG_STAMP)
# blk_virt likewise selects which VMM listen EPs it can notify.
$(BUILD_DIR)/blk_virt.o: $(GUEST_CONFIG_STAMP)
$(BUILD_DIR)/serial_virt.pd.o: $(GUEST_CONFIG_STAMP)

IMAGE_FILE  := $(BUILD_DIR)/agentos.img

.PHONY: check-sel4-sdk
check-sel4-sdk:
	@test -f "$(SEL4_INC_DIR)/sel4/sel4.h" -a \
	      -f "$(SEL4_SDK)/board/$(AGENTOS_BOARD)/$(SEL4_PROFILE)/elf/sel4.elf" || \
		{ echo "ERROR: SEL4_SDK=$(SEL4_SDK) lacks $(AGENTOS_BOARD)/$(SEL4_PROFILE) kernel or headers."; exit 1; }

build: check-sel4-sdk $(IMAGE_FILE) $(BUILD_DIR)/root_task.elf

# ─── Extracted service PDs (E9: sources live in services/, not src/) ─────────
# CFLAGS already includes -Iinclude (kernel/agentos-root-task/include) so all
# seL4 and agentos headers resolve identically to when the files lived in src/.
# CFLAGS_SERVICES adds -I$(REPO_ROOT) for files that include "contracts/…/…".
#
# Older incremental build directories may still have .d files that name the
# service files at their pre-E9 src/ paths. Empty rules let make recover once
# and regenerate those dependency files from the current services/ sources.
src/agentfs.c src/event_bus.c src/vibe_engine.c src/serial_pd.c src/net_server.c src/net_pd.c:

$(BUILD_DIR)/agentfs.o: $(SERVICES_DIR)/agentfs/agentfs.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/event_bus.o: $(SERVICES_DIR)/event-bus/event_bus.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/vibe_engine.o: $(SERVICES_DIR)/vibe-engine/vibe_engine.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

ifeq ($(X86_FIRMWARE_RESET),1)
$(BUILD_DIR)/serial_pd.elf: $(BUILD_DIR)/serial_uart.pd.o $(BUILD_DIR)/serial_pump.pd.o
$(BUILD_DIR)/serial_uart.pd.o: $(REPO_ROOT_MK)/platform/serial-virt/uart.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@
$(BUILD_DIR)/serial_pd.o: $(SERVICES_DIR)/serial-mux/serial_x86.c
else
$(BUILD_DIR)/serial_pd.o: $(SERVICES_DIR)/serial-mux/serial_pd.c
endif
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/net_server.o: $(SERVICES_DIR)/net-server/net_server.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/net_pd.o: $(SERVICES_DIR)/net-service/net_pd.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# net_virt PD: the network virtualizer (docs/TCB.md invariant 2).  Built from
# platform/net-virt/ with the plain PD toolchain; the .pd.o suffix keeps the
# queue pump object apart from the VMM-slot copies vmm.mk compiles.
$(BUILD_DIR)/net_virt.o: $(REPO_ROOT_MK)/platform/net-virt/net_virt.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/net_virt_pump.pd.o: $(REPO_ROOT_MK)/platform/net-virt/net_virt_pump.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# blk_virt PD: the block virtualizer (docs/TCB.md invariant 2).  Same shape
# as net_virt: platform/blk-virt/ sources, plain PD toolchain, .pd.o pump.
$(BUILD_DIR)/blk_virt.o: $(REPO_ROOT_MK)/platform/blk-virt/blk_virt.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/blk_virt_pump.pd.o: $(REPO_ROOT_MK)/platform/blk-virt/blk_virt_pump.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# Serial virtualizer PD and bounded queue service.
$(BUILD_DIR)/serial_virt.pd.o: $(REPO_ROOT_MK)/platform/serial-virt/serial_virt.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/serial_pump.pd.o: $(REPO_ROOT_MK)/platform/serial-virt/pump.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/serial_service.pd.o: $(REPO_ROOT_MK)/platform/serial-virt/service.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# ─── Stub PDs (E9-S4): placeholders for contracts with no implementation yet ──
$(BUILD_DIR)/timer_svc.o: $(SERVICES_DIR)/timer-service/timer_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/entropy_svc.o: $(SERVICES_DIR)/entropy-service/entropy_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/block_svc.o: $(SERVICES_DIR)/block-service/block_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/usb_svc.o: $(SERVICES_DIR)/usb-service/usb_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/model_svc.o: $(SERVICES_DIR)/modelsvc/model_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/tool_svc.o: $(SERVICES_DIR)/toolsvc/tool_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# vibeos — VOS lifecycle PD entry point (IPC dispatch loop — future work)
# vos_create/destroy/snapshot/restore are compiled for the host test suite;
# the bare-metal PD dispatch loop will link them in a future sprint.
$(BUILD_DIR)/vibeos_main.o: $(SERVICES_DIR)/vibeos/vibeos_main.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# vos_*.o kept for reference: compiled by xtask test (AGENTOS_TEST_HOST)
$(BUILD_DIR)/vos_create.o: $(SERVICES_DIR)/vibeos/vos_create.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/vos_destroy.o: $(SERVICES_DIR)/vibeos/vos_destroy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/vos_snapshot.o: $(SERVICES_DIR)/vibeos/vos_snapshot.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SERVICES) $< -o $@

$(BUILD_DIR)/vos_restore.o: $(SERVICES_DIR)/vibeos/vos_restore.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SERVICES) $< -o $@

VIBEOS_OBJS := $(BUILD_DIR)/vibeos_main.o \
               $(BUILD_DIR)/sel4_crt.o \
               $(BUILD_DIR)/pd_entry.o

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/libs/pd-support/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/platform/guest-vmm/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/block-driver/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/command-console/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/fault-handler/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/legacy-pds/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/log-drain/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/nameserver/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/net-service/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/services/vm-manager/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: $(REPO_ROOT_MK)/tests/fault-inject/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/rust_pd_ipc.o: $(REPO_ROOT)/libs/rust-pd/runtime/ipc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/rust_pd_memory.o: $(REPO_ROOT)/libs/rust-pd/runtime/memory.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/framebuffer_service.o: $(REPO_ROOT)/platform/framebuffer/service.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/display_service.o: $(REPO_ROOT)/platform/display/service.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/display_producer.o: $(REPO_ROOT)/platform/display/producer.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/ramfb.o: $(REPO_ROOT)/platform/display/ramfb.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/ramfb_mmio.o: $(REPO_ROOT)/platform/display/ramfb_mmio.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/ramfb_pd.o: $(REPO_ROOT)/platform/display/ramfb_pd.c $(GUEST_CONFIG_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/display_ramfb.elf: $(BUILD_DIR)/ramfb_pd.o $(BUILD_DIR)/ramfb.o $(BUILD_DIR)/ramfb_mmio.o $(BUILD_DIR)/display_service.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD_DIR)/input_service.o: $(REPO_ROOT)/platform/input-virt/service.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/input_rebind_service.o: $(REPO_ROOT)/platform/input-virt/rebind_service.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/input_virt.o: $(REPO_ROOT)/platform/input-virt/input_virt.c $(GUEST_CONFIG_STAMP)
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/input_virt.elf: $(BUILD_DIR)/input_virt.o $(BUILD_DIR)/input_service.o $(BUILD_DIR)/input_rebind_service.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD_DIR)/framebuffer_queue.o: $(REPO_ROOT)/platform/framebuffer/framebuffer_pd.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/framebuffer_observer.o: $(REPO_ROOT)/platform/framebuffer/observer.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/framebuffer_queue.o: $(GUEST_CONFIG_STAMP)
$(BUILD_DIR)/framebuffer_rebind.o: $(REPO_ROOT)/platform/framebuffer/rebind.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/framebuffer_rebind_endpoint.o: $(REPO_ROOT)/platform/framebuffer/rebind_endpoint.c
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD_DIR)/framebuffer_queue.elf: $(BUILD_DIR)/framebuffer_queue.o $(BUILD_DIR)/framebuffer_rebind.o $(BUILD_DIR)/framebuffer_rebind_endpoint.o $(BUILD_DIR)/framebuffer_service.o $(BUILD_DIR)/framebuffer_observer.o $(BUILD_DIR)/display_producer.o $(BUILD_DIR)/display_service.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD_DIR)/framebuffer_client0.o: $(REPO_ROOT)/tests/platform/framebuffer_client_pd.c $(GUEST_CONFIG_STAMP)
	$(CC) $(CFLAGS) -DFB_TEST_CLIENT=0 -c $< -o $@
$(BUILD_DIR)/framebuffer_client1.o: $(REPO_ROOT)/tests/platform/framebuffer_client_pd.c $(GUEST_CONFIG_STAMP)
	$(CC) $(CFLAGS) -DFB_TEST_CLIENT=1 -c $< -o $@
$(BUILD_DIR)/framebuffer_client0.elf: $(BUILD_DIR)/framebuffer_client0.o $(BUILD_DIR)/framebuffer_service.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD_DIR)/framebuffer_client1.elf: $(BUILD_DIR)/framebuffer_client1.o $(BUILD_DIR)/framebuffer_service.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/rust_pd_network.o: $(REPO_ROOT)/libs/rust-pd/runtime/network.c $(GUEST_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/native_rust_probe.elf: FORCE $(BUILD_DIR)/rust_pd_ipc.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o $(BUILD_DIR)/rust_pd_network.o
	cd $(REPO_ROOT) && cargo build -p agentos-native-rust-probe --release --target aarch64-unknown-none
	$(LD) --gc-sections $(LDFLAGS) $(filter-out FORCE,$^) $(REPO_ROOT)/target/aarch64-unknown-none/release/libagentos_native_rust_probe.a $(LIBS) -o $@

$(BUILD_DIR)/native_rust_client.o: $(REPO_ROOT)/tests/native-rust/client.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/native_rust_client.elf: $(BUILD_DIR)/native_rust_client.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/rust_pd_memory.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# Swap slot sources need wasm3 headers + bare-metal shim
$(BUILD_DIR)/swap_slot.o: $(REPO_ROOT_MK)/services/legacy-pds/swap_slot.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SWAPSLOT) $< -o $@

$(BUILD_DIR)/wasm3_host.o: $(REPO_ROOT_MK)/services/legacy-pds/wasm3_host.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SWAPSLOT) $< -o $@

$(BUILD_DIR)/m3_bare_metal.o: src/m3_bare_metal.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# Vendored wasm3 sources — compiled with bare-metal shim
$(BUILD_DIR)/wasm3_%.o: wasm3/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_WASM3) $< -o $@

$(BUILD_DIR)/controller.elf: $(CONTROLLER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/event_bus.elf: $(EVENTBUS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/init_agent.elf: $(INITAGENT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/worker.elf: $(WORKER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/agentfs.elf: $(AGENTFS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vibe_engine.elf: $(VIBEENGINE_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vibeos.elf: $(VIBEOS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# Stub ELFs — each links a single minimal stub object
$(BUILD_DIR)/timer_svc.elf: $(BUILD_DIR)/timer_svc.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/entropy_svc.elf: $(BUILD_DIR)/entropy_svc.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/block_svc.elf: $(BUILD_DIR)/block_svc.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/usb_svc.elf: $(BUILD_DIR)/usb_svc.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/model_svc.elf: $(BUILD_DIR)/model_svc.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/tool_svc.elf: $(BUILD_DIR)/tool_svc.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/log_drain.elf: $(LOG_DRAIN_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/serial_pd.elf: $(SERIAL_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# agentos-8f5: on-target contract TAP runner PD (test image only).  The runner
# unity-#includes the tests/contracts/*_test.c suites, so a single object holds
# all suite logic; -Itests/-Itests/harness let its relative includes resolve.
$(BUILD_DIR)/target_contract_runner.o: $(REPO_ROOT)/tests/harness/target_contract_runner.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -I$(REPO_ROOT)/tests -I$(REPO_ROOT)/tests/harness $< -o $@

$(BUILD_DIR)/test_runner.elf: $(BUILD_DIR)/target_contract_runner.o
	$(LD) $(LDFLAGS) $^ $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(LIBS) -o $@

$(BUILD_DIR)/swap_slot.elf: $(SWAPSLOT_OBJS) $(WASM3_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/mem_profiler.elf: $(MEMPROFILER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_isolator.elf: $(NETISOLATOR_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/snapshot_sched.elf: $(SNAPSHOTSCHED_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/perf_counters.elf: $(PERFCOUNTERS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/time_partition.elf: $(TIMEPARTITION_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/trace_recorder.elf: $(TRACERECORDER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/nameserver.elf: $(NAMESERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/virtio_blk.elf: $(VIRTIO_BLK_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vfs_server.elf: $(VFS_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/spawn_server.elf: $(SPAWN_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/app_slot.elf: $(APP_SLOT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_server.elf: $(NET_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_pd.elf: $(NET_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_virt.elf: $(NET_VIRT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/blk_virt.elf: $(BLK_VIRT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/serial_virt.elf: $(SERIAL_VIRT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/wg_net.elf: $(WG_NET_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/app_manager.elf: $(APP_MANAGER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/http_svc.elf: $(HTTP_SVC_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# vm_manager: multi-VM lifecycle manager (wraps vmm_mux; AArch64 libvmm optional)
$(BUILD_DIR)/vm_manager.o: $(REPO_ROOT_MK)/services/vm-manager/vm_manager.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -I../../libvmm/include $< -o $@

# vmm_mux_stub shares the same generic contract.
$(BUILD_DIR)/vmm_mux_stub.o: $(REPO_ROOT_MK)/platform/guest-vmm/vmm_mux_stub.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -I../../libvmm/include $< -o $@

$(BUILD_DIR)/vm_manager.elf: $(VM_MANAGER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/auth_server.elf: $(AUTH_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/ext2fs.elf: $(EXT2FS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/proc_server.elf: $(PROC_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vm_snapshot.elf: $(VM_SNAPSHOT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/pflocal_server.elf: $(PFLOCAL_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/exec_server.elf: $(EXEC_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/term_server.elf: $(TERM_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@


$(BUILD_DIR)/block_pd.elf: $(BLOCK_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/framebuffer_pd.elf: $(FB_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/usb_pd.elf: $(USB_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/cc_pd.elf: $(CC_PD_OBJS) $(BUILD_DIR)/virtio_host_transport.o $(BUILD_DIR)/serial_pump.pd.o $(BUILD_DIR)/inspect_snapshot.pd.o $(BUILD_DIR)/framebuffer_observer.o $(BUILD_DIR)/input_service.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/gpu_sched.elf: $(GPU_SCHED_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/irq_pd.elf: $(IRQ_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/timer_pd.elf: $(TIMER_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/fault_handler.elf: $(FAULT_HANDLER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/fault_inject.elf: $(FAULT_INJECT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# ─── RISC-V 64 VMMs (process-in-PD, no hypervisor extension) ─────────────
ifeq ($(ARCH),riscv64)
$(BUILD_DIR)/fdt_builder.o: $(REPO_ROOT_MK)/platform/guest-vmm/fdt_builder.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/guest_vmm_primary.o: $(REPO_ROOT_MK)/platform/guest-vmm/guest_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/guest_vmm_primary.o $(BUILD_DIR)/fdt_builder.o \
                             $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/m3_bare_metal.o \
                             $(BUILD_DIR)/log.o $(BUILD_DIR)/pd_entry.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/guest_vmm_secondary.o: $(REPO_ROOT_MK)/platform/guest-vmm/guest_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/guest_vmm_secondary.elf: $(BUILD_DIR)/guest_vmm_secondary.o $(BUILD_DIR)/fdt_builder.o \
                               $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/m3_bare_metal.o \
                               $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
endif

# ─── Profile-backed VMM slots (AArch64 only) ──────────────────────────────
ifeq ($(ARCH),aarch64)
ifeq ($(strip $(GUEST_PRIMARY_PROFILE)),)
# Default AArch64 build: keep a normal agentOS-linked placeholder VMM PD.
# The libvmm Microkit ELF currently has entry 0 under the custom root-task
# loader, so full VMM builds are opt-in via a primary guest profile.
$(BUILD_DIR)/guest_vmm_primary.o: $(REPO_ROOT_MK)/platform/guest-vmm/guest_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DARCH_AARCH64 -DGUEST_VMM_NATIVE_STUB=1 $< -o $@

$(BUILD_DIR)/guest_vmm_primary.elf: FORCE \
                             $(BUILD_DIR)/guest_vmm_primary.o \
                             $(BUILD_DIR)/sel4_crt.o \
                             $(BUILD_DIR)/pd_entry.o \
                             $(BUILD_DIR)/m3_bare_metal.o \
                             $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $(filter-out FORCE,$^) $(LIBS) -o $@
else
ifeq ($(BOARD_NATIVE),1)
# Native AArch64 board (e.g., RPi5): libvmm has QEMU-specific GIC addresses;
# use a stub instead until board-specific VMM support is added.
$(BUILD_DIR)/guest_vmm_primary.o: $(REPO_ROOT_MK)/platform/guest-vmm/guest_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DARCH_AARCH64 -DGUEST_VMM_NATIVE_STUB=1 $< -o $@

$(BUILD_DIR)/guest_vmm_primary.elf: FORCE \
                             $(BUILD_DIR)/guest_vmm_primary.o \
                             $(BUILD_DIR)/sel4_crt.o \
                             $(BUILD_DIR)/pd_entry.o \
                             $(BUILD_DIR)/m3_bare_metal.o \
                             $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $(filter-out FORCE,$^) $(LIBS) -o $@
else
# QEMU AArch64: use libvmm for a full profile-backed VM.
$(BUILD_DIR)/guest_vmm_primary.elf: FORCE
	@echo ""
	@echo "═══ Building primary profile VMM (libvmm) ═══"
	$(MAKE) -f vmm.mk \
		BUILD_DIR=$(BUILD_DIR) \
		AGENTOS_ARCH=$(AGENTOS_ARCH) \
		AGENTOS_BOARD=$(AGENTOS_BOARD) \
		SEL4_SDK=$(SEL4_SDK) \
		SEL4_PROFILE=$(SEL4_PROFILE) \
		VMM_SLOT=primary \
		GUEST_PROFILE=$(GUEST_PRIMARY_PROFILE) \
		GUEST_PLACEMENT=$(if $(strip $(GUEST_SECONDARY_PROFILE)),dual-primary,default) \
		vmm-all
endif
endif
endif

# ─── Secondary profile-backed VMM slot ────────────────────────────────────
ifeq ($(ARCH),aarch64)
ifneq ($(strip $(GUEST_SECONDARY_PROFILE)),)
$(BUILD_DIR)/guest_vmm_secondary.elf: FORCE
	@echo ""
	@echo "═══ Building secondary profile VMM (libvmm) ═══"
	$(MAKE) -f vmm.mk \
		BUILD_DIR=$(BUILD_DIR) \
		AGENTOS_ARCH=$(AGENTOS_ARCH) \
		AGENTOS_BOARD=$(AGENTOS_BOARD) \
		SEL4_SDK=$(SEL4_SDK) \
		SEL4_PROFILE=$(SEL4_PROFILE) \
		VMM_SLOT=secondary \
		GUEST_PROFILE=$(GUEST_SECONDARY_PROFILE) \
		GUEST_PLACEMENT=$(if $(strip $(GUEST_PRIMARY_PROFILE)),dual-secondary,default) \
		vmm-all
	@echo "═══ guest_vmm_secondary.elf ✓ ═══"
endif
endif

# ─── Guest VMM x86_64 paths ────────────────────────────────────────────────
# The regular board retains its passive stub. The distinct VTX board builds
# only the narrow VMX/EPT HLT-exit proof, not a general guest VMM.
ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/x86_runner.pd.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_runner_pd.c $(REPO_ROOT_MK)/platform/include/platform/x86_runner_native.h $(REPO_ROOT_MK)/platform/include/platform/x86_runner.h $(REPO_ROOT_MK)/kernel/agentos-root-task/include/contracts/x86_runner.h Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_runner.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_runner.c $(REPO_ROOT_MK)/platform/include/platform/x86_runner.h $(REPO_ROOT_MK)/kernel/agentos-root-task/include/contracts/x86_runner.h Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_runner_client.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_runner_client.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
$(BUILD_DIR)/x86_smp.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_smp.c Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only $< -o $@
ifeq ($(X86_FIRMWARE_RESET),1)
$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/x86_runner_client.o $(BUILD_DIR)/x86_runner.o $(BUILD_DIR)/x86_smp.o
endif
$(BUILD_DIR)/x86_runner.elf: $(BUILD_DIR)/x86_runner.pd.o $(BUILD_DIR)/x86_runner.o $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/pd_entry.o $(BUILD_DIR)/m3_bare_metal.o $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
$(BUILD_DIR)/x86_runner_ap.elf: $(BUILD_DIR)/x86_runner.elf
	cp $< $@
ifeq ($(X86_DUAL_GUEST),1)
# Each runner ELF is loaded into its own native VSpace/TCB by root.
$(BUILD_DIR)/x86_secondary_runner.elf $(BUILD_DIR)/x86_secondary_runner_ap.elf: $(BUILD_DIR)/x86_runner.elf
	cp $< $@
# Share the immutable guest payload and RAM size, but compile the coordinator
# and its adapters independently with the secondary identity and manifest.
$(BUILD_DIR)/guest_vmm_secondary.elf: FORCE
	$(MAKE) BUILD_DIR=$(abspath $(BUILD_DIR)/secondary) \
		X86_DUAL_GUEST=0 X86_VMM_SLOT=secondary \
		X86_BOOT_PROFILE_BIN=$(X86_SECONDARY_BOOT_PROFILE_BIN) \
		X86_BOOT_PROFILE_SHA256=$(X86_SECONDARY_BOOT_PROFILE_SHA256) \
		$(abspath $(BUILD_DIR)/secondary)/guest_vmm_primary.elf
	cp $(abspath $(BUILD_DIR)/secondary)/guest_vmm_primary.elf $@
endif
ifeq ($(AGENTOS_BOARD),x86_64_generic_vtx)
$(BUILD_DIR)/guest_vmm_primary.o: $(REPO_ROOT_MK)/platform/guest-vmm/x86_vtx_proof.c $(GUEST_CONFIG_STAMP) Makefile
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -mgeneral-regs-only -DARCH_X86_64 $< -o $@
else
$(BUILD_DIR)/guest_vmm_primary.o: $(REPO_ROOT_MK)/platform/guest-vmm/guest_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DARCH_X86_64 -DGUEST_VMM_X86_STUB=1 $< -o $@
endif

$(BUILD_DIR)/guest_vmm_primary.elf: $(BUILD_DIR)/guest_vmm_primary.o \
                             $(BUILD_DIR)/sel4_crt.o \
                             $(BUILD_DIR)/pd_entry.o \
                             $(BUILD_DIR)/m3_bare_metal.o \
                             $(BUILD_DIR)/log.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
endif

# SYSTEM_TOML: per-arch/board TOML system descriptor consumed by xtask gen-image.
# The TOML mirrors the PD list from the corresponding .system XML but omits
# memory-region / channel details (handled by the root task at runtime).
# A minimal fallback TOML is provided; board-specific overrides live in boards/.
SYSTEM_TOML_DEFAULT := $(abspath $(CURDIR)/agentos.toml)
ifdef FRAMEBUFFER_TEST
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-framebuffer-test.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml) $(GUEST_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "framebuffer_queue"\npriority = 215\n'; printf '\n[[pd]]\nname = "framebuffer_client0"\npriority = 214\n'; printf '\n[[pd]]\nname = "framebuffer_client1"\npriority = 214\n'; } > $@
	$(if $(strip $(DISPLAY_RAMFB)),printf '\n[[pd]]\nname = "display_ramfb"\npriority = 216\n' >> $@,true)
else ifneq ($(strip $(GUEST_GRAPHICS)$(GUEST_INPUT)),)
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-guest-graphics.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml) $(GUEST_CONFIG_STAMP)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; $(if $(strip $(GUEST_GRAPHICS)),printf '\n[[pd]]\nname = "framebuffer_queue"\npriority = 215\n';,) $(if $(strip $(GUEST_INPUT)),printf '\n[[pd]]\nname = "input_virt"\npriority = 215\n';,) $(if $(strip $(GUEST_SECONDARY_PROFILE)),printf '\n[[pd]]\nname = "guest_vmm_secondary"\npriority = 250\n';,) } > $@
	$(if $(strip $(DISPLAY_RAMFB)),printf '\n[[pd]]\nname = "display_ramfb"\npriority = 216\n' >> $@,true)
else ifdef NATIVE_RUST_TEST
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-native-rust-test.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "native_rust_probe"\npriority = 245\n'; printf '\n[[pd]]\nname = "native_rust_client"\npriority = 250\n'; } > $@
else ifdef SEL4_TEST_IMAGE
# agentos-8f5: test images (with no guest profiles) append the contract-runner PD
# so gen-pd-bundle embeds test_runner.elf and the root task can spawn it, plus
# the event_bus PD whose contract the runner exercises (not in the default
# image; see system_desc_aarch64.c AGENTOS_SEL4_TEST_IMAGE).
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-test.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "event_bus"\npriority = 195\n'; printf '\n[[pd]]\nname = "test_runner"\npriority = 250\n'; } > $@
else ifdef FAULT_INJECT
ifneq ($(and $(strip $(GUEST_PRIMARY_PROFILE)),$(strip $(GUEST_SECONDARY_PROFILE))),)
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-both-fault-inject.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "guest_vmm_secondary"\npriority = 250\n'; printf '\n[[pd]]\nname = "fault_inject"\npriority = 254\n'; } > $@
else ifneq ($(strip $(GUEST_SECONDARY_PROFILE)),)
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-secondary-fault-inject.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	{ sed 's/name = "guest_vmm_primary"/name = "guest_vmm_secondary"/' $<; printf '\n[[pd]]\nname = "fault_inject"\npriority = 254\n'; } > $@
else
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-fault-inject.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "fault_inject"\npriority = 254\n'; } > $@
endif
else
ifeq ($(strip $(GUEST_PRIMARY_PROFILE)),)
ifneq ($(strip $(GUEST_SECONDARY_PROFILE)),)
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-secondary.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	sed 's/name = "guest_vmm_primary"/name = "guest_vmm_secondary"/' $< > $@
endif
else ifneq ($(strip $(GUEST_SECONDARY_PROFILE)),)
SYSTEM_TOML_DEFAULT := $(abspath $(BUILD_DIR)/agentos-both.toml)
$(SYSTEM_TOML_DEFAULT): $(abspath $(CURDIR)/agentos.toml)
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "guest_vmm_secondary"\npriority = 250\n'; } > $@
endif
endif
_BOARD_TOML := ../../boards/$(BOARD_NAME)/agentos.toml
_BOARD_TOML_NONEMPTY := $(shell [ -s "$(_BOARD_TOML)" ] && printf '%s' "$(_BOARD_TOML)")
SYSTEM_TOML := $(if $(_BOARD_TOML_NONEMPTY),$(_BOARD_TOML_NONEMPTY),$(SYSTEM_TOML_DEFAULT))
ifeq ($(AGENTOS_BOARD),x86_64_generic_vtx)
ifeq ($(X86_FIRMWARE_RESET),1)
SYSTEM_TOML := $(REPO_ROOT_MK)/boards/qemu-x86_64-vtx/firmware.toml
ifeq ($(X86_CC_PCI),1)
SYSTEM_TOML := $(REPO_ROOT_MK)/boards/qemu-x86_64-vtx/cc.toml
else ifeq ($(X86_MANAGED_START),1)
SYSTEM_TOML := $(abspath $(BUILD_DIR)/x86-managed-$(X86_USERSPACE_PROOF).toml)
$(SYSTEM_TOML): $(REPO_ROOT_MK)/boards/qemu-x86_64-vtx/firmware.toml Makefile
	@mkdir -p $(BUILD_DIR)
	{ cat $<; \
	  $(if $(filter 1,$(X86_USERSPACE_PROOF)),printf '\n[[pd]]\nname = "x86_lifecycle_probe"\npriority = 251\n';) \
	  printf '\n[[pd]]\nname = "vm_manager"\npriority = 220\n'; } > $@
endif
endif
endif

# Kernel ELF: pulled directly from the Microkit SDK release package
ifeq ($(X86_DUAL_GUEST),1)
_X86_SINGLE_SYSTEM_TOML := $(SYSTEM_TOML)
SYSTEM_TOML := $(abspath $(BUILD_DIR)/x86-dual.toml)
$(SYSTEM_TOML): $(_X86_SINGLE_SYSTEM_TOML) Makefile
	@mkdir -p $(BUILD_DIR)
	{ cat $<; printf '\n[[pd]]\nname = "x86_secondary_runner"\npriority = 250\n\n[[pd]]\nname = "x86_secondary_runner_ap"\npriority = 250\n\n[[pd]]\nname = "guest_vmm_secondary"\npriority = 250\n'; } > $@
endif

KERNEL_ELF  := $(SEL4_SDK)/board/$(AGENTOS_BOARD)/$(SEL4_PROFILE)/elf/sel4.elf

# On AArch64, loader.elf is also a build artifact (needed at QEMU launch time).
# We add it as a dependency so it is rebuilt whenever the loader sources change.
ifeq ($(ARCH),aarch64)
_LOADER_DEP := $(BUILD_DIR)/loader.elf
else
_LOADER_DEP :=
endif

ifneq ($(filter $(ARCH),aarch64 x86_64),)
# Step 2: Generate the PD bundle after all service PD ELFs and the final
# system TOML have been selected.
$(PD_BUNDLE): $(addprefix $(BUILD_DIR)/, $(IMAGES)) $(SYSTEM_TOML) $(GUEST_CONFIG_STAMP)
	cargo run -p xtask -- gen-pd-bundle \
		--system $(SYSTEM_TOML) \
		--pd-dir $(BUILD_DIR) \
		--out $@

# Step 3: Turn the binary bundle into a relocatable ELF64 object.
#
# objcopy --input binary derives symbol names from the input file path,
# mangling every non-alphanumeric character to '_'.  To get predictable
# symbol names we invoke objcopy from BUILD_DIR with just the basename, so
# the mangled name is always:
#   _binary_agentos_pd_bundle_bin_{start,end,size}
ifeq ($(ARCH),aarch64)
$(PD_BUNDLE_OBJ): $(PD_BUNDLE)
	cd $(BUILD_DIR) && $(OBJCOPY) \
		--input-target=binary \
		--output-target=elf64-littleaarch64 \
		--binary-architecture=aarch64 \
		--rename-section .data=.pd_bundle,alloc,load,data,contents \
		--redefine-sym _binary_agentos_pd_bundle_bin_start=__pd_bundle_start \
		--redefine-sym _binary_agentos_pd_bundle_bin_end=__pd_bundle_end \
		--redefine-sym _binary_agentos_pd_bundle_bin_size=__pd_bundle_size \
		agentos_pd_bundle.bin agentos_pd_bundle.o
else ifeq ($(ARCH),x86_64)
$(PD_BUNDLE_OBJ): $(PD_BUNDLE)
	cd $(BUILD_DIR) && $(OBJCOPY) \
		--input-target=binary \
		--output-target=elf64-x86-64 \
		--binary-architecture=i386:x86-64 \
		--rename-section .data=.pd_bundle,alloc,load,data,contents \
		--redefine-sym _binary_agentos_pd_bundle_bin_start=__pd_bundle_start \
		--redefine-sym _binary_agentos_pd_bundle_bin_end=__pd_bundle_end \
		--redefine-sym _binary_agentos_pd_bundle_bin_size=__pd_bundle_size \
		agentos_pd_bundle.bin agentos_pd_bundle.o
endif
endif

$(IMAGE_FILE): $(addprefix $(BUILD_DIR)/, $(IMAGES)) $(BUILD_DIR)/root_task.elf $(SYSTEM_TOML) $(KERNEL_ELF) $(_LOADER_DEP) $(GUEST_CONFIG_STAMP)
	cargo run -p xtask -- gen-image \
		--arch $(ARCH) \
		--system $(SYSTEM_TOML) \
		--kernel $(KERNEL_ELF) \
		--root-task $(BUILD_DIR)/root_task.elf \
		--pd-dir $(BUILD_DIR) \
		--out $(IMAGE_FILE)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: build clean FORCE

# ─── Auto-generated header dependency files ───────────────────────────────────
include source_migration.mk
# -MMD -MP in CFLAGS causes clang to emit a .d file alongside each .o.
# Including them here makes 'make' recompile any .o whose headers changed.
-include $(wildcard $(BUILD_DIR)/*.d)
