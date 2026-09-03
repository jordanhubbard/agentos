# agentOS Top-Level Makefile
#
# Quick start:
#   make help
#   make install && make run
#
# Targets:
#   make help         — show important top-level targets and defaults
#   make install      — install all build dependencies
#   make build        — build the kernel image for BOARD/TARGET_ARCH
#   make run          — build + boot agentOS with Unix guest support in QEMU
#   make test         — CI boot test (exit 0/1)
#   make test-guest-login — prove Ubuntu/FreeBSD serial login via CC-PD
#   make clean        — remove build artifacts for current board

.PHONY: all install deps deps-tools submodules channels run run-fast test test-guest-login sel4-test-image run-tests test-snapshot-sched test-power-mgr test-proc-server test-vibeos-contract test-integration test-host gate gate-aarch64 gate-x86_64 e2e e2e-guest e2e-contract e2e-dual-os e2e-ubuntu-amd64 e2e-ubuntu-arm64 e2e-nixos e2e-freebsd15 e2e-all bootstrap-guest clean clean-all clean-images help release release-minor release-major fetch-guest build-tools

# ─── Read config.yaml (if present) ───────────────────────────────────────────
CONFIG_TARGET := $(shell grep '^target_arch:' config.yaml 2>/dev/null | sed 's/target_arch:[[:space:]]*//' | tr -d '[:space:]')
ifeq ($(CONFIG_TARGET),)
  CONFIG_TARGET := $(shell uname -m | sed 's/arm64/aarch64/')
endif
CONFIG_GUEST_OS := $(shell grep '^guest_os:' config.yaml 2>/dev/null | sed 's/guest_os:[[:space:]]*//' | tr -d '[:space:]')
ifeq ($(CONFIG_GUEST_OS),)
  CONFIG_GUEST_OS := ubuntu
endif

TARGET_ARCH ?= $(CONFIG_TARGET)
GUEST_OS    ?= $(CONFIG_GUEST_OS)
QEMU_TEST_TIMEOUT ?= 300
DUAL_OS_TEST_TIMEOUT ?= 900
QEMU_TEST_GUEST_OS = $(if $(filter x86_64,$(ARCH)),none,$(GUEST_OS))

# ─── Paths (computed FIRST, before any -include changes MAKEFILE_LIST) ───────
# ROOT_DIR must be set before board.mk is included; otherwise
# $(lastword $(MAKEFILE_LIST)) resolves to the board.mk path, not the
# repo root.
ROOT_DIR     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR   := $(ROOT_DIR)kernel/agentos-root-task

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

# Include per-board configuration.  Sets MICROKIT_BOARD (used as BOARD alias),
# BOARD_ARCH, BOARD_NATIVE, BOARD_UART_*, and optional QEMU_* flags.
-include boards/$(BOARD_NAME)/board.mk

# Target/QEMU-backed test gates (agentos-0h4, agentos-45b).
-include mk/target-tests.mk

# Let board.mk override the board name and arch when present.
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

SEL4_PROFILE ?= release

# BUILD_DIR and IMAGE depend on BOARD (resolved after board.mk override above)
BUILD_DIR    := $(ROOT_DIR)build/$(BOARD)
IMAGE        := $(BUILD_DIR)/agentos.img
AGENTOS_IMAGES ?= $(ROOT_DIR)build/guest-images
BUILD_TMP_DIR := $(ROOT_DIR)build/tmp

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

# ─── Rust toolchain ──────────────────────────────────────────────────────────
export PATH := $(HOME)/.cargo/bin:$(PATH)

# ─── Native arch / HW-accelerated QEMU ────────────────────────────────────
# Normalise uname -m: macOS Apple Silicon reports "arm64", seL4 uses "aarch64"
NATIVE_ARCH := $(shell uname -m | sed 's/arm64/aarch64/')

ifeq ($(UNAME_S),Darwin)
  ifeq ($(NATIVE_ARCH),aarch64)
    # HVF on Apple Silicon has irrecoverable assertion failures with seL4's
    # aarch64 memory access patterns (hvf_vcpu_exec isv assertion, hvf.c).
    # Use TCG (software emulation) until this is resolved upstream in QEMU.
    #
    # Quarterly retest tracker — agentos-3jn.
    # Last reviewed: 2026-06-07
    #   Host:  Apple M-series, macOS 26.5.1 (Darwin 25.5.0)
    #   QEMU:  11.0.1 (Homebrew)
    # Status: workaround RETAINED. The upstream hvf_vcpu_exec "isv" assertion
    #   has no fix landed in QEMU 11.0.x, so -accel hvf stays disabled for
    #   aarch64 on Darwin. NOTE: an actual end-to-end seL4+HVF boot was NOT
    #   re-run on this date (full seL4 build/boot was out of scope for the
    #   config pass); a fresh boot test under HVF is still PENDING before this
    #   workaround can be removed. Next retest: ~2026-09.
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
  NATIVE_LOADER_ELF = $(NATIVE_BUILD_DIR)/loader.elf
  NATIVE_QEMU_FLAGS  = -machine virt,virtualization=on,highmem=off,secure=off \
                        -cpu $(_NATIVE_CPU) -m 2G \
                        -display none -monitor none \
                        -global virtio-mmio.force-legacy=off \
                        -chardev socket,id=char0,path=$(ROOT_DIR)build/agentos-serial.sock,server=on,wait=off \
                        -serial chardev:char0 \
                        -chardev socket,id=cc_pd_char,path=$(ROOT_DIR)build/cc_pd.sock,server=on,wait=off \
                        -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0 \
                        -device virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0 \
                        $(QEMU_ACCEL_NATIVE) \
                        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
                        -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0 \
                        -device loader,file=$(NATIVE_LOADER_ELF),cpu-num=0 \
                        -device loader,file=$(NATIVE_IMAGE),addr=0x48000000
else
  NATIVE_BOARD      := x86_64_generic
  NATIVE_QEMU       := qemu-system-x86_64
  NATIVE_QEMU_FLAGS  = -machine q35 -cpu host -m 2G \
                        -display none -monitor none -serial unix:$(ROOT_DIR)build/agentos-serial.sock \
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
install: deps-tools
	@echo ""
	@echo "✅ All dependencies installed! Run 'make run' to start."

deps: install

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
	@echo "[deps-tools] Building xtask..."
	@cargo build -p xtask 2>/dev/null || true
	@echo "  xtask:               $$(cargo run -p xtask -- --version 2>/dev/null || echo 'built')"

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
		-p make-swap-image -p trace-replay -p xtask
	@echo "✓ Tools built → target/release/"

# =============================================================================
# fetch-guest: download the guest OS image for GUEST_OS (idempotent)
# =============================================================================
fetch-guest:
ifeq ($(GUEST_OS),freebsd)
	@cargo xtask fetch-guest --os freebsd --output-dir $(AGENTOS_IMAGES)
else ifeq ($(GUEST_OS),ubuntu)
	@cargo xtask fetch-guest --os ubuntu --output-dir $(AGENTOS_IMAGES)
else ifeq ($(GUEST_OS),both)
	@cargo xtask fetch-guest --os ubuntu --output-dir $(AGENTOS_IMAGES)
	@cargo xtask fetch-guest --os freebsd --output-dir $(AGENTOS_IMAGES)
endif

# =============================================================================
# build (internal — used by test)
# =============================================================================
build: fetch-guest submodules
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
	@mkdir -p $(BUILD_DIR)
	@PATH="$(LLVM_BIN):$(LLD_BIN):$$PATH" $(MAKE) -C $(KERNEL_DIR) build \
		BUILD_DIR=$(BUILD_DIR) \
		AGENTOS_BOARD=$(BOARD) \
		AGENTOS_ARCH=$(ARCH) \
		SEL4_PROFILE=$(SEL4_PROFILE) \
		AGENTOS_FREEBSD_IMAGE=$(if $(AGENTOS_FREEBSD_IMAGE),$(AGENTOS_FREEBSD_IMAGE),$(FREEBSD_IMAGE)) \
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

# QEMU flags for interactive run: serial → stdio, SSH port forwarding per guest.
#
# QEMU_FAST=1 enables TCG-mode dev-iteration tweaks on Apple Silicon, where
# HVF is not usable with seL4 and we are stuck on software emulation:
#   -cpu max               richer feature set than cortex-a53; some hot paths
#                          dispatch to faster TCG helpers
#   -accel tcg,thread=multi  spread translation across host cores
# These flags only kick in when no hardware accelerator is available
# (QEMU_ACCEL_NATIVE empty), so passing QEMU_FAST=1 on a Linux/KVM host or
# x86_64/HVF host is harmless.
ifeq ($(QEMU_FAST),1)
  ifeq ($(QEMU_ACCEL_NATIVE),)
    _RUN_CPU := max
    _QEMU_FAST_FLAGS := -accel tcg,thread=multi
  else
    _RUN_CPU := $(if $(filter aarch64,$(NATIVE_ARCH)),cortex-a53,qemu64)
    _QEMU_FAST_FLAGS :=
  endif
else
  _RUN_CPU := $(if $(filter aarch64,$(NATIVE_ARCH)),cortex-a53,qemu64)
  _QEMU_FAST_FLAGS :=
endif
FREEBSD_IMAGE ?= $(if $(AGENTOS_FREEBSD_IMAGE),$(AGENTOS_FREEBSD_IMAGE),$(AGENTOS_IMAGES)/freebsd-15.0-aarch64.iso)
_UBUNTU_BLK = -drive file=$(AGENTOS_IMAGES)/ubuntu-26.04-aarch64.iso,format=raw,if=none,id=ubuntu_hd,readonly=on,file.locking=off \
              -device virtio-blk-device,drive=ubuntu_hd,bus=virtio-mmio-bus.1
_FREEBSD_BLK = -drive file=$(FREEBSD_IMAGE),format=raw,if=none,id=freebsd_hd,readonly=on,file.locking=off \
               -device virtio-blk-device,drive=freebsd_hd,bus=virtio-mmio-bus.31
# Outer QEMU block devices, selected by GUEST_OS.  Note: GUEST_OS=buildroot (and
# GUEST_OS=none) intentionally attach NO outer disk — the buildroot Linux kernel
# + initrd are packaged inside linux_vmm.elf by vmm.mk (BUILDROOT_LINUX_IMAGE /
# BUILDROOT_INITRD_IMAGE), so the guest boots entirely from the inner libvmm
# image with no host-provided ISO.  Only ubuntu/freebsd need an outer virtio-blk.
_QEMU_BLK_FLAGS = $(if $(filter both,$(GUEST_OS)),$(_UBUNTU_BLK) $(_FREEBSD_BLK),$(if $(filter ubuntu,$(GUEST_OS)),$(_UBUNTU_BLK),$(if $(filter freebsd,$(GUEST_OS)),$(_FREEBSD_BLK),)))
QEMU_RUN_MEM ?= $(if $(filter both,$(GUEST_OS)),3G,2G)
QEMU_RUN_SMP ?= $(if $(filter smp-% smp,$(SEL4_PROFILE)),4,1)
comma := ,
QEMU_MACHINE_FLAGS_BASE := virt$(comma)virtualization=on$(comma)highmem=off$(comma)secure=off
QEMU_MACHINE_FLAGS := $(QEMU_MACHINE_FLAGS_BASE)$(if $(filter freebsd both,$(GUEST_OS)),$(comma)acpi=off)
QEMU_RUN_FLAGS = -machine $(QEMU_MACHINE_FLAGS) \
                 -cpu $(_RUN_CPU) -m $(QEMU_RUN_MEM) \
                 -smp $(QEMU_RUN_SMP) \
                 $(_QEMU_FAST_FLAGS) \
                 -display none -monitor none \
                 -global virtio-mmio.force-legacy=off \
                 -serial stdio \
                 -chardev socket,id=cc_pd_char,path=$(ROOT_DIR)build/cc_pd.sock,server=on,wait=off \
                 -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0 \
                 -device virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0 \
                 -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22,hostfwd=tcp:127.0.0.1:2223-10.0.2.15:2223,hostfwd=tcp:127.0.0.1:2224-10.0.2.15:2224 \
                 -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0,ctrl_vq=off,ctrl_rx=off,ctrl_vlan=off,guest_announce=off,mq=off,ctrl_mac_addr=off,ctrl_guest_offloads=off \
                 $(_QEMU_BLK_FLAGS) \
                 -device loader,file=$(NATIVE_LOADER_ELF),cpu-num=0 \
                 -device loader,file=$(NATIVE_IMAGE),addr=0x48000000

# run (default): build native → QEMU with serial on stdout and a Unix guest
# =============================================================================
run:
	@$(MAKE) build BOARD=$(NATIVE_BOARD) TARGET_ARCH=$(NATIVE_ARCH)
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║  agentOS — QEMU ($(NATIVE_ARCH))         ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@echo "Arch   : $(NATIVE_ARCH)"
	@echo "Board  : $(NATIVE_BOARD)"
	@echo "Accel  : $(if $(QEMU_ACCEL_NATIVE),$(QEMU_ACCEL_NATIVE),$(if $(filter 1,$(QEMU_FAST)),tcg multi-thread + cpu max,none (TCG)))"
	@echo "Memory : $(QEMU_RUN_MEM)"
	@echo "Guest  : $(GUEST_OS)"
	@echo "Image  : $(NATIVE_IMAGE)"
	@echo "CC-PD  : $(ROOT_DIR)build/cc_pd.sock"
	@echo "GUI    : cd $(abspath $(ROOT_DIR)../agentos_gui) && make run"
	@echo ""
	@echo "Guest SSH: ssh -p 2222 ubuntu@localhost    (Ubuntu)"
	@echo "           ssh -p 2223 root@localhost      (FreeBSD)"
	@echo "           ssh -p 2224 root@localhost      (NixOS)"
	@echo "Buildroot: no outer ISO; Linux runs inside linux_vmm.elf → '#' shell on serial"
	@echo "Exit QEMU: Ctrl-A X"
	@echo "──────────────────────────────────────────────"
	@$(NATIVE_QEMU) $(QEMU_RUN_FLAGS)

# run-fast: same as run, with TCG-mode performance knobs enabled.
# On Apple Silicon (TCG-only because HVF is incompatible with seL4) this
# adds -accel tcg,thread=multi and switches the CPU model to 'max', giving
# a noticeable boot-time speedup for dev iteration.  On Linux/KVM hosts
# QEMU_FAST is a no-op since hardware acceleration is already in use.
run-fast:
	@$(MAKE) run QEMU_FAST=1

# =============================================================================
# test: CI boot test (exits 0 on success, 1 on failure)
# =============================================================================
test: build
	@AGENTOS_FREEBSD_IMAGE="$(FREEBSD_IMAGE)" cargo xtask qemu-test --board $(BOARD) --guest-os $(QEMU_TEST_GUEST_OS) --timeout-secs $(QEMU_TEST_TIMEOUT)

# =============================================================================
# gate: MANDATORY dual-arch target/QEMU quality gate.
#
# This is the gate that MUST pass before any OS-level behavior may be claimed
# "complete" / "boot-proven" in README, DESIGN, PLAN, or a release.  It runs the
# real seL4 target build + QEMU boot test on BOTH supported architectures with
# GUEST_OS=none, exactly as required by agentos-46q.
#
#   HOST-ONLY tests (test-integration / test-host): compile C suites with
#     -DAGENTOS_TEST_HOST and run them on the build host.  They exercise logic
#     but stub out seL4 IPC, so per the PLAN priority rules they are NOT proof
#     of production OS behavior — they are a fast pre-filter only.
#
#   TARGET / QEMU-BACKED tests (gate-aarch64 / gate-x86_64): build the real
#     seL4 image for the board and boot it under QEMU via `make test`.  These,
#     and only these, may back an OS-level completion claim.
#
# Usage:
#   make gate                  # run BOTH target arches (the release gate)
#   make gate-aarch64          # single arch, target/QEMU-backed
#   make gate-x86_64           # single arch, target/QEMU-backed
gate-aarch64:
	@echo ""
	@echo "── [GATE] TARGET/QEMU test: aarch64 (GUEST_OS=none) ──────────"
	@$(MAKE) test TARGET_ARCH=aarch64 GUEST_OS=none

gate-x86_64:
	@echo ""
	@echo "── [GATE] TARGET/QEMU test: x86_64 (GUEST_OS=none) ───────────"
	@$(MAKE) test TARGET_ARCH=x86_64 GUEST_OS=none

gate: test-host gate-aarch64 gate-x86_64
	@echo ""
	@echo "╔══════════════════════════════════════════════════════════╗"
	@echo "║  ✅ DUAL-ARCH GATE PASSED                                 ║"
	@echo "║  Host-only suite + aarch64 + x86_64 QEMU boot tests OK.   ║"
	@echo "║  OS-level completion claims are now permitted.           ║"
	@echo "╚══════════════════════════════════════════════════════════╝"
	@echo ""

# test-host: alias for the host-only integration suite.  Named explicitly so
# callers and CI cannot mistake host-only coverage for target/QEMU proof.
test-host: test-integration

sel4-test-image:
	@$(MAKE) build \
		BOARD=$(BOARD) \
		TARGET_ARCH=$(ARCH) \
		BOARD_NAME=$(BOARD_NAME) \
		BUILD_DIR=$(ROOT_DIR)build/$(BOARD)-test \
		GUEST_OS=none \
		SEL4_TEST_IMAGE=1
	@echo "✓ seL4 target TAP image: $(ROOT_DIR)build/$(BOARD)-test/agentos.img"

run-tests:
	@cargo xtask run-tests --board $(BOARD) --timeout-secs $(QEMU_TEST_TIMEOUT)

# Build and boot each supported full guest OS, then prove the CC-PD API can
# drain the serial console to a login prompt and inject input that the guest
# echoes. This is the headless proof behind the GUI console.
test-guest-login:
	@cargo xtask qemu-test --board $(BOARD) --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT)
	@cargo xtask qemu-test --board $(BOARD) --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT)

# =============================================================================
# test-snapshot-sched: standalone unit test for the snapshot_sched PD
# =============================================================================
test-snapshot-sched:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — snapshot_sched unit tests    ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@mkdir -p $(BUILD_TMP_DIR)
	cc tests/test_snapshot_sched.c -o $(BUILD_TMP_DIR)/test_snapshot_sched -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST -DAGENTOS_SNAPSHOT_SCHED
	@$(BUILD_TMP_DIR)/test_snapshot_sched
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
	@mkdir -p $(BUILD_TMP_DIR)
	cc tests/test_power_mgr.c -o $(BUILD_TMP_DIR)/test_power_mgr -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST
	@$(BUILD_TMP_DIR)/test_power_mgr
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
	@mkdir -p $(BUILD_TMP_DIR)
	cc tests/test_proc_server.c -o $(BUILD_TMP_DIR)/test_proc_server -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST
	@$(BUILD_TMP_DIR)/test_proc_server
	@echo "✓ proc_server tests passed"
	@echo ""

# =============================================================================
# test-vibeos-contract: standalone contract tests for the VibeOS lifecycle API
# =============================================================================
test-vibeos-contract:
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║   agentOS — VibeOS contract tests        ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@mkdir -p $(BUILD_TMP_DIR)
	cc tests/vibe/test_vibeos_contract.c -o $(BUILD_TMP_DIR)/test_vibeos_contract -I tests -I kernel/agentos-root-task/include -DAGENTOS_TEST_HOST
	@$(BUILD_TMP_DIR)/test_vibeos_contract
	@echo "✓ vibeos contract tests passed"
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
	@mkdir -p $(BUILD_TMP_DIR)
	@status=0; \
	for test in \
	    tests/test_quota.c \
	    tests/test_cap_policy_hotreload.c \
	    tests/test_power_mgr.c \
	    tests/test_snapshot_sched.c \
	    tests/test_proc_server.c \
	    tests/test_serial_pd.c \
	    tests/test_framebuffer_pd.c \
	    tests/test_guest_contract.c \
	    tests/test_vm_multi_guest.c \
	    tests/test_e13_agent_boot.c \
	            tests/vibe/test_vibeos_contract.c; do \
	    if gcc -I tests \
	        -I kernel/agentos-root-task/include \
	        -DAGENTOS_TEST_HOST \
	        -DAGENTOS_SNAPSHOT_SCHED \
	        $$test \
	        -o $(BUILD_TMP_DIR)/agentos_test 2>&1 \
	    && $(BUILD_TMP_DIR)/agentos_test; then \
	        echo "PASS: $$test"; \
	    else \
	        echo "FAIL: $$test"; \
	        status=1; \
	    fi; \
	done; \
	if gcc -I platform/include \
	        tests/platform/test_net_virt_pump.c \
	        platform/net-virt/net_virt_pump.c \
	        -o $(BUILD_TMP_DIR)/test_net_virt_pump 2>&1 \
	    && $(BUILD_TMP_DIR)/test_net_virt_pump; then \
	    echo "PASS: tests/platform/test_net_virt_pump.c"; \
	else \
	    echo "FAIL: tests/platform/test_net_virt_pump.c"; \
	    status=1; \
	fi; \
	if gcc -I platform/include \
	        tests/platform/test_inspect_snapshot.c \
	        platform/inspect/inspect_snapshot.c \
	        -o $(BUILD_TMP_DIR)/test_inspect_snapshot 2>&1 \
	    && $(BUILD_TMP_DIR)/test_inspect_snapshot; then \
	    echo "PASS: tests/platform/test_inspect_snapshot.c"; \
	else \
	    echo "FAIL: tests/platform/test_inspect_snapshot.c"; \
	    status=1; \
	fi; \
	echo ""; \
	echo "Integration tests complete."; \
	echo ""; \
	exit $$status

# =============================================================================
# e2e: End-to-end integration test suite (QEMU + guest VMs + SSH)
# =============================================================================
# Requires: make build BOARD=$(BOARD) && make fetch-guest
# Exit code 2 = SKIP (prerequisites not met — QEMU or images missing)
e2e: build
	@chmod +x tests/e2e/run_e2e.sh tests/e2e/*.sh
	@bash tests/e2e/run_e2e.sh

e2e-guest:
	@chmod +x tests/e2e/suite_common.sh
	@bash tests/e2e/suite_common.sh

e2e-contract:
	@chmod +x tests/e2e/test_cc_contract.sh
	@BRIDGE_AVAILABLE=1 bash tests/e2e/test_cc_contract.sh

e2e-dual-os:
	@cargo xtask qemu-test --board $(BOARD) --guest-os both --timeout-secs $(DUAL_OS_TEST_TIMEOUT)

# Per-guest-OS E2E targets — run the full suite against a specific guest image.
# Images must exist in build/guest-images/; create them with: make bootstrap-guest OS=<os>
e2e-ubuntu-amd64:
	@chmod +x tests/e2e/run_e2e.sh tests/e2e/*.sh
	@E2E_GUEST_OS=ubuntu-amd64 bash tests/e2e/run_e2e.sh

e2e-ubuntu-arm64:
	@chmod +x tests/e2e/run_e2e.sh tests/e2e/*.sh
	@E2E_GUEST_OS=ubuntu-arm64 bash tests/e2e/run_e2e.sh

e2e-nixos:
	@chmod +x tests/e2e/run_e2e.sh tests/e2e/*.sh
	@E2E_GUEST_OS=nixos bash tests/e2e/run_e2e.sh

e2e-freebsd15:
	@chmod +x tests/e2e/run_e2e.sh tests/e2e/*.sh
	@E2E_GUEST_OS=freebsd15 bash tests/e2e/run_e2e.sh

# e2e-all: run E2E suite for every guest image that exists in build/guest-images/
e2e-all:
	@chmod +x tests/e2e/run_e2e.sh tests/e2e/*.sh
	@failed=0; \
	for gos in freebsd ubuntu-amd64 ubuntu-arm64 nixos freebsd15; do \
	    img=""; \
	    case "$$gos" in \
	        freebsd)       img="$(AGENTOS_IMAGES)/freebsd-15.0-aarch64.iso" ;; \
	        ubuntu-amd64)  img="$(AGENTOS_IMAGES)/ubuntu-amd64.img" ;; \
	        ubuntu-arm64)  img="$(AGENTOS_IMAGES)/ubuntu-26.04-aarch64.iso" ;; \
	        nixos)         img="$(AGENTOS_IMAGES)/nixos.img" ;; \
	        freebsd15)     img="$(AGENTOS_IMAGES)/freebsd15-amd64.img" ;; \
	    esac; \
	    if [ -f "$$img" ]; then \
	        echo ""; echo "══ E2E: $$gos ══"; \
	        E2E_GUEST_OS=$$gos E2E_GUEST_IMG=$$img bash tests/e2e/run_e2e.sh || failed=$$((failed+1)); \
	    else \
	        echo "[SKIP] $$gos: image not found ($$img)"; \
	    fi; \
	done; \
	[ "$$failed" -eq 0 ] || (echo ""; echo "$$failed guest OS(es) failed E2E"; exit 1)

# bootstrap-guest: create a guest disk image from installer ISOs.
# ISOs are cached in $$AGENTOS_ISO_DIR (default ~/.cache/agentos/isos)
# and auto-downloaded from the vendor's official site on cache miss.
# Usage: make bootstrap-guest OS=nixos
#        make bootstrap-guest OS=ubuntu-amd64
bootstrap-guest:
	@chmod +x tools/bootstrap-guest.sh
	@[ -n "$(OS)" ] || (echo "Usage: make bootstrap-guest OS=<ubuntu-amd64|ubuntu-arm64|nixos|freebsd15>"; exit 1)
	@bash tools/bootstrap-guest.sh $(OS)

# =============================================================================
# clean
# =============================================================================
clean:
	@echo "Cleaning build artifacts for $(BOARD)..."
	@rm -rf $(BUILD_DIR)
	@rm -rf $(ROOT_DIR)libvmm/arch $(ROOT_DIR)libvmm/util $(ROOT_DIR)libvmm/virtio
	@rm -f  $(ROOT_DIR)libvmm/guest.d $(ROOT_DIR)libvmm/guest.o
	@rm -rf $(ROOT_DIR)util
	@rm -f  $(ROOT_DIR).libvmm_cflags.*
	@rm -f  $(KERNEL_DIR)/report.txt
	@rm -f  $(ROOT_DIR)build/cc_pd.sock $(ROOT_DIR)build/agentos-serial.sock
	@echo "✓ Clean."

clean-all:
	@echo "Cleaning all build artifacts..."
	@rm -rf $(ROOT_DIR)build
	@rm -rf $(ROOT_DIR)libvmm/arch $(ROOT_DIR)libvmm/util $(ROOT_DIR)libvmm/virtio
	@rm -f  $(ROOT_DIR)libvmm/guest.d $(ROOT_DIR)libvmm/guest.o
	@rm -rf $(ROOT_DIR)util
	@rm -f  $(ROOT_DIR).libvmm_cflags.*
	@rm -f  $(KERNEL_DIR)/report.txt
	@echo "✓ Clean."

clean-images:
	@echo "Removing guest OS image cache: $(AGENTOS_IMAGES)"
	@rm -rf $(AGENTOS_IMAGES)
	@echo "✓ Done. Re-fetch with: make fetch-guest GUEST_OS=ubuntu|freebsd"

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
	@echo "agentOS - top-level make targets"
	@echo ""
	@echo "Usage:"
	@echo "  make <target> [TARGET_ARCH=aarch64|x86_64|riscv64] [GUEST_OS=buildroot|ubuntu|freebsd|both|none]"
	@echo ""
	@echo "Current defaults:"
	@echo "  TARGET_ARCH     $(TARGET_ARCH)"
	@echo "  BOARD_NAME      $(BOARD_NAME)"
	@echo "  BOARD           $(BOARD)"
	@echo "  GUEST_OS        $(GUEST_OS)"
	@echo "  QEMU_RUN_MEM    $(QEMU_RUN_MEM)"
	@echo "  BUILD_DIR       $(BUILD_DIR)"
	@echo "  AGENTOS_IMAGES  $(AGENTOS_IMAGES)"
	@echo ""
	@echo "Primary targets:"
	@echo "  make help             Show this help text"
	@echo "  make install          Install host build dependencies (alias: make deps)"
	@echo "  make build            Fetch the selected guest image and build agentOS"
	@echo "  make run              Build native agentOS and boot QEMU with CC-PD socket"
	@echo "                        Uses QEMU_RUN_MEM=3G automatically for GUEST_OS=both"
	@echo "  make run GUEST_OS=buildroot"
	@echo "                        Boot linux_vmm hosting buildroot Linux to a '#' prompt"
	@echo "                        (no outer ISO; guest is packaged inside linux_vmm.elf)"
	@echo "  make run-fast         Same as run, plus TCG perf knobs (cpu max + multi-thread)"
	@echo "                        No-op on Linux/KVM hosts where HW accel is already on"
	@echo "                        Recommended dev loop on Apple Silicon:"
	@echo "                        make run-fast GUEST_OS=buildroot"
	@echo "  make test             Build and run the QEMU boot/API smoke test"
	@echo "  make gate             MANDATORY dual-arch QEMU gate before OS-level claims"
	@echo "                        (host suite + aarch64 + x86_64 GUEST_OS=none boot tests)"
	@echo "  make test-guest-login Boot Ubuntu and FreeBSD to an interactive serial prompt"
	@echo ""
	@echo "Guest images:"
	@echo "  make fetch-guest GUEST_OS=ubuntu     Stage Ubuntu 26.04 assets in build/guest-images"
	@echo "  make fetch-guest GUEST_OS=freebsd    Stage FreeBSD 15.0 assets in build/guest-images"
	@echo "  make fetch-guest GUEST_OS=both       Stage both Ubuntu and FreeBSD assets"
	@echo "  make bootstrap-guest OS=<name>       Build guest disks from cached or downloaded ISOs"
	@echo "                                      names: ubuntu-amd64 ubuntu-arm64 nixos freebsd15"
	@echo ""
	@echo "Test targets:"
	@echo "  make gate             MANDATORY dual-arch gate (target/QEMU, both arches)"
	@echo "  make gate-aarch64     Target/QEMU boot test: aarch64 GUEST_OS=none"
	@echo "  make gate-x86_64      Target/QEMU boot test: x86_64 GUEST_OS=none"
	@echo "  make sel4-test-image  Build the seL4-target TAP test image"
	@echo "  make run-tests        Run the seL4-target TAP test image in QEMU"
	@echo "  make test-host        Host-only suite (alias of test-integration; NOT OS proof)"
	@echo "  make test-integration Run host-side contract/integration tests"
	@echo "  make e2e              Run the default QEMU/guest/CC end-to-end suite"
	@echo "  make e2e-dual-os      Run Ubuntu and FreeBSD guest E2E coverage"
	@echo "  make e2e-all          Run E2E suites for every staged guest image"
	@echo ""
	@echo "Cleanup/tooling:"
	@echo "  make clean            Remove build artifacts for the selected board"
	@echo "  make clean-all        Remove all build artifacts under build/"
	@echo "  make clean-images     Remove staged guest images"
	@echo "  make build-tools      Build Rust host tools in release mode"
	@echo ""
	@echo "Quick start:"
	@echo "  make install && make run"
	@echo ""
	@echo "Common examples:"
	@echo "  make build TARGET_ARCH=aarch64 GUEST_OS=ubuntu"
	@echo "  make build TARGET_ARCH=aarch64 GUEST_OS=both"
	@echo "  make run GUEST_OS=freebsd"
	@echo "  make run-fast GUEST_OS=buildroot   # fast dev loop on Apple Silicon"
	@echo "  make gate                          # full release gate, both arches"
	@echo "  make test-guest-login QEMU_TEST_TIMEOUT=420"
	@echo "  cd ../agentos_gui && make run"
	@echo ""
