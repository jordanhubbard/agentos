# agentOS Top-Level Makefile
#
# Architecture is driven by config.yaml (target_arch / host_arch) or overrides:
#
#   make demo                          — build + boot using config.yaml defaults
#   make demo TARGET_ARCH=aarch64      — override target to ARM64
#   make demo TARGET_ARCH=x86_64       — override target to x86_64
#   make demo BOARD=rpi4b_4gb          — override board directly (advanced)
#   make deps                          — install all build dependencies
#   make test                          — CI boot test (exit 0/1)
#   make clean                         — remove build artifacts for current target
#   make clean-all                     — remove all build artifacts
#
# Config file: config.yaml (top-level)
#   target_arch: riscv64 | aarch64 | x86_64
#   host_arch:   auto | x86_64 | aarch64
#
# Quick start:
#   make deps && make demo

.PHONY: all deps deps-tools deps-sdk build demo test clean clean-all help

# ─── Read config.yaml (if present) ───────────────────────────────────────────
# Extract target_arch from config.yaml using simple grep/sed (no YAML parser needed)
CONFIG_TARGET := $(shell grep '^target_arch:' config.yaml 2>/dev/null | sed 's/target_arch:[[:space:]]*//' | tr -d '[:space:]')
ifeq ($(CONFIG_TARGET),)
  CONFIG_TARGET := riscv64
endif

# Command-line TARGET_ARCH overrides config.yaml
TARGET_ARCH ?= $(CONFIG_TARGET)

# ─── Board / arch config ──────────────────────────────────────────────────────
# BOARD can be set directly to override the auto-mapping from TARGET_ARCH.
# If not set, it maps:  riscv64 → qemu_virt_riscv64
#                        aarch64 → qemu_virt_aarch64
#                        x86_64  → x86_64_generic
ifndef BOARD
  ifeq ($(TARGET_ARCH),aarch64)
    BOARD := qemu_virt_aarch64
  else ifeq ($(TARGET_ARCH),x86_64)
    BOARD := x86_64_generic
  else
    BOARD := qemu_virt_riscv64
  endif
endif

ifeq ($(BOARD),qemu_virt_aarch64)
  ARCH         := aarch64
  QEMU         := qemu-system-aarch64
  # virtualization=on enables ARM EL2 hypervisor extensions (required for VMM)
  # highmem=off + secure=off match libvmm's tested configuration
  # cortex-a53 is what libvmm examples are tested against
  # -serial mon:stdio is critical: connects PL011 UART to terminal (guest serial I/O)
  QEMU_FLAGS    = -machine virt,virtualization=on,highmem=off,secure=off \
                  -cpu cortex-a53 -m 2G \
                  -serial mon:stdio -nographic \
                  -device loader,file=$(IMAGE),addr=0x70000000,cpu-num=0
else ifeq ($(BOARD),$(filter $(BOARD),x86_64_generic x86_64_generic_vtx))
  ARCH         := x86_64
  QEMU         := qemu-system-x86_64
  # KVM acceleration if available (host x86_64), otherwise TCG
  QEMU_ACCEL   := $(shell [ "$$(uname -m)" = "x86_64" ] && [ -e /dev/kvm ] && echo "-enable-kvm" || echo "")
  QEMU_FLAGS    = -machine q35 -cpu qemu64 -m 2G -nographic \
                  -serial mon:stdio \
                  $(QEMU_ACCEL) \
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
		qemu-system-x86 \
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
	@echo "  qemu-system-x86_64:  $$(qemu-system-x86_64 --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
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
	@echo "Target : $(TARGET_ARCH)"
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
	@echo "Config: config.yaml (target_arch: riscv64|aarch64|x86_64, host_arch: auto)"
	@echo "Current: TARGET_ARCH=$(TARGET_ARCH) → BOARD=$(BOARD)"
	@echo ""
	@echo "Targets:"
	@echo "  make deps                         Install build deps (brew / apt)"
	@echo "  make build                        Build for config.yaml target (default: riscv64)"
	@echo "  make build TARGET_ARCH=aarch64    Build for ARM64"
	@echo "  make build TARGET_ARCH=x86_64     Build for x86_64"
	@echo "  make build BOARD=rpi4b_4gb        Build for a specific Microkit board"
	@echo "  make demo                         Build + launch in QEMU"
	@echo "  make demo TARGET_ARCH=aarch64     ARM64 QEMU demo (with Linux VMM)"
	@echo "  make demo TARGET_ARCH=x86_64      x86_64 QEMU demo"
	@echo "  make test                         CI boot test (exit 0/1)"
	@echo "  make clean                        Remove build artifacts (current target)"
	@echo "  make clean-all                    Remove all build artifacts"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps && make demo"
	@echo ""
	@echo "Architecture override (command line wins over config.yaml):"
	@echo "  TARGET_ARCH=riscv64|aarch64|x86_64"
	@echo "  HOST_ARCH=auto|x86_64|aarch64"
	@echo ""
