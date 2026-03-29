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

.PHONY: all deps build demo test clean help

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

# Detect macOS vs Linux for BIOS path
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  # Homebrew QEMU puts the RISC-V BIOS here
  BIOS := $(shell find /opt/homebrew /usr/local -name "opensbi-riscv64-generic-fw_dynamic.bin" 2>/dev/null | head -1)
  ifeq ($(BIOS),)
    BIOS := /usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
  endif
endif

all: build

# =============================================================================
# deps: install all build dependencies
# =============================================================================
deps:
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
	@echo "  clang:               $$(clang --version 2>/dev/null | head -1 || echo 'NOT FOUND')"
	@echo "  ld.lld:              $$(ld.lld --version 2>/dev/null | head -1 || echo 'NOT FOUND (try: brew install llvm)')"
	@echo ""
	@echo "✓ Run 'make demo' to build and launch agentOS."
	@echo ""

# =============================================================================
# build: compile the kernel image
# =============================================================================
build:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║         agentOS — building kernel        ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@command -v clang >/dev/null 2>&1 || \
		(echo "ERROR: clang not found. Run 'make deps' first." && exit 1)
	@command -v ld.lld >/dev/null 2>&1 || \
		(echo "ERROR: ld.lld not found. Run 'make deps' first (brew install llvm)." && exit 1)
	@test -d "$(MICROKIT_SDK)" || \
		(echo "ERROR: Microkit SDK not found at $(MICROKIT_SDK)" && exit 1)
	@$(MAKE) -C $(KERNEL_DIR) \
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
# test: CI-friendly boot test (pass/fail, no eyeballing)
# =============================================================================
test: build
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║       agentOS — CI test harness          ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@bash $(ROOT_DIR)scripts/ci-test.sh

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
	@echo "  make test    Build + run CI test harness (pass/fail)"
	@echo "  make clean   Remove build artifacts"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps && make demo"
	@echo ""
