# agentOS Top-Level Makefile
#
# Targets:
#   make deps              — install all build dependencies (macOS: brew, Linux: apt)
#   make build             — compile agentOS (default: riscv64)
#   make build BOARD=qemu_virt_aarch64  — compile for ARM64 (Sparky)
#   make demo              — build + launch the QEMU demo
#   make demo BOARD=qemu_virt_aarch64   — demo on aarch64 QEMU
#   make test              — boot in QEMU, verify output, exit 0/1 (CI)
#   make clean             — remove build artifacts for current BOARD
#   make clean-all         — remove all build artifacts
#
# Quick start:
#   make deps && make demo

.PHONY: all deps deps-tools deps-sdk build demo test clean clean-all help \
        build-freebsd demo-freebsd fetch-freebsd-guest

# ─── Board / arch config ──────────────────────────────────────────────────────
BOARD         ?= qemu_virt_riscv64

# ─── FreeBSD VM guest paths ───────────────────────────────────────────────────
FREEBSD_SYSTEM  := manifests/agentos-freebsd.system
FREEBSD_VMM_DIR := kernel/freebsd-vmm
FREEBSD_BUILD   := $(ROOT_DIR)build/freebsd-vm
GUEST_IMAGES    := $(ROOT_DIR)guest-images
EDK2_FD         := $(GUEST_IMAGES)/edk2-aarch64-code.fd
EDK2_VARS       := $(GUEST_IMAGES)/edk2-aarch64-vars.fd
FREEBSD_IMG     := $(GUEST_IMAGES)/freebsd-14.2-arm64.img
FREEBSD_IMG_RW  := $(FREEBSD_BUILD)/freebsd-rootfs.qcow2   # copy-on-write overlay

# QEMU flags for FreeBSD VM guest under agentOS
# seL4 runs at EL2 (virtualization=on), 4GB RAM (2GB for host, 2GB for guest)
QEMU_FREEBSD_FLAGS = \
    -machine virt,virtualization=on,gic-version=2 \
    -cpu cortex-a57 \
    -m 4G \
    -nographic \
    -kernel $(FREEBSD_BUILD)/agentos-freebsd.img \
    -drive if=pflash,format=raw,file=$(EDK2_FD),readonly=on \
    -drive if=pflash,format=raw,file=$(EDK2_VARS) \
    -drive if=virtio,format=qcow2,file=$(FREEBSD_IMG_RW) \
    -serial mon:stdio

ifeq ($(BOARD),qemu_virt_aarch64)
  ARCH         := aarch64
  QEMU         := qemu-system-aarch64
  QEMU_FLAGS    = -machine virt,virtualization=on -cpu cortex-a57 -m 2G -nographic \
                  -kernel $(IMAGE)
else
  ARCH         := riscv64
  QEMU         := qemu-system-riscv64
  BIOS         ?= /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
  QEMU_FLAGS    = -machine virt -cpu rv64 -m 2G -nographic \
                  -bios $(BIOS) \
                  -kernel $(IMAGE)
endif

# ─── Paths ────────────────────────────────────────────────────────────────────
ROOT_DIR      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR    := $(ROOT_DIR)kernel/agentos-root-task
MICROKIT_SDK  := $(ROOT_DIR)microkit-sdk-2.1.0
BUILD_DIR     := $(ROOT_DIR)build/$(BOARD)
IMAGE         := $(BUILD_DIR)/agentos.img

# ─── OS / arch detection ──────────────────────────────────────────────────────
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  ifeq ($(UNAME_M),arm64)
    BREW_PREFIX  := /opt/homebrew
    SDK_PLATFORM := macos-aarch64
  else
    BREW_PREFIX  := /usr/local
    SDK_PLATFORM := macos-x86-64
  endif
  LLVM_BIN := $(shell \
    for d in $(BREW_PREFIX)/opt/llvm/bin $(BREW_PREFIX)/opt/llvm@*/bin; do \
      [ -x "$$d/clang" ] && echo "$$d" && break; \
    done 2>/dev/null)
  LLD_BIN := $(shell \
    for d in $(LLVM_BIN) $(BREW_PREFIX)/opt/lld/bin $(BREW_PREFIX)/opt/lld@*/bin; do \
      [ -x "$$d/ld.lld" ] && echo "$$d" && break; \
    done 2>/dev/null)
  BIOS := $(shell find $(BREW_PREFIX) -name "opensbi-riscv64-generic-fw_dynamic.bin" 2>/dev/null | head -1)
  ifeq ($(BIOS),)
    BIOS := $(BREW_PREFIX)/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
  endif
else ifeq ($(UNAME_S),Linux)
  LLVM_BIN     := /usr/bin
  LLD_BIN      := /usr/bin
  ifeq ($(UNAME_M),aarch64)
    SDK_PLATFORM := linux-aarch64
  else
    SDK_PLATFORM := linux-x86-64
  endif
endif

# SDK download URL
SDK_URL := https://github.com/seL4/microkit/releases/download/2.1.0/microkit-sdk-2.1.0-$(SDK_PLATFORM).tar.gz

all: build

# =============================================================================
# deps
# =============================================================================
deps: deps-tools deps-sdk
	@echo ""
	@echo "✅ All dependencies installed! Run 'make demo' to build and boot."

deps-tools:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║        agentOS — installing deps         ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
ifeq ($(UNAME_S),Darwin)
	@echo "[macOS] Checking Homebrew..."
	@command -v brew >/dev/null 2>&1 || \
		(echo "ERROR: Homebrew not found. Install from https://brew.sh" && exit 1)
	@echo "[macOS] Installing dependencies via brew..."
	@brew install --quiet \
		qemu \
		llvm \
		lld \
		cmake \
		ninja \
		python3 \
		dtc \
		coreutils \
		2>/dev/null || true
	@echo "[macOS] All deps installed. ✓"
else ifeq ($(UNAME_S),Linux)
	@echo "[Linux] Installing dependencies via apt..."
	@sudo apt-get update -qq
	@sudo apt-get install -y --no-install-recommends \
		qemu-system-misc \
		qemu-system-arm \
		clang \
		lld \
		cmake \
		ninja-build \
		python3 \
		python3-pip \
		device-tree-compiler \
		2>/dev/null || true
	@echo "[Linux] All deps installed. ✓"
else
	@echo "ERROR: Unsupported OS: $(UNAME_S)"
	@exit 1
endif
	@echo ""
	@echo "Dependency check:"
	@echo "  qemu-system-riscv64: $$(qemu-system-riscv64 --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
	@echo "  qemu-system-aarch64: $$(qemu-system-aarch64 --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
ifeq ($(UNAME_S),Darwin)
	@echo "  clang (LLVM):        $$($(LLVM_BIN)/clang --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
	@echo "  ld.lld:              $$($(LLD_BIN)/ld.lld --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
else
	@echo "  clang:               $$(clang --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
	@echo "  ld.lld:              $$(ld.lld --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
endif

# Download Microkit SDK if not present
deps-sdk: $(MICROKIT_SDK)/bin/microkit

$(MICROKIT_SDK)/bin/microkit:
	@echo ""
	@echo "[SDK] Downloading Microkit SDK 2.1.0 for $(SDK_PLATFORM)..."
	@mkdir -p $(dir $(MICROKIT_SDK))
	@curl -fSL "$(SDK_URL)" -o /tmp/microkit-sdk.tar.gz
	@echo "[SDK] Extracting..."
	@tar xzf /tmp/microkit-sdk.tar.gz -C $(ROOT_DIR)
	@rm /tmp/microkit-sdk.tar.gz
	@test -x $(MICROKIT_SDK)/bin/microkit && echo "[SDK] ✓ Installed at $(MICROKIT_SDK)" || \
		(echo "ERROR: SDK extraction failed" && exit 1)

# =============================================================================
# build
# =============================================================================
build: deps-sdk
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — building kernel ($(BOARD))   ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
ifeq ($(UNAME_S),Darwin)
	@test -x "$(LLVM_BIN)/clang" || \
		(echo "ERROR: Homebrew LLVM not found. Run 'make deps' first." && exit 1)
	@test -x "$(LLD_BIN)/ld.lld" || \
		(echo "ERROR: ld.lld not found. Run 'make deps' first." && exit 1)
else
	@command -v clang >/dev/null 2>&1 || \
		(echo "ERROR: clang not found. Run 'make deps' first." && exit 1)
	@command -v ld.lld >/dev/null 2>&1 || \
		(echo "ERROR: ld.lld not found. Run 'make deps' first." && exit 1)
endif
	@test -d "$(MICROKIT_SDK)" || \
		(echo "ERROR: Microkit SDK not found. Run 'make deps' first." && exit 1)
	@mkdir -p $(BUILD_DIR)
	@PATH="$(LLVM_BIN):$(LLD_BIN):$$PATH" $(MAKE) -C $(KERNEL_DIR) \
		BUILD_DIR=$(BUILD_DIR) \
		MICROKIT_SDK=$(abspath $(MICROKIT_SDK)) \
		MICROKIT_BOARD=$(BOARD) \
		MICROKIT_CONFIG=debug
	@echo ""
	@echo "✓ Build complete: $(IMAGE)"
	@echo ""

# =============================================================================
# demo: build + launch in QEMU
# =============================================================================
demo: build
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║     agentOS — launching QEMU demo        ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@echo "Board  : $(BOARD)"
	@echo "Image  : $(IMAGE)"
	@echo "QEMU   : $(shell $(QEMU) --version 2>/dev/null | head -1)"
	@echo ""
	@echo "Starting agentOS... (Ctrl-A X to quit QEMU)"
	@echo "──────────────────────────────────────────────"
ifeq ($(ARCH),riscv64)
	@test -f "$(BIOS)" || \
		(echo "ERROR: BIOS not found at $(BIOS). Run 'make deps' first." && exit 1)
endif
	@$(QEMU) $(QEMU_FLAGS)

# =============================================================================
# test: CI boot test (exits 0 on success, 1 on failure)
# =============================================================================
test: build
	@BOARD=$(BOARD) bash scripts/run-tests.sh

# =============================================================================
# FreeBSD VM guest: fetch images, build VMM PD, and launch
# =============================================================================

# Fetch FreeBSD AArch64 disk image and EDK2 UEFI firmware
fetch-freebsd-guest:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║  agentOS — fetching FreeBSD guest images ║"
	@echo "╚══════════════════════════════════════════╝"
	@bash scripts/fetch-freebsd-guest.sh

# Build the FreeBSD VMM protection domain (AArch64 only)
build-freebsd: deps-sdk
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║  agentOS — building FreeBSD VMM PD       ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@test -d "$(ROOT_DIR)libs/libvmm" || \
		(echo "[libvmm] Cloning au-ts/libvmm..." && \
		 git submodule update --init libs/libvmm 2>/dev/null || \
		 git clone https://github.com/au-ts/libvmm.git $(ROOT_DIR)libs/libvmm)
	@echo "[libvmm] Building libvmm for qemu_virt_aarch64..."
	@$(MAKE) -C $(ROOT_DIR)libs/libvmm \
		BOARD=qemu_virt_aarch64 \
		MICROKIT_SDK=$(abspath $(MICROKIT_SDK)) \
		BUILD_DIR=$(ROOT_DIR)libs/libvmm/build/qemu_virt_aarch64
	@echo "[dtc] Compiling FreeBSD guest DTB..."
	@mkdir -p $(FREEBSD_BUILD)
	@dtc -I dts -O dtb \
		-o $(FREEBSD_BUILD)/freebsd-guest.dtb \
		$(FREEBSD_VMM_DIR)/freebsd-vmm.dts
	@echo "[VMM] Building freebsd_vmm.elf..."
	@$(MAKE) -C $(FREEBSD_VMM_DIR) \
		BUILD_DIR=$(FREEBSD_BUILD) \
		MICROKIT_SDK=$(abspath $(MICROKIT_SDK)) \
		LIBVMM_DIR=$(abspath $(ROOT_DIR)libs/libvmm) \
		EDK2_FD=$(EDK2_FD) \
		FREEBSD_DTB=$(FREEBSD_BUILD)/freebsd-guest.dtb
	@echo "[Microkit] Packing agentOS FreeBSD image..."
	@$(MICROKIT_SDK)/bin/microkit \
		$(FREEBSD_SYSTEM) \
		--search-path $(FREEBSD_BUILD) \
		--board qemu_virt_aarch64 \
		--config debug \
		--output $(FREEBSD_BUILD)/agentos-freebsd.img \
		--report $(FREEBSD_BUILD)/agentos-freebsd-report.txt
	@echo ""
	@echo "✓ FreeBSD VM build complete: $(FREEBSD_BUILD)/agentos-freebsd.img"
	@echo ""

# Build + launch FreeBSD as VM guest under agentOS
demo-freebsd: build-freebsd
	@echo ""
	@echo "╔═══════════════════════════════════════════════════╗"
	@echo "║  agentOS — launching FreeBSD VM guest under seL4  ║"
	@echo "╚═══════════════════════════════════════════════════╝"
	@echo ""
	@test -f "$(EDK2_FD)" || \
		(echo "ERROR: EDK2 firmware not found. Run 'make fetch-freebsd-guest' first." && exit 1)
	@test -f "$(FREEBSD_IMG)" || \
		(echo "ERROR: FreeBSD disk image not found. Run 'make fetch-freebsd-guest' first." && exit 1)
	@echo "Stack:"
	@echo "  seL4 (EL2) → freebsd_vmm PD → EDK2 UEFI → loader.efi → FreeBSD"
	@echo ""
	@echo "Creating copy-on-write overlay for FreeBSD disk..."
	@qemu-img create -f qcow2 -b $(FREEBSD_IMG) -F raw $(FREEBSD_IMG_RW) 2>/dev/null || \
		(test -f $(FREEBSD_IMG_RW) && echo "[OK] overlay exists")
	@echo ""
	@echo "Starting agentOS with FreeBSD VM guest... (Ctrl-A X to quit QEMU)"
	@echo "────────────────────────────────────────────────────────────────"
	@qemu-system-aarch64 $(QEMU_FREEBSD_FLAGS)

# =============================================================================
# clean
# =============================================================================
clean:
	@echo "Cleaning build artifacts for $(BOARD)..."
	@rm -rf $(BUILD_DIR)
	@echo "✓ Clean."

clean-all:
	@echo "Cleaning all build artifacts..."
	@rm -rf $(ROOT_DIR)build
	@echo "✓ Clean."

# =============================================================================
# help
# =============================================================================
help:
	@echo ""
	@echo "agentOS — the OS for agents, by agents"
	@echo ""
	@echo "Targets:"
	@echo "  make deps                      Install build deps (brew / apt)"
	@echo "  make build                     Build for riscv64 (default)"
	@echo "  make build BOARD=qemu_virt_aarch64  Build for ARM64 (Sparky)"
	@echo "  make demo                      Build + launch in QEMU"
	@echo "  make demo  BOARD=qemu_virt_aarch64  ARM64 QEMU demo"
	@echo "  make test                      CI boot test (exit 0/1)"
	@echo "  make clean                     Remove build artifacts (current BOARD)"
	@echo "  make clean-all                 Remove all build artifacts"
	@echo ""
	@echo "FreeBSD VM Guest:"
	@echo "  make fetch-freebsd-guest       Download FreeBSD 14 AArch64 + EDK2 UEFI"
	@echo "  make build-freebsd             Build FreeBSD VMM PD (requires libvmm)"
	@echo "  make demo-freebsd              Build + launch FreeBSD under agentOS/seL4"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps && make demo"
	@echo ""
	@echo "FreeBSD quick start:"
	@echo "  make deps && make fetch-freebsd-guest && make demo-freebsd"
	@echo ""
