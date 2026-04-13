# agentOS Top-Level Makefile
#
# Quick start:
#   make install && make run
#
# Targets:
#   make install      — install all build dependencies
#   make build        — build the kernel image for BOARD/TARGET_ARCH
#   make run          — build (native arch) + QEMU (HW-accel) + agentOS console (http://localhost:8080)
#   make test         — CI boot test (exit 0/1)
#   make clean        — remove build artifacts for current target

.PHONY: all install deps-tools deps-sdk submodules channels run dashboard test test-snapshot-sched test-power-mgr test-proc-server test-integration clean clean-all clean-images help release release-minor release-major fetch-guest build-tools

# ─── Read config.yaml (if present) ───────────────────────────────────────────
CONFIG_TARGET := $(shell grep '^target_arch:' config.yaml 2>/dev/null | sed 's/target_arch:[[:space:]]*//' | tr -d '[:space:]')
ifeq ($(CONFIG_TARGET),)
  CONFIG_TARGET := riscv64
endif

TARGET_ARCH ?= $(CONFIG_TARGET)
GUEST_OS    ?= none

# ─── Paths (computed FIRST, before any -include changes MAKEFILE_LIST) ───────
# ROOT_DIR must be set before board.mk is included; otherwise
# $(lastword $(MAKEFILE_LIST)) resolves to the board.mk path, not the
# repo root.
ROOT_DIR     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR   := $(ROOT_DIR)kernel/agentos-root-task
MICROKIT_SDK := $(ROOT_DIR)microkit-sdk-2.1.0
CONSOLE_DIR  := $(ROOT_DIR)console

# ─── BOARD_NAME: selects a boards/<name>/board.mk configuration ──────────────
# Derive from TARGET_ARCH when not explicitly provided.  Override with
#   make BOARD_NAME=intel-nuc build
#   make BOARD_NAME=rpi5 build
ifndef BOARD_NAME
  ifeq ($(TARGET_ARCH),aarch64)
    BOARD_NAME := qemu-aarch64
  else ifeq ($(TARGET_ARCH),x86_64)
    BOARD_NAME := qemu-x86_64
  else
    BOARD_NAME := qemu-riscv64
  endif
endif

# Include per-board configuration.  Sets MICROKIT_BOARD, BOARD_ARCH,
# BOARD_NATIVE, BOARD_UART_*, and optional QEMU_* flags.
-include boards/$(BOARD_NAME)/board.mk

# Let board.mk override the Microkit board and arch when present.
ifneq ($(MICROKIT_BOARD),)
  BOARD := $(MICROKIT_BOARD)
endif
ifneq ($(BOARD_ARCH),)
  TARGET_ARCH := $(BOARD_ARCH)
endif

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

# BUILD_DIR and IMAGE depend on BOARD (resolved after board.mk override above)
BUILD_DIR    := $(ROOT_DIR)build/$(BOARD)
IMAGE        := $(BUILD_DIR)/agentos.img

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

# ─── Rust toolchain ──────────────────────────────────────────────────────────
export PATH := $(HOME)/.cargo/bin:$(PATH)

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
  # -cpu host requires KVM/HVF; use cortex-a53 for TCG (matches libvmm reference)
  _NATIVE_CPU       := $(if $(QEMU_ACCEL_NATIVE),host,cortex-a53)
  NATIVE_QEMU_FLAGS  = -machine virt,virtualization=on,highmem=off,secure=off \
                        -cpu $(_NATIVE_CPU) -m 2G \
                        -display none -monitor none \
                        -chardev socket,id=char0,path=/tmp/agentos-serial.sock,server=on,wait=off \
                        -serial chardev:char0 \
                        $(QEMU_ACCEL_NATIVE) \
                        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
                        -device virtio-net-device,netdev=net0 \
                        -device loader,file=$(NATIVE_IMAGE),addr=0x70000000,cpu-num=0
else
  NATIVE_BOARD      := x86_64_generic
  NATIVE_QEMU       := qemu-system-x86_64
  NATIVE_QEMU_FLAGS  = -machine q35 -cpu host -m 2G \
                        -display none -monitor none -serial unix:/tmp/agentos-serial.sock \
                        $(QEMU_ACCEL_NATIVE) \
                        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
                        -device e1000,netdev=net0 \
                        -kernel $(NATIVE_IMAGE)
endif

NATIVE_BUILD_DIR := $(ROOT_DIR)build/$(NATIVE_BOARD)
NATIVE_IMAGE     := $(NATIVE_BUILD_DIR)/agentos.img

channels:
	python3 tools/gen-channels/gen_channels.py

all: run

# =============================================================================
# install: set up build dependencies (alias: deps)
# =============================================================================
install: deps-tools deps-sdk
	@echo ""
	@echo "✅ All dependencies installed! Run 'make run' to launch the console."

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
	@command -v cargo >/dev/null 2>&1 || \
		(echo "[macOS] Installing Rust toolchain..." && \
		 curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path && \
		 echo "[macOS] Rust installed. ✓")
	@rustup target add wasm32-unknown-unknown 2>/dev/null || true
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
		device-tree-compiler \
		curl \
		xz-utils \
		2>/dev/null || true
	@command -v cargo >/dev/null 2>&1 || \
		(curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path)
	@rustup target add wasm32-unknown-unknown 2>/dev/null || true
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
		rust \
		2>/dev/null || true
	@rustup target add wasm32-unknown-unknown 2>/dev/null || true
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
# build-tools: compile all Rust tool binaries in release mode
# =============================================================================
build-tools:
	@echo "Building agentOS Rust tools..."
	@cargo build --release \
		-p gen-sdf -p gen-ringbuf -p sign-wasm -p attest-verify \
		-p make-swap-image -p trace-replay -p agentos-console -p xtask
	@echo "✓ Tools built → target/release/"

# =============================================================================
# fetch-guest: download the guest OS image for GUEST_OS (idempotent)
# =============================================================================
fetch-guest:
ifeq ($(GUEST_OS),freebsd)
	@cargo xtask fetch-guest --os freebsd
else ifeq ($(GUEST_OS),ubuntu)
	@cargo xtask fetch-guest --os ubuntu
endif

# =============================================================================
# build (internal — used by console and test)
# =============================================================================
build: fetch-guest submodules deps-sdk
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
		GUEST_OS=$(GUEST_OS) \
		BOARD_NAME=$(BOARD_NAME) \
		BOARD_NATIVE=$(BOARD_NATIVE) \
		BOARD_UART_PHYS=$(BOARD_UART_PHYS) \
		BOARD_UART_SIZE=$(BOARD_UART_SIZE) \
		BOARD_UART_TYPE=$(BOARD_UART_TYPE) \
		BOARD_UART_IRQ=$(BOARD_UART_IRQ)
	@echo ""
	@echo "✓ Build complete: $(IMAGE)"
	@echo ""

# =============================================================================
# dashboard: start the agentOS console (agentOS already running on hardware)
# =============================================================================
dashboard:
	@echo ""
	@echo "agentOS console → http://localhost:8080"
	@echo "Connects to agentOS at http://127.0.0.1:8789"
	@echo "Press Ctrl-C to stop."
	@echo ""
	@cargo run -p agentos-console --release

# =============================================================================
# run (default): build native → QEMU (HW-accel) + agentOS console
#
# Builds agentOS for the host's native CPU, launches it headlessly in QEMU
# with hardware acceleration (HVF on macOS, KVM on Linux), starts the
# agentOS console server, and opens it in the default browser.
# Ctrl-C shuts down both the console server and QEMU cleanly.
# =============================================================================
run:
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
	@echo "Console: http://localhost:8080  (opening in browser...)"
	@echo "──────────────────────────────────────────────"
	@rm -f /tmp/agentos-serial.sock /tmp/freebsd-serial.sock /tmp/linux-serial.sock
	@lsof -ti:8080 2>/dev/null | xargs kill 2>/dev/null || true
	@lsof -ti:8789 2>/dev/null | xargs kill 2>/dev/null || true
	@trap 'kill "$$QEMU_PID" 2>/dev/null; wait "$$BRIDGE_PID" 2>/dev/null; exit' INT TERM; \
	 cargo run -p agentos-console --release & BRIDGE_PID=$$!; \
	 for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do \
	   [ -S /tmp/agentos-serial.sock ] && break; sleep 0.1; done; \
	 $(NATIVE_QEMU) $(NATIVE_QEMU_FLAGS) & QEMU_PID=$$!; \
	 (command -v open     >/dev/null 2>&1 && open     http://localhost:8080) || \
	 (command -v xdg-open >/dev/null 2>&1 && xdg-open http://localhost:8080) || true; \
	 wait "$$BRIDGE_PID"; \
	 kill "$$QEMU_PID" 2>/dev/null || true

# =============================================================================
# test: CI boot test (exits 0 on success, 1 on failure)
# =============================================================================
test: build
	@cargo xtask test --board $(BOARD) --guest-os $(GUEST_OS)

# =============================================================================
# test-snapshot-sched: standalone unit test for the snapshot_sched PD
# =============================================================================
test-snapshot-sched:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — snapshot_sched unit tests    ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	cc tests/test_snapshot_sched.c -o /tmp/test_snapshot_sched -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST -DAGENTOS_SNAPSHOT_SCHED
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
	cc tests/test_power_mgr.c -o /tmp/test_power_mgr -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST
	@/tmp/test_power_mgr
	@echo "✓ power_mgr tests passed"
	@echo ""

# =============================================================================
# test-proc-server: standalone unit test for the proc_server PD (Track F)
# =============================================================================
test-proc-server:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — proc_server unit tests       ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	cc tests/test_proc_server.c -o /tmp/test_proc_server -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST
	@/tmp/test_proc_server
	@echo "✓ proc_server tests passed"
	@echo ""

# =============================================================================
# test-integration: compile and run C integration tests on the host
#
# Each test file is self-contained: all seL4/Microkit primitives are stubbed
# via #ifdef AGENTOS_TEST_HOST.  No QEMU required.
# =============================================================================
test-integration:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — integration tests (host)     ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@echo "[make] Running integration tests..."
	@for test in \
	    tests/test_quota.c \
	    tests/test_cap_policy_hotreload.c \
	    tests/test_power_mgr.c \
	    tests/test_snapshot_sched.c \
	    tests/test_dev_shell.c \
	    tests/test_proc_server.c; do \
	    gcc -I kernel/agentos-root-task/include \
	        -DAGENTOS_TEST_HOST \
	        -DAGENTOS_SNAPSHOT_SCHED \
	        -DAGENTOS_DEV_SHELL \
	        -o /tmp/agentos_test $$test 2>&1 \
	    && /tmp/agentos_test \
	    && echo "PASS: $$test" \
	    || echo "FAIL: $$test"; \
	done
	@echo ""
	@echo "Integration tests complete."
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

clean-images:
	@echo "Removing cached guest OS images..."
	@rm -f $(ROOT_DIR)guest-images/*.img \
	       $(ROOT_DIR)guest-images/*.qcow2 \
	       $(ROOT_DIR)guest-images/*.raw \
	       $(ROOT_DIR)guest-images/*.fd \
	       $(ROOT_DIR)guest-images/*.xz \
	       /tmp/agentos-serial.sock
	@echo "✓ Guest images removed. Next 'make GUEST_OS=...' will re-download."

# =============================================================================
# release: tag + GitHub release (requires gh CLI and clean working tree)
# =============================================================================
release:
	@cargo xtask release --bump patch

release-minor:
	@cargo xtask release --bump minor

release-major:
	@cargo xtask release --bump major

# =============================================================================
# help
# =============================================================================
help:
	@echo ""
	@echo "agentOS — the OS for agents, by agents"
	@echo ""
	@echo "Targets:"
	@echo "  make install          Install build deps (brew / apt)"
	@echo "  make build            Build kernel image for BOARD/TARGET_ARCH"
	@echo "  make run              Build (native arch) + QEMU (HW-accel) + agentOS console"
	@echo "  make test             CI boot test (exit 0/1)"
	@echo "  make clean            Remove build artifacts for current board"
	@echo ""
	@echo "Quick start:"
	@echo "  make install && make run"
	@echo ""
	@echo "Language breakdown:"
	@echo "  Kernel/firmware    → C"
	@echo "  Arch-specific      → Assembly"
	@echo "  Userspace/tooling  → Rust"
	@echo "  Dashboard UI       → Rust/WASM (trunk build)"
	@echo ""
