# agentOS Top-Level Makefile
#
# Quick start:
#   make deps && make
#
# Targets:
#   make              — build (native arch) + QEMU (HW-accel) + agentOS console (http://localhost:8795)
#   make dashboard    — start agentOS console only (agentOS already running on hardware)
#   make deps         — install all build dependencies
#   make test         — CI boot test (exit 0/1)
#   make clean        — remove build artifacts for current target
#   make clean-all    — remove all build artifacts

.PHONY: all deps deps-tools deps-sdk submodules console dashboard test test-snapshot-sched test-power-mgr clean clean-all help release release-minor release-major

# ─── Read config.yaml (if present) ───────────────────────────────────────────
CONFIG_TARGET := $(shell grep '^target_arch:' config.yaml 2>/dev/null | sed 's/target_arch:[[:space:]]*//' | tr -d '[:space:]')
ifeq ($(CONFIG_TARGET),)
  CONFIG_TARGET := riscv64
endif

TARGET_ARCH ?= $(CONFIG_TARGET)
GUEST_OS    ?= none

# ─── Board / arch config (used by internal build + test targets) ──────────────
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
  ARCH := aarch64
else ifeq ($(BOARD),$(filter $(BOARD),x86_64_generic x86_64_generic_vtx))
  ARCH := x86_64
else
  ARCH := riscv64
  BIOS ?= /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
endif

# ─── Paths ────────────────────────────────────────────────────────────────────
ROOT_DIR     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR   := $(ROOT_DIR)kernel/agentos-root-task
MICROKIT_SDK := $(ROOT_DIR)microkit-sdk-2.1.0
BUILD_DIR    := $(ROOT_DIR)build/$(BOARD)
IMAGE        := $(BUILD_DIR)/agentos.img
CONSOLE_DIR  := $(ROOT_DIR)console

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
else ifeq ($(UNAME_S),FreeBSD)
  LLVM_BIN     := /usr/local/bin
  LLD_BIN      := /usr/local/bin
  SDK_PLATFORM := unsupported-freebsd
endif

SDK_URL := https://github.com/seL4/microkit/releases/download/2.1.0/microkit-sdk-2.1.0-$(SDK_PLATFORM).tar.gz

# ─── Native arch / HW-accelerated console ────────────────────────────────────
# Normalise uname -m: macOS Apple Silicon reports "arm64", seL4 uses "aarch64"
NATIVE_ARCH := $(shell uname -m | sed 's/arm64/aarch64/')

ifeq ($(UNAME_S),Darwin)
  ifeq ($(NATIVE_ARCH),aarch64)
    # HVF on Apple Silicon has irrecoverable assertion failures with seL4's
    # aarch64 memory access patterns (hvf_vcpu_exec isv assertion, hvf.c).
    # Use TCG (software emulation) until this is resolved upstream in QEMU.
    QEMU_ACCEL_NATIVE :=
  else
    QEMU_ACCEL_NATIVE := -accel hvf
  endif
else ifeq ($(UNAME_S),Linux)
  QEMU_ACCEL_NATIVE := $(shell [ -e /dev/kvm ] && echo "-enable-kvm" || echo "")
else
  QEMU_ACCEL_NATIVE :=
endif

ifeq ($(NATIVE_ARCH),aarch64)
  NATIVE_BOARD      := qemu_virt_aarch64
  NATIVE_QEMU       := qemu-system-aarch64
  NATIVE_QEMU_FLAGS  = -machine virt,virtualization=on,highmem=off,secure=off \
                        -cpu host -m 2G \
                        -display none -monitor none -serial null \
                        $(QEMU_ACCEL_NATIVE) \
                        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
                        -device virtio-net-device,netdev=net0 \
                        -device loader,file=$(NATIVE_IMAGE),addr=0x70000000,cpu-num=0
else
  NATIVE_BOARD      := x86_64_generic
  NATIVE_QEMU       := qemu-system-x86_64
  NATIVE_QEMU_FLAGS  = -machine q35 -cpu host -m 2G \
                        -display none -monitor none -serial null \
                        $(QEMU_ACCEL_NATIVE) \
                        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
                        -device e1000,netdev=net0 \
                        -kernel $(NATIVE_IMAGE)
endif

NATIVE_BUILD_DIR := $(ROOT_DIR)build/$(NATIVE_BOARD)
NATIVE_IMAGE     := $(NATIVE_BUILD_DIR)/agentos.img

all: console

# =============================================================================
# deps
# =============================================================================
deps: deps-tools deps-sdk
	@echo ""
	@echo "✅ All dependencies installed! Run 'make' to launch the console."

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
else ifeq ($(UNAME_S),FreeBSD)
	@echo "[FreeBSD] Installing dependencies via pkg..."
	@sudo pkg install -y \
		llvm \
		dtc \
		dtc-devel \
		gmake \
		python3 \
		curl \
		wget \
		2>/dev/null || true
	@echo "[FreeBSD] All deps installed. ✓"
	@echo ""
	@echo "NOTE: FreeBSD host — cross-compilation only."
	@echo "  QEMU must be installed separately: sudo pkg install qemu"
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

ifeq ($(UNAME_S),FreeBSD)
deps-sdk:
	@echo ""
	@echo "ERROR: Microkit SDK does not ship a FreeBSD host toolchain."
	@echo "  Cross-compile from a Linux or macOS host, or use a Linux VM."
	@echo "  See: https://github.com/seL4/microkit/releases"
	@false
else
deps-sdk: $(MICROKIT_SDK)/bin/microkit
endif

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
# submodules: initialise any uninitialised git submodules
# =============================================================================
submodules:
	@if git submodule status 2>/dev/null | grep -q '^-'; then \
		echo "[submodules] Uninitialised submodule(s) detected — running git submodule update --init --recursive..."; \
		git submodule update --init --recursive; \
		echo "[submodules] ✓ Submodules ready."; \
	fi

# =============================================================================
# build (internal — used by console and test)
# =============================================================================
build: submodules deps-sdk
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — building kernel ($(BOARD))   ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
ifeq ($(UNAME_S),FreeBSD)
	@echo "ERROR: Microkit SDK does not ship a FreeBSD host toolchain."
	@echo "  Cross-compile from a Linux or macOS host, or use a Linux VM."
	@false
else ifeq ($(UNAME_S),Darwin)
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
		MICROKIT_CONFIG=debug \
		GUEST_OS=$(GUEST_OS)
	@echo ""
	@echo "✓ Build complete: $(IMAGE)"
	@echo ""

# internal sentinel: npm install for agentOS console bridge
$(CONSOLE_DIR)/node_modules: $(CONSOLE_DIR)/package.json
	@echo "Installing agentOS console npm dependencies..."
	@cd $(CONSOLE_DIR) && npm install --silent
	@echo "✓ console deps installed"

# =============================================================================
# dashboard: start the agentOS console (agentOS already running on hardware)
# =============================================================================
dashboard: $(CONSOLE_DIR)/node_modules
	@echo ""
	@echo "agentOS console → http://localhost:8795"
	@echo "Connects to agentOS at http://127.0.0.1:8789"
	@echo "Press Ctrl-C to stop."
	@echo ""
	@cd $(CONSOLE_DIR) && node agentos_console.mjs

# =============================================================================
# console (default): build native → QEMU (HW-accel) + agentOS console
#
# Builds agentOS for the host's native CPU, launches it headlessly in QEMU
# with hardware acceleration (HVF on macOS, KVM on Linux), starts the
# agentOS console, and opens it in the default browser.
# Ctrl-C shuts down both the console and QEMU cleanly.
# =============================================================================
console: $(CONSOLE_DIR)/node_modules
	@$(MAKE) build BOARD=$(NATIVE_BOARD) TARGET_ARCH=$(NATIVE_ARCH)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║  agentOS — console (native, HW-accel)    ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@echo "Arch  : $(NATIVE_ARCH)"
	@echo "Board : $(NATIVE_BOARD)"
	@echo "Accel : $(if $(QEMU_ACCEL_NATIVE),$(QEMU_ACCEL_NATIVE),none (TCG fallback))"
	@echo "Image : $(NATIVE_IMAGE)"
	@echo ""
	@echo "Console: http://localhost:8795  (opening in browser...)"
	@echo "──────────────────────────────────────────────"
	@trap 'kill "$$QEMU_PID" 2>/dev/null; exit' INT TERM; \
	 $(NATIVE_QEMU) $(NATIVE_QEMU_FLAGS) & QEMU_PID=$$!; \
	 sleep 0.5; \
	 command -v open     >/dev/null 2>&1 && open     http://localhost:8795 || \
	 command -v xdg-open >/dev/null 2>&1 && xdg-open http://localhost:8795 || true; \
	 cd $(CONSOLE_DIR) && node agentos_console.mjs; \
	 kill "$$QEMU_PID" 2>/dev/null || true

# =============================================================================
# test: CI boot test (exits 0 on success, 1 on failure)
# =============================================================================
test: build
	@BOARD=$(BOARD) bash scripts/run-tests.sh

# =============================================================================
# test-snapshot-sched: standalone unit test for the snapshot_sched PD
# =============================================================================
test-snapshot-sched:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — snapshot_sched unit tests    ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	cc test/test_snapshot_sched.c -o /tmp/test_snapshot_sched -DAGENTOS_SNAPSHOT_SCHED
	@/tmp/test_snapshot_sched
	@echo "✓ snapshot_sched tests passed"
	@echo ""

# =============================================================================
# test-power-mgr: standalone unit test for the power_mgr DVFS thermal model
# =============================================================================
test-power-mgr:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — power_mgr unit tests         ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	cc test/test_power_mgr.c -o /tmp/test_power_mgr
	@/tmp/test_power_mgr
	@echo "✓ power_mgr tests passed"
	@echo ""

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
# release: tag + GitHub release (requires gh CLI and clean working tree)
# =============================================================================
release:
	@bash scripts/release.sh patch

release-minor:
	@bash scripts/release.sh minor

release-major:
	@bash scripts/release.sh major

# =============================================================================
# help
# =============================================================================
help:
	@echo ""
	@echo "agentOS — the OS for agents, by agents"
	@echo ""
	@echo "Targets:"
	@echo "  make                  Build (native arch) + QEMU (HW-accel) + agentOS console"
	@echo "  make dashboard        Start agentOS console only (agentOS running on hardware)"
	@echo "  make deps             Install build deps (brew / apt)"
	@echo "  make test             CI boot test (exit 0/1)"
	@echo "  make test-snapshot-sched"
	@echo "  make test-power-mgr"
	@echo "  make clean            Remove build artifacts"
	@echo "  make clean-all        Remove all build artifacts"
	@echo "  make release          Cut a patch release (tag + GitHub release)"
	@echo "  make release-minor    Cut a minor release"
	@echo "  make release-major    Cut a major release"
	@echo ""
	@echo "Quick start:"
	@echo "  make deps && make"
	@echo ""
