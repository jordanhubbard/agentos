# agentOS Top-Level Makefile
#
# Targets:
#   make deps    — install all build dependencies (macOS: brew, Linux: apt)
#   make build   — compile the agentOS kernel image
#   make demo    — build + launch the QEMU demo
#   make clean   — remove build artifacts
#
# Quick start:
#   make deps && make demo

.PHONY: all deps build demo clean help

# Paths
ROOT_DIR      := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR    := $(ROOT_DIR)kernel/agentos-root-task
MICROKIT_SDK  := $(ROOT_DIR)microkit-sdk-2.1.0
BUILD_DIR     := $(KERNEL_DIR)/build-riscv
IMAGE         := $(BUILD_DIR)/agentos.img
BIOS          ?= /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin

# QEMU flags
QEMU          := qemu-system-riscv64
QEMU_FLAGS    := -machine virt -cpu rv64 -m 2G -nographic \
                 -bios $(BIOS) \
                 -kernel $(IMAGE)

# Detect OS and architecture
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  # Homebrew prefix varies: /opt/homebrew (Apple Silicon) vs /usr/local (Intel)
  ifeq ($(UNAME_M),arm64)
    BREW_PREFIX := /opt/homebrew
    SDK_PLATFORM := macos-aarch64
  else
    BREW_PREFIX := /usr/local
    SDK_PLATFORM := macos-x86-64
  endif
  # Homebrew LLVM (Xcode clang lacks RISC-V target)
  LLVM_BIN := $(BREW_PREFIX)/opt/llvm/bin
  # Homebrew QEMU puts the RISC-V BIOS here
  BIOS := $(shell find $(BREW_PREFIX) -name "opensbi-riscv64-generic-fw_dynamic.bin" 2>/dev/null | head -1)
  ifeq ($(BIOS),)
    BIOS := $(BREW_PREFIX)/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
  endif
else ifeq ($(UNAME_S),Linux)
  LLVM_BIN := /usr/bin
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
# deps: install all build dependencies
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
		cmake \
		ninja \
		python3 \
		dtc \
		coreutils \
		2>/dev/null || true
	@echo "[macOS] Checking for RISC-V cross-compiler..."
	@command -v clang >/dev/null 2>&1 || \
		(echo "ERROR: clang not found even after llvm install" && exit 1)
	@echo "[macOS] All deps installed. ✓"
else ifeq ($(UNAME_S),Linux)
	@echo "[Linux] Installing dependencies via apt..."
	@sudo apt-get update -qq
	@sudo apt-get install -y --no-install-recommends \
		qemu-system-misc \
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
ifeq ($(UNAME_S),Darwin)
	@echo "  clang (LLVM):        $$($(LLVM_BIN)/clang --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
	@echo "  ld.lld:              $$($(LLVM_BIN)/ld.lld --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
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
# build: compile the kernel image
# =============================================================================
build: deps-sdk
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║         agentOS — building kernel        ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
ifeq ($(UNAME_S),Darwin)
	@test -x "$(LLVM_BIN)/clang" || \
		(echo "ERROR: Homebrew LLVM not found. Run 'make deps' first." && exit 1)
	@test -x "$(LLVM_BIN)/ld.lld" || \
		(echo "ERROR: ld.lld not found. Run 'make deps' first." && exit 1)
else
	@command -v clang >/dev/null 2>&1 || \
		(echo "ERROR: clang not found. Run 'make deps' first." && exit 1)
	@command -v ld.lld >/dev/null 2>&1 || \
		(echo "ERROR: ld.lld not found. Run 'make deps' first." && exit 1)
endif
	@test -d "$(MICROKIT_SDK)" || \
		(echo "ERROR: Microkit SDK not found. Run 'make deps' first." && exit 1)
	@PATH="$(LLVM_BIN):$$PATH" $(MAKE) -C $(KERNEL_DIR) \
		BUILD_DIR=build-riscv \
		MICROKIT_SDK=$(MICROKIT_SDK) \
		MICROKIT_BOARD=qemu_virt_riscv64 \
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
	@echo "Kernel image : $(IMAGE)"
	@echo "BIOS         : $(BIOS)"
	@echo "QEMU         : $(shell $(QEMU) --version 2>/dev/null | head -1)"
	@echo ""
	@echo "Starting agentOS... (Ctrl-A X to quit QEMU)"
	@echo "──────────────────────────────────────────────"
	@test -f "$(BIOS)" || \
		(echo "ERROR: BIOS not found at $(BIOS). Run 'make deps' first." && exit 1)
	@$(QEMU) $(QEMU_FLAGS)

# =============================================================================
# clean: remove build artifacts
# =============================================================================
clean:
	@echo "Cleaning build artifacts..."
	@$(MAKE) -C $(KERNEL_DIR) \
		BUILD_DIR=build-riscv \
		MICROKIT_SDK=$(MICROKIT_SDK) \
		MICROKIT_BOARD=qemu_virt_riscv64 \
		MICROKIT_CONFIG=debug \
		clean 2>/dev/null || true
	@echo "✓ Clean."

# =============================================================================
# help
# =============================================================================
help:
	@echo ""
	@echo "agentOS — the OS for agents, by agents"
	@echo ""
	@echo "Targets:"
	@echo "  make deps    Install build dependencies (brew on macOS, apt on Linux)"
	@echo "  make build   Compile the agentOS kernel image"
	@echo "  make demo    Build + launch in QEMU (the full experience)"
	@echo "  make clean   Remove build artifacts"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps && make demo"
	@echo ""
