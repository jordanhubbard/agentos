# agentOS Top-Level Makefile
#
# Quick start:
#   make setup
#   make demo
#
# Targets:
#   make help         — show important top-level targets and defaults
#   make setup        — install dependencies and the shared Microkit SDK
#   make demo         — prove and retain Ubuntu + FreeBSD for authenticated SSH
#   make demo-test    — run the same dual-guest proof non-interactively
#   make demo-desktop — prove and retain a tunnel-confined Ubuntu VNC desktop
#   make demo-desktop-test — run the desktop protocol/frame proof and exit
#   make demo-smoke   — run the fast host-only preflight suite
#   make install      — install all build dependencies
#   make build        — build the kernel image for BOARD/TARGET_ARCH
#   make run          — build + boot agentOS with Unix guest support in QEMU
#   make test         — CI boot test (exit 0/1)
#   make test-guest-login — prove Ubuntu/FreeBSD serial login via CC-PD
#   make test-guest-net   — prove one packet through emulated virtio-net
#   make test-guest-blk   — prove one request through emulated virtio-blk
#   make test-guest-console — prove Ubuntu login through emulated virtio-console
#   make test-ubuntu-virtio — require Ubuntu on agentOS net + blk + console
#   make test-ubuntu-live — boot the full Ubuntu Casper live filesystem
#   make clean        — remove build artifacts for current board

.DEFAULT_GOAL := help

.PHONY: all setup sdk demo demo-check demo-smoke demo-test demo-desktop demo-desktop-test demo-clean install deps deps-tools submodules channels format policy-check guest-profile-check lint-source run run-fast run-dual-ssh test test-guest-login test-guest-net test-guest-blk test-guest-console test-ubuntu-virtio test-ubuntu-live test-guest-boot-timing-compare sel4-test-image run-tests test-snapshot-sched test-proc-server test-vibeos-contract test-integration test-host gate gate-aarch64 gate-x86_64 gate-x86_64-vtx e2e e2e-guest e2e-contract e2e-dual-os e2e-ubuntu-amd64 e2e-ubuntu-arm64 e2e-nixos e2e-freebsd15 e2e-all bootstrap-guest clean clean-all clean-images help release release-minor release-major release-prepare release-check release-publish release-verify presentation-render fetch-guest build-tools

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
# Canonical build selectors. GUEST_OS remains a compatibility spelling and is
# resolved through profile aliases; lower layers receive only profile paths and
# slot policy. The legacy `both` spelling resolves the bounded release scenario.
GUEST_PROFILE ?=
GUEST_SCENARIO ?=
GUEST_PRIMARY_PROFILE ?=
GUEST_SECONDARY_PROFILE ?=
_profile_for_alias = $(strip $(shell cargo xtask guest-profile --resolve-alias $(1)))
_profile_control_type = $(strip $(shell cargo xtask guest-profile --profile $(1) --print-control-type))
_profile_ram_size = $(strip $(shell cargo xtask guest-profile --profile $(1) --placement $(2) --print-ram-size))
_scenario_profile = $(strip $(shell cargo xtask guest-scenario --alias $(1) --control-type $(2)))
ifneq ($(strip $(GUEST_SCENARIO)),)
  ifneq ($(strip $(GUEST_PROFILE)$(GUEST_PRIMARY_PROFILE)$(GUEST_SECONDARY_PROFILE)),)
    $(error GUEST_SCENARIO cannot be combined with profile selectors)
  endif
  _SELECTED_GUEST_SCENARIO := $(GUEST_SCENARIO)
else ifneq ($(strip $(GUEST_PROFILE)),)
  ifneq ($(strip $(GUEST_PRIMARY_PROFILE)$(GUEST_SECONDARY_PROFILE)),)
    $(error GUEST_PROFILE cannot be combined with explicit slot profiles)
  endif
  _SELECTED_GUEST_PROFILE := $(GUEST_PROFILE)
else ifneq ($(strip $(GUEST_PRIMARY_PROFILE)$(GUEST_SECONDARY_PROFILE)),)
  # Explicit slot composition is already canonical; ignore the legacy default.
else ifeq ($(GUEST_OS),both)
  _SELECTED_GUEST_SCENARIO := both
else ifneq ($(GUEST_OS),none)
  _SELECTED_GUEST_PROFILE := $(call _profile_for_alias,$(GUEST_OS))
  ifeq ($(_SELECTED_GUEST_PROFILE),)
    $(error unknown guest profile alias GUEST_OS=$(GUEST_OS); use GUEST_PROFILE=<profile.toml>)
  endif
endif
ifneq ($(strip $(_SELECTED_GUEST_SCENARIO)),)
  GUEST_PRIMARY_PROFILE := $(call _scenario_profile,$(_SELECTED_GUEST_SCENARIO),1)
  GUEST_SECONDARY_PROFILE := $(call _scenario_profile,$(_SELECTED_GUEST_SCENARIO),2)
  ifeq ($(strip $(GUEST_PRIMARY_PROFILE)),)
    $(error scenario $(_SELECTED_GUEST_SCENARIO) has no primary profile)
  endif
  ifeq ($(strip $(GUEST_SECONDARY_PROFILE)),)
    $(error scenario $(_SELECTED_GUEST_SCENARIO) has no secondary profile)
  endif
endif
ifneq ($(strip $(_SELECTED_GUEST_PROFILE)),)
  _SELECTED_CONTROL_TYPE := $(call _profile_control_type,$(_SELECTED_GUEST_PROFILE))
  ifeq ($(_SELECTED_CONTROL_TYPE),1)
    GUEST_PRIMARY_PROFILE := $(_SELECTED_GUEST_PROFILE)
  else ifeq ($(_SELECTED_CONTROL_TYPE),2)
    GUEST_SECONDARY_PROFILE := $(_SELECTED_GUEST_PROFILE)
  else
    $(error profile $(_SELECTED_GUEST_PROFILE) has unsupported control type $(_SELECTED_CONTROL_TYPE))
  endif
endif
ifneq ($(strip $(GUEST_PRIMARY_PROFILE)),)
  _GUEST_PRIMARY_PLACEMENT := $(if $(strip $(GUEST_SECONDARY_PROFILE)),dual-primary,default)
  _GUEST_PRIMARY_RAM_SIZE := $(call _profile_ram_size,$(GUEST_PRIMARY_PROFILE),$(_GUEST_PRIMARY_PLACEMENT))
  ifeq ($(_GUEST_PRIMARY_RAM_SIZE),)
    $(error profile $(GUEST_PRIMARY_PROFILE) has no executable $(_GUEST_PRIMARY_PLACEMENT) RAM plan)
  endif
  GUEST_PRIMARY_LARGE ?= $(shell test "$(_GUEST_PRIMARY_RAM_SIZE)" -gt 536870912 && echo 1 || echo 0)
else
  GUEST_PRIMARY_LARGE ?= 0
endif
QEMU_TEST_TIMEOUT ?= 300
# Console and live-media proofs may run beside a retained guest instance.
# Zero keeps the profile's normal forwarding port.
QEMU_TEST_SSH_PORT ?= 0
QEMU_TEST_GPU_SSH_PORT ?= 12224
GUEST_LINUX_CC ?= aarch64-linux-gnu-gcc
# Correct suspend accounting freezes each guest's architectural time while it
# is stopped.  A full vendor-live-media dual proof can therefore take longer
# than the old 90-minute bound that accidentally included a clock jump.
DUAL_OS_TEST_TIMEOUT ?= 7200
DESKTOP_TEST_TIMEOUT ?= 3600
QEMU_TEST_GUEST_OS = $(if $(filter x86_64,$(ARCH)),none,$(GUEST_OS))

# ─── Paths (computed FIRST, before any -include changes MAKEFILE_LIST) ───────
# ROOT_DIR must be set before board.mk is included; otherwise
# $(lastword $(MAKEFILE_LIST)) resolves to the board.mk path, not the
# repo root.
ROOT_DIR     := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
KERNEL_DIR   := $(ROOT_DIR)kernel/agentos-root-task
SEL4_SDK_VERSION ?= 2.1.0
SEL4_SDK ?= $(HOME)/.cache/agentos/microkit-sdk-$(SEL4_SDK_VERSION)
export SEL4_SDK

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
  BIOS ?= $(BREW_PREFIX)/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
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

MICROKIT_SDK_ARCHIVE := microkit-sdk-$(SEL4_SDK_VERSION)-$(SDK_PLATFORM).tar.gz
MICROKIT_SDK_URL := https://github.com/seL4/microkit/releases/download/$(SEL4_SDK_VERSION)/$(MICROKIT_SDK_ARCHIVE)

# ─── Rust toolchain ──────────────────────────────────────────────────────────
export PATH := $(HOME)/.cargo/bin:$(PATH)
# Native guest helpers must keep their acquisition toolchain when the kernel
# sub-make prepends its own LLVM directory to PATH.
ifndef AGENTOS_HOST_TOOL_PATH
export AGENTOS_HOST_TOOL_PATH := $(PATH)
endif

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
                        -device virtserialport,bus=vser0.0,chardev=cc_pd_char,name=cc.0,nr=1 \
                        $(QEMU_ACCEL_NATIVE) \
                        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
                        -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.16 \
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
	@cargo xtask gen-channels

format:
	@cargo fmt --package xtask

policy-check:
	@cargo fmt --package xtask -- --check
	@cargo xtask policy-check

all: run

# =============================================================================
# install: set up build dependencies (alias: deps)
# =============================================================================
install: deps-tools
	@echo ""
	@echo "✅ All dependencies installed! Run 'make sdk' or 'make setup' next."

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
		dtc \
		coreutils \
		e2fsprogs \
		zstd
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
		build-essential \
		git \
		qemu-system-misc \
		qemu-system-arm \
		qemu-system-x86 \
		clang \
		lld \
		llvm \
		cmake \
		ninja-build \
		device-tree-compiler \
		e2fsprogs \
		libarchive-tools \
		openssh-client \
		curl \
		zstd \
		xz-utils \
		pkg-config
	@if apt-cache show qemu-system-riscv >/dev/null 2>&1; then \
		sudo apt-get install -y --no-install-recommends qemu-system-riscv; \
	fi
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
		e2fsprogs \
		gmake \
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
	@cargo build -p xtask
	@echo "  cargo:               $$(cargo --version)"

# =============================================================================
# setup/demo: two-command first-run path and one-command repeatable showcase
# =============================================================================
.PHONY: sdk-check
include tools/sdk/candidate.mk

sdk-check:
	@test -d "$(SEL4_SDK)/board" || \
		(echo "ERROR: Microkit SDK missing at $(SEL4_SDK); run 'make sdk'." && exit 1)
	@echo "✓ Microkit SDK $(SEL4_SDK_VERSION): $(SEL4_SDK)"

sdk:
	@if [ ! -d "$(SEL4_SDK)/board" ] && [ "$(SDK_PLATFORM)" = "unsupported-freebsd" ]; then \
		echo "ERROR: the Microkit SDK does not publish a FreeBSD host archive."; \
		echo "Use macOS or Linux for the guest demo, or set SEL4_SDK to a cross-build SDK."; \
		exit 1; \
	fi
	@if [ -d "$(SEL4_SDK)/board" ]; then \
		echo "✓ Microkit SDK $(SEL4_SDK_VERSION): $(SEL4_SDK)"; \
	else \
		echo "Downloading Microkit SDK $(SEL4_SDK_VERSION) for $(SDK_PLATFORM)..."; \
		command -v curl >/dev/null 2>&1 || \
			(echo "ERROR: curl is required; run 'make install' first." && exit 1); \
		tmp="$$(mktemp -t agentos-microkit-sdk.XXXXXX)"; \
		trap 'rm -f "$$tmp"' EXIT INT TERM; \
		curl -fsSL "$(MICROKIT_SDK_URL)" -o "$$tmp"; \
		mkdir -p "$$(dirname "$(SEL4_SDK)")"; \
		tar -xzf "$$tmp" -C "$$(dirname "$(SEL4_SDK)")"; \
		test -d "$(SEL4_SDK)/board" || \
			(echo "ERROR: SDK archive did not create $(SEL4_SDK)" && exit 1); \
		echo "✓ Microkit SDK installed: $(SEL4_SDK)"; \
	fi

setup:
	@$(MAKE) install
	@$(MAKE) sdk
	@$(MAKE) demo-check
	@echo ""
	@echo "✓ agentOS demo environment is ready."
	@echo "  Run: make demo"

demo-check:
	@echo "Checking the agentOS dual-guest demo environment..."
	@command -v cargo >/dev/null 2>&1 || \
		(echo "ERROR: cargo not found; run 'make setup'." && exit 1)
	@command -v qemu-system-aarch64 >/dev/null 2>&1 || \
		(echo "ERROR: qemu-system-aarch64 not found; run 'make setup'." && exit 1)
	@command -v cmake >/dev/null 2>&1 || \
		(echo "ERROR: cmake not found; run 'make setup'." && exit 1)
	@command -v ninja >/dev/null 2>&1 || \
		(echo "ERROR: ninja not found; run 'make setup'." && exit 1)
	@command -v dtc >/dev/null 2>&1 || \
		(echo "ERROR: dtc not found; run 'make setup'." && exit 1)
	@command -v ssh >/dev/null 2>&1 || \
		(echo "ERROR: ssh not found; install an OpenSSH client." && exit 1)
	@command -v ssh-keygen >/dev/null 2>&1 || \
		(echo "ERROR: ssh-keygen not found; install OpenSSH tools." && exit 1)
	@command -v curl >/dev/null 2>&1 || \
		(echo "ERROR: curl not found; run 'make setup'." && exit 1)
	@command -v bsdtar >/dev/null 2>&1 || \
		(echo "ERROR: bsdtar not found; run 'make setup'." && exit 1)
	@command -v zstd >/dev/null 2>&1 || \
		(echo "ERROR: zstd not found; run 'make setup'." && exit 1)
	@test -x "$(LLVM_BIN)/clang" || \
		(echo "ERROR: clang not found; run 'make setup'." && exit 1)
	@test -x "$(LLD_BIN)/ld.lld" || \
		(echo "ERROR: ld.lld not found; run 'make setup'." && exit 1)
	@command -v llvm-objcopy >/dev/null 2>&1 || test -x "$(LLVM_BIN)/llvm-objcopy" || \
		(echo "ERROR: llvm-objcopy not found; run 'make setup'." && exit 1)
	@test -d "$(SEL4_SDK)/board" || \
		(echo "ERROR: Microkit SDK not found at $(SEL4_SDK); run 'make sdk'." && exit 1)
	@echo "✓ Demo prerequisites are available."

demo-smoke: demo-check
	@$(MAKE) test-host
	@echo ""
	@echo "✓ Host-only demo smoke tests passed (this is not a boot proof)."

demo-test: demo-check
	@echo ""
	@echo "Running the non-interactive Ubuntu + FreeBSD authenticated-SSH proof..."
	@$(MAKE) e2e-dual-os BOARD=qemu_virt_aarch64

demo: demo-check
	@test -t 0 || \
		(echo "ERROR: 'make demo' requires an interactive terminal; use 'make demo-test' in automation." && exit 1)
	@echo ""
	@echo "Starting the agentOS dual-guest demonstration."
	@echo "The gate boots Ubuntu and FreeBSD concurrently and proves key-only SSH."
	@echo "After it passes, open the printed SSH commands in two other terminals."
	@echo "Press Enter here when the demonstration is complete."
	@echo ""
	@$(MAKE) run-dual-ssh

demo-desktop-test: demo-check
	@echo ""
	@echo "Running the non-interactive Ubuntu desktop protocol/frame proof..."
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu-live \
		--assert-desktop --timeout-secs $(DESKTOP_TEST_TIMEOUT)

demo-desktop: demo-check
	@test -t 0 || \
		(echo "ERROR: 'make demo-desktop' requires an interactive terminal; use 'make demo-desktop-test' in automation." && exit 1)
	@echo ""
	@echo "Starting the tunnel-confined Ubuntu desktop proof."
	@echo "The guest installs a lightweight Openbox + TigerVNC session at runtime."
	@echo "After the RFB frame gate passes, open the printed command in an external VNC viewer."
	@echo "Press Enter here when the demonstration is complete."
	@echo ""
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu-live \
		--assert-desktop --keep-running --timeout-secs $(DESKTOP_TEST_TIMEOUT)

demo-clean:
	@echo "Cleaning demo sockets, logs, and generated SSH keys..."
	@rm -f $(ROOT_DIR)build/cc_pd.sock $(ROOT_DIR)build/agentos-serial.sock
	@rm -rf $(ROOT_DIR)build/tmp/dual-ssh
	@rm -f $(ROOT_DIR)build/tmp/agentos-qemu-*.log
	@rm -f $(ROOT_DIR)build/tmp/agentos-qemu-*.cc_pd.sock
	@echo "✓ Demo runtime artifacts removed; guest image caches were preserved."

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
# fetch-guest: execute the bounded acquisition recipe for selected profiles
# =============================================================================
.PHONY: seed-guest-root
seed-guest-root:
	@cargo xtask seed-guest --root-ext4 "$(SEED_ROOT_EXT4)" --public-key "$(SEED_PUBLIC_KEY)" --output "$(SEED_OUTPUT)" --instance-id "$(SEED_INSTANCE_ID)" $(if $(SEED_GUEST_ADDRESS),--guest-address "$(SEED_GUEST_ADDRESS)",) $(if $(SEED_DISK_RAW),--disk-raw "$(SEED_DISK_RAW)" --partition-offset "$(SEED_PARTITION_OFFSET)",)

fetch-guest:
ifneq ($(strip $(GUEST_PRIMARY_PROFILE)),)
	@cargo xtask fetch-guest --profile $(GUEST_PRIMARY_PROFILE)
endif
ifneq ($(strip $(GUEST_SECONDARY_PROFILE)),)
	@cargo xtask fetch-guest --profile $(GUEST_SECONDARY_PROFILE)
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
		SEL4_SDK=$(SEL4_SDK) \
		SEL4_PROFILE=$(SEL4_PROFILE) \
		GUEST_OS=$(GUEST_OS) \
		GUEST_PRIMARY_PROFILE=$(GUEST_PRIMARY_PROFILE) \
		GUEST_SECONDARY_PROFILE=$(GUEST_SECONDARY_PROFILE) \
		GUEST_PRIMARY_LARGE=$(GUEST_PRIMARY_LARGE) \
		BOARD_NAME=$(BOARD_NAME) \
		BOARD_NATIVE=$(BOARD_NATIVE) \
		BOARD_UART_PHYS=$(BOARD_UART_PHYS) \
		BOARD_UART_SIZE=$(BOARD_UART_SIZE) \
		BOARD_UART_TYPE=$(BOARD_UART_TYPE) \
		BOARD_UART_IRQ=$(BOARD_UART_IRQ)
	@echo ""
	@echo "✓ Build complete: $(IMAGE)"
	@echo ""

# Interactive runs use the same bounded profile/scenario interpreter as QA.
# A two-slot composition requires a scenario so machine, memory, media, and
# host-port policy remain declarative.
_RUN_PROFILE := $(strip $(if $(_SELECTED_GUEST_PROFILE),$(_SELECTED_GUEST_PROFILE),$(if $(GUEST_PRIMARY_PROFILE),$(GUEST_PRIMARY_PROFILE),$(GUEST_SECONDARY_PROFILE))))
_RUN_SELECTION_ARGS = $(if $(_SELECTED_GUEST_SCENARIO),--scenario $(_SELECTED_GUEST_SCENARIO),$(if $(_RUN_PROFILE),--profile $(_RUN_PROFILE),))

# run (default): build native → QEMU with serial on stdout and a Unix guest
# =============================================================================
.PHONY: run-x86_64-cc
run-x86_64-cc:
	@cargo xtask qemu-launch --board x86_64_generic_vtx --x86-cc

.PHONY: run-x86_64-cc-linux
run-x86_64-cc-linux:
	@test -n "$(X86_ROOT_DISK)" || { echo 'Set X86_ROOT_DISK to a disposable raw Debian root disk'; exit 1; }
	@cargo xtask qemu-launch --board x86_64_generic_vtx --x86-cc \
		--x86-boot-profile $(if $(X86_BOOT_PROFILE),$(X86_BOOT_PROFILE),debian-amd64.toml) --x86-block-image "$(X86_ROOT_DISK)" \
		--x86-block-write --ssh-port $(if $(X86_SSH_PORT),$(X86_SSH_PORT),12224)

run:
	@if [ -z "$(_SELECTED_GUEST_SCENARIO)" ] && [ -n "$(GUEST_PRIMARY_PROFILE)" ] && [ -n "$(GUEST_SECONDARY_PROFILE)" ]; then \
		echo "ERROR: interactive two-slot launch requires GUEST_SCENARIO=<alias>"; \
		exit 2; \
	fi
	@echo ""
	@echo "╔══════════════════════════════════════════╗"
	@echo "║  agentOS — QEMU ($(NATIVE_ARCH))         ║"
	@echo "╚══════════════════════════════════════════╝"
	@echo ""
	@echo "Arch   : $(NATIVE_ARCH)"
	@echo "Board  : $(NATIVE_BOARD)"
	@echo "Config : $(if $(_SELECTED_GUEST_SCENARIO),scenario $(_SELECTED_GUEST_SCENARIO),$(if $(_RUN_PROFILE),profile $(_RUN_PROFILE),no guest profile))"
	@echo "GUI    : cd $(abspath $(ROOT_DIR)../agentos_gui) && make run"
	@echo ""
	@echo "Validated dual-guest SSH showcase: make demo"
	@echo "Exit QEMU: Ctrl-A X"
	@echo "──────────────────────────────────────────────"
	@cargo xtask qemu-launch --board $(NATIVE_BOARD) $(_RUN_SELECTION_ARGS) $(if $(filter 1,$(QEMU_FAST)),--fast,)

# run-fast: same as run, with TCG-mode performance knobs enabled.
# On Apple Silicon (TCG-only because HVF is incompatible with seL4) this
# adds -accel tcg,thread=multi while retaining the SDK-qualified CPU model, giving
# a noticeable boot-time speedup for dev iteration.  On Linux/KVM hosts
# QEMU_FAST is a no-op since hardware acceleration is already in use.
run-fast:
	@$(MAKE) run QEMU_FAST=1

# =============================================================================
# test: CI boot test (exits 0 on success, 1 on failure)
# =============================================================================
test: build
	@cargo xtask qemu-test --board $(BOARD) --guest-os $(QEMU_TEST_GUEST_OS) --timeout-secs $(QEMU_TEST_TIMEOUT)

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

# Dedicated KVM/VMX proof.  This is intentionally outside make gate: generic
# x86 coverage remains a portable reduced smoke test, while this target
# requires a host exposing /dev/kvm and nested Intel VMX.
gate-x86_64-vtx:
	@echo ""
	@echo "── [GATE] TARGET/KVM test: x86_64 VMX/EPT HLT exit ───────────"
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: gate-x86_64-firmware-modes
# Use SEL4_SDK_VERSION=2.3.0 for upstream VM-entry mode controls.
gate-x86_64-firmware-modes:
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-firmware-modes --timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: gate-x86_64-firmware-reset
.PHONY: x86-userspace-initramfs gate-x86_64-userspace
x86-userspace-initramfs:
	@cargo xtask build-x86-initramfs

gate-x86_64-userspace:
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-firmware-reset --assert-x86-userspace \
		--timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: gate-x86_64-teardown
gate-x86_64-teardown: gate-x86_64-userspace

.PHONY: gate-x86_64-linux-login
.PHONY: debian-x86-console-hook
.PHONY: debian-aarch64-console-hook
debian-aarch64-console-hook:
	@mkdir -p $(BUILD_TMP_DIR)/debian-aarch64/empty-toolchain
	clang --gcc-toolchain=$(BUILD_TMP_DIR)/debian-aarch64/empty-toolchain -target aarch64-unknown-linux-gnu -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pie -nostdlib -static -fuse-ld=lld -O2 \
		-Wall -Wextra -Werror -Wl,--build-id=none -Wl,-e,_start \
		guest-profiles/helpers/debian_init_bottom_aarch64.c -o $(BUILD_TMP_DIR)/debian-aarch64/udev
	llvm-objcopy --strip-all --remove-section=.comment $(BUILD_TMP_DIR)/debian-aarch64/udev
	clang --gcc-toolchain=$(BUILD_TMP_DIR)/debian-aarch64/empty-toolchain -target aarch64-unknown-linux-gnu -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pie -nostdlib -static -fuse-ld=lld -O2 \
		-Wall -Wextra -Werror -Wl,--build-id=none -Wl,-e,_start \
		guest-profiles/helpers/debian_init_top_aarch64.c -o $(BUILD_TMP_DIR)/debian-aarch64/udev-top
	llvm-objcopy --strip-all --remove-section=.comment $(BUILD_TMP_DIR)/debian-aarch64/udev-top

debian-x86-console-hook:
	@mkdir -p $(BUILD_TMP_DIR)/debian-x86
	clang -target x86_64-unknown-linux-gnu -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pie -nostdlib -static -fuse-ld=lld -O2 \
		-Wall -Wextra -Werror -Wl,--build-id=none -Wl,-e,_start \
		guest-profiles/helpers/debian_init_bottom_x86_64.c \
		-o $(BUILD_TMP_DIR)/debian-x86/udev
	llvm-objcopy --strip-all --remove-section=.comment $(BUILD_TMP_DIR)/debian-x86/udev
	clang -target x86_64-unknown-linux-gnu -ffreestanding -fno-builtin \
		-fno-stack-protector -fno-pie -nostdlib -static -fuse-ld=lld -O2 \
		-Wall -Wextra -Werror -Wl,--build-id=none -Wl,-e,_start \
		guest-profiles/helpers/debian_init_top_x86_64.c \
		-o $(BUILD_TMP_DIR)/debian-x86/udev-top
	llvm-objcopy --strip-all --remove-section=.comment $(BUILD_TMP_DIR)/debian-x86/udev-top

gate-x86_64-linux-login:
	@test -n "$(X86_ROOT_DISK)" || { echo 'Set X86_ROOT_DISK to a disposable raw root disk'; exit 1; }
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-firmware-reset --assert-x86-linux-login \
		$(if $(X86_BOOT_PROFILE),--x86-boot-profile $(X86_BOOT_PROFILE),) \
		--x86-block-image "$(X86_ROOT_DISK)" --x86-block-write \
		--timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: gate-x86_64-storage
.PHONY: gate-x86_64-debian-ssh
.PHONY: gate-x86_64-cc-linux
.PHONY: x86-smp-probe gate-x86_64-smp
x86-smp-probe:
	@mkdir -p $(BUILD_TMP_DIR)
	clang -target x86_64-linux-gnu -fuse-ld=lld -std=c11 -O2 -Wall -Wextra -Werror \
		-ffreestanding -fno-builtin -fno-stack-protector -fno-pie -nostdlib -static \
		-Wl,-e,_start -Wl,--build-id=none tests/platform/x86_smp_probe.c \
		tests/platform/x86_smp_probe_start.S -o $(BUILD_TMP_DIR)/x86-smp-probe
gate-x86_64-smp: x86-smp-probe
	$(MAKE) gate-x86_64-cc-linux X86_BOOT_PROFILE=debian-amd64-2cpu.toml \
		X86_SMP_PROBE=$(BUILD_TMP_DIR)/x86-smp-probe
gate-x86_64-cc-linux:
	@test -n "$(X86_ROOT_DISK)" -a -n "$(X86_SSH_KEY)" -a -n "$(X86_SSH_PORT)" || { echo 'Set X86_ROOT_DISK, X86_SSH_KEY and X86_SSH_PORT'; exit 1; }
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-firmware-reset --assert-x86-linux-login --assert-x86-cc \
		--x86-boot-profile $(if $(X86_BOOT_PROFILE),$(X86_BOOT_PROFILE),debian-amd64.toml) --x86-ssh-key "$(X86_SSH_KEY)" \
		$(if $(X86_SMP_PROBE),--x86-smp-probe "$(X86_SMP_PROBE)",) \
		$(if $(X86_SECONDARY_DISK),--x86-secondary-block-image "$(X86_SECONDARY_DISK)",) \
		$(if $(filter 1,$(X86_SECONDARY_WRITABLE)),--x86-secondary-block-write,) \
		$(if $(X86_SSH_KNOWN_HOSTS),--x86-ssh-known-hosts "$(X86_SSH_KNOWN_HOSTS)",) \
		--ssh-port "$(X86_SSH_PORT)" --x86-block-image "$(X86_ROOT_DISK)" \
		--x86-block-write --timeout-secs $(QEMU_TEST_TIMEOUT)

gate-x86_64-debian-ssh:
	@test -n "$(X86_ROOT_DISK)" -a -n "$(X86_SSH_KEY)" -a -n "$(X86_SSH_PORT)" || { echo 'Set X86_ROOT_DISK, X86_SSH_KEY and X86_SSH_PORT'; exit 1; }
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-firmware-reset --assert-x86-linux-login \
		--x86-boot-profile $(if $(X86_BOOT_PROFILE),$(X86_BOOT_PROFILE),debian-amd64.toml) --x86-ssh-key "$(X86_SSH_KEY)" \
		$(if $(X86_SSH_KNOWN_HOSTS),--x86-ssh-known-hosts "$(X86_SSH_KNOWN_HOSTS)",) \
		--ssh-port "$(X86_SSH_PORT)" --x86-block-image "$(X86_ROOT_DISK)" \
		--x86-block-write --timeout-secs $(QEMU_TEST_TIMEOUT)

gate-x86_64-storage:
	@cargo xtask x86-storage --timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: gate-x86_64-guest-faults
gate-x86_64-guest-faults:
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-guest-faults --timeout-secs $(QEMU_TEST_TIMEOUT)

gate-x86_64-firmware-reset:
	@cargo xtask qemu-test --board x86_64_generic_vtx --guest-os none \
		--assert-vmx-exit --assert-firmware-reset --timeout-secs $(QEMU_TEST_TIMEOUT)

# gate-guest-io: guest I/O proofs through the virtualizer path. GUEST_OS=none
# is a stub VMM, so the boot gates above prove PD load and root-task parking
# only; these three targets are what make "the OS does I/O" a true claim.
gate-guest-io:
	@echo ""
	@echo "── [GATE] GUEST I/O: buildroot virtio-net / virtio-blk, Ubuntu virtio-console ──"
	@$(MAKE) test-guest-net BOARD=qemu_virt_aarch64
	@$(MAKE) test-guest-blk BOARD=qemu_virt_aarch64
	@$(MAKE) test-guest-console BOARD=qemu_virt_aarch64

gate: test-host test-virtio-backends-build gate-aarch64 gate-x86_64 gate-guest-io

# Link the real firmware VMM, including its MMIO dispatcher and shared virtio
# transport. This needs SDK 2.3 VMCS controls, but no guest blobs, and does
# not claim Intel execution: make test-x86-firmware-build SEL4_SDK_VERSION=2.3.0
.PHONY: test-x86-firmware-build
test-x86-firmware-build:
	$(MAKE) -C kernel/agentos-root-task \
		BUILD_DIR=$(abspath $(BUILD_TMP_DIR)/x86-firmware-link) \
		AGENTOS_ARCH=x86_64 AGENTOS_BOARD=x86_64_generic_vtx \
		SEL4_SDK=$(SEL4_SDK) SEL4_SDK_VERSION=$(SEL4_SDK_VERSION) \
		X86_FIRMWARE_RESET=1 \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/guest_vmm_primary.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/x86_runner.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/serial_pd.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/blk_virt.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/net_virt.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/net_pd.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/virtio_blk.elf \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/rt_main.o \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/rt_x86_host_pci.o \
		$(abspath $(BUILD_TMP_DIR)/x86-firmware-link)/rt_virtio_pci_caps.o
	@echo "PASS: x86 firmware VMM, serial/block drivers and block/network virtualizer link checks"

.PHONY: test-x86-secondary-firmware-build
.PHONY: prepare-x86-profile
prepare-x86-profile:
	@test -n "$(X86_BOOT_PROFILE)" || { echo 'X86_BOOT_PROFILE is required'; exit 1; }
	cargo xtask guest-profile --profile "$(X86_BOOT_PROFILE)" \
		--prepare-x86-slot "$(if $(X86_VMM_SLOT),$(X86_VMM_SLOT),primary)"

test-x86-secondary-firmware-build:
	$(MAKE) test-x86-firmware-build GUEST_OS=none X86_VMM_SLOT=secondary BUILD_TMP_DIR=$(abspath $(BUILD_TMP_DIR)/secondary)
	$(MAKE) -C kernel/agentos-root-task \
		BUILD_DIR=$(abspath $(BUILD_TMP_DIR)/secondary-managed) \
		AGENTOS_ARCH=x86_64 AGENTOS_BOARD=x86_64_generic_vtx \
		SEL4_SDK=$(SEL4_SDK) SEL4_SDK_VERSION=$(SEL4_SDK_VERSION) \
		X86_FIRMWARE_RESET=1 X86_MANAGED_START=1 X86_VMM_SLOT=secondary \
		$(abspath $(BUILD_TMP_DIR)/secondary-managed)/x86_firmware_vmm.o
	@echo "PASS: secondary x86 coordinator and canonical adapters link; managed reset path compiles"

# test-host: alias for the host-only integration suite.  Named explicitly so
# callers and CI cannot mistake host-only coverage for target/QEMU proof.
# lint-source is a source lint (policy-check's sibling), not a test; it is
# listed here so the invariants it protects are checked on every host run,
# but it is not counted among the host tests below.
test-host: policy-check guest-profile-check lint-source test-integration test-operator-host test-log-ring-host test-framebuffer-host
test-host: test-x86-cpu-host
test-host: test-x86-composition-host
test-host: test-vm-manager-identity-host

.PHONY: test-vm-manager-identity-host
test-vm-manager-identity-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
		-DAGENTOS_TEST_HOST -ffunction-sections -fdata-sections \
		-iquote kernel/agentos-root-task/include -I tests/platform/loop-stubs -I platform/include -I libvmm/include \
		tests/platform/test_vm_manager_guest_identity.c -Wl,--gc-sections -o $(BUILD_TMP_DIR)/test_vm_manager_guest_identity
	$(BUILD_TMP_DIR)/test_vm_manager_guest_identity

.PHONY: test-x86-composition-host
test-x86-composition-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -iquote kernel/agentos-root-task/include \
		-DAGENTOS_X86_VTX=1 -DAGENTOS_X86_FIRMWARE_RESET=1 -DAGENTOS_X86_CC_PCI=1 -DAGENTOS_X86_MANAGED_START=1 \
		tests/platform/test_x86_composition.c kernel/agentos-root-task/src/system_desc_x86_64.c -o $(BUILD_TMP_DIR)/test_x86_composition
	$(BUILD_TMP_DIR)/test_x86_composition
	$(CC) -std=c11 -Wall -Wextra -Werror -iquote kernel/agentos-root-task/include \
		-DAGENTOS_X86_VTX=1 -DAGENTOS_X86_FIRMWARE_RESET=1 -DAGENTOS_X86_CC_PCI=1 -DAGENTOS_X86_MANAGED_START=1 -DAGENTOS_X86_DUAL_GUEST=1 \
		tests/platform/test_x86_composition.c kernel/agentos-root-task/src/system_desc_x86_64.c -o $(BUILD_TMP_DIR)/test_x86_dual_composition
	$(BUILD_TMP_DIR)/test_x86_dual_composition

test-host: test-guest-scheduling-host test-guest-gic-mapping-host test-guest-paging-host test-net-rx-accounting-host
test-host: test-guest-execution-host
test-host: test-fault-registry-host
.PHONY: test-fault-registry-host
test-fault-registry-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Itests/platform/virtio-stubs -Itests/platform/mmio-stubs -Ilibvmm/include \
		tests/platform/test_fault_registry.c libvmm/src/arch/aarch64/fault_registry.c \
		-o $(BUILD_TMP_DIR)/test_fault_registry
	$(BUILD_TMP_DIR)/test_fault_registry
test-host: test-x86-guest-objects-host
test-host: test-untyped-host
test-host: test-loader-page-tables-host
.PHONY: test-loader-page-tables-host
test-loader-page-tables-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I kernel/loader \
		tests/platform/test_loader_page_tables.c -o $(BUILD_TMP_DIR)/test_loader_page_tables
	$(BUILD_TMP_DIR)/test_loader_page_tables

.PHONY: test-untyped-host
test-untyped-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -DAGENTOS_TEST_HOST \
		-I kernel/agentos-root-task/include tests/api/test_ut_alloc.c \
		-o $(BUILD_TMP_DIR)/test_ut_alloc
	$(BUILD_TMP_DIR)/test_ut_alloc

test-host: test-x86-memory-rebuild-host
test-host: test-x86-recreate-host
.PHONY: test-x86-recreate-host
test-x86-recreate-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
		-Iplatform/include tests/platform/test_x86_recreate.c \
		platform/guest-vmm/x86_recreate.c -o $(BUILD_TMP_DIR)/test_x86_recreate
	$(BUILD_TMP_DIR)/test_x86_recreate
test-host: test-blk-rebind-host
test-host: test-net-rebind-host
.PHONY: test-net-rebind-host
test-net-rebind-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I platform/include \
		-idirafter kernel/agentos-root-task/include tests/platform/test_net_rebind.c \
		-o $(BUILD_TMP_DIR)/test_net_rebind
	$(BUILD_TMP_DIR)/test_net_rebind
.PHONY: test-blk-rebind-host
test-blk-rebind-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I platform/include \
		-idirafter kernel/agentos-root-task/include tests/platform/test_blk_rebind.c \
		-o $(BUILD_TMP_DIR)/test_blk_rebind
	$(BUILD_TMP_DIR)/test_blk_rebind

.PHONY: test-x86-memory-rebuild-host
test-x86-memory-rebuild-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Itests/platform/x86-objects-stubs \
		-Iplatform/include -Ilibvmm/include -iquote kernel/agentos-root-task/include \
		tests/platform/test_x86_memory_rebuild.c platform/guest-vmm/x86_rebuild_memory.c \
		-o $(BUILD_TMP_DIR)/test_x86_memory_rebuild
	$(BUILD_TMP_DIR)/test_x86_memory_rebuild
test-host: test-x86-teardown-host
test-host: test-x86-control-host

.PHONY: test-x86-control-host
test-x86-control-host:
	@mkdir -p $(BUILD_TMP_DIR)
	gcc -std=c11 -Wall -Wextra -Werror -DCONFIG_KERNEL_MCS \
		-I tests/platform/control-stubs -I platform/include \
		-idirafter kernel/agentos-root-task/include tests/platform/test_x86_control.c \
		platform/guest-vmm/x86_control.c platform/guest-vmm/runtime.c \
		-o $(BUILD_TMP_DIR)/test_x86_control
	$(BUILD_TMP_DIR)/test_x86_control
	gcc -std=c11 -Wall -Wextra -Werror -DCONFIG_KERNEL_MCS -DAGENTOS_X86_USERSPACE_PROOF -DAGENTOS_X86_LIFECYCLE_TRACE \
		-I tests/platform/control-stubs -I platform/include \
		-idirafter kernel/agentos-root-task/include tests/platform/test_x86_control.c \
		platform/guest-vmm/x86_control.c platform/guest-vmm/runtime.c \
		-o $(BUILD_TMP_DIR)/test_x86_control_proof
	$(BUILD_TMP_DIR)/test_x86_control_proof

.PHONY: test-x86-teardown-host
test-x86-teardown-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -DAGENTOS_X86_FIRMWARE_RESET \
		-Itests/platform/teardown-stubs -Iplatform/include -Ilibvmm/include \
		-iquote kernel/agentos-root-task/include tests/platform/test_x86_teardown.c \
		platform/guest-vmm/teardown.c platform/guest-vmm/x86_release_memory.c \
		-o $(BUILD_TMP_DIR)/test_x86_teardown
	$(BUILD_TMP_DIR)/test_x86_teardown

.PHONY: test-x86-guest-objects-host
test-x86-guest-objects-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -DAGENTOS_TEST_HOST \
		-Itests/platform/x86-objects-stubs -Ikernel/agentos-root-task/include \
		tests/platform/test_x86_guest_objects.c -o $(BUILD_TMP_DIR)/test_x86_guest_objects
	$(BUILD_TMP_DIR)/test_x86_guest_objects
.PHONY: test-guest-execution-host
test-guest-execution-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -DAGENTOS_TEST_HOST -DCONFIG_KERNEL_MCS -I tests/platform/execution-stubs -I platform/include -I kernel/agentos-root-task/include tests/platform/test_guest_execution.c platform/guest-ram/vmm_guest_execution.c -o $(BUILD_TMP_DIR)/test_guest_execution
	@$(BUILD_TMP_DIR)/test_guest_execution
.PHONY: test-net-rx-accounting-host
test-net-rx-accounting-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -include assert.h -ffunction-sections -fdata-sections -Wl,$(if $(filter Darwin,$(UNAME_S)),-dead_strip,--gc-sections) -I tests/platform/mmio-stubs -I platform/include -I libvmm/include -I libvmm/dep/sddf/include -I libvmm/dep/sddf/include/extern tests/platform/test_virtio_net_rx_accounting.c libvmm/src/virtio/net.c libvmm/src/virtio/gpa.c -o $(BUILD_TMP_DIR)/test_virtio_net_rx_accounting
	@$(BUILD_TMP_DIR)/test_virtio_net_rx_accounting
.PHONY: test-guest-paging-host
test-guest-paging-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I tests/platform/paging-stubs -I platform/include -iquote kernel/agentos-root-task/include tests/platform/test_guest_paging.c platform/guest-ram/vmm_guest_paging.c -o $(BUILD_TMP_DIR)/test_guest_paging
	@$(BUILD_TMP_DIR)/test_guest_paging
.PHONY: test-guest-gic-mapping-host
test-guest-gic-mapping-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I tests/platform/scheduling-stubs -I platform/include -iquote kernel/agentos-root-task/include tests/platform/test_guest_gic_mapping.c -o $(BUILD_TMP_DIR)/test_guest_gic_mapping
	@$(BUILD_TMP_DIR)/test_guest_gic_mapping
.PHONY: test-guest-scheduling-host
test-guest-scheduling-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I tests/platform/scheduling-stubs -I platform/include -iquote kernel/agentos-root-task/include tests/platform/test_guest_scheduling.c -o $(BUILD_TMP_DIR)/test_guest_scheduling
	@$(BUILD_TMP_DIR)/test_guest_scheduling
test-host: test-x86-profile-host
.PHONY: test-x86-profile-host
test-x86-profile-host:
	@mkdir -p $(BUILD_TMP_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -I platform/include -idirafter kernel/agentos-root-task/include \
		tests/platform/test_x86_profile.c platform/guest-vmm/x86_profile.c \
		platform/guest-vmm/profile.c libs/pd-support/sha256_mini.c \
		-o $(BUILD_TMP_DIR)/test_x86_profile
	@$(BUILD_TMP_DIR)/test_x86_profile
test-host: test-virtio-host-transport
test-host: test-virtio-pci-caps
test-host: test-cc-transport-host
test-host: test-cc-serial-control-host

.PHONY: test-cc-serial-control-host
test-cc-serial-control-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-I platform/include tests/platform/test_cc_serial_control.c \
		services/command-console/cc_serial_control.c -o $(BUILD_TMP_DIR)/test_cc_serial_control
	$(BUILD_TMP_DIR)/test_cc_serial_control

.PHONY: test-cc-transport-host
test-cc-transport-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-idirafter kernel/agentos-root-task/include tests/platform/test_cc_transport.c \
		-o $(BUILD_TMP_DIR)/test_cc_transport
	$(BUILD_TMP_DIR)/test_cc_transport

.PHONY: test-virtio-pci-caps
test-virtio-pci-caps:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-I platform/include tests/platform/test_virtio_pci_caps.c \
		platform/blk-virt/virtio_pci_caps.c -o $(BUILD_TMP_DIR)/test_virtio_pci_caps
	$(BUILD_TMP_DIR)/test_virtio_pci_caps

.PHONY: test-virtio-host-transport
test-virtio-host-transport:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-I platform/include -I libvmm/dep/sddf/include \
		-idirafter kernel/agentos-root-task/include \
		tests/platform/test_virtio_host_transport.c services/block-driver/virtio_host_transport.c \
		-o $(BUILD_TMP_DIR)/test_virtio_host_transport
	$(BUILD_TMP_DIR)/test_virtio_host_transport

test-host: test-x86-config-host
test-host: test-x86-apic-host
test-host: test-x86-smp-host
.PHONY: test-x86-smp-host
test-x86-smp-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-Iplatform/include tests/platform/test_x86_smp.c \
		platform/guest-vmm/x86_smp.c platform/guest-vmm/x86_apic.c \
		-o $(BUILD_TMP_DIR)/test_x86_smp
	$(BUILD_TMP_DIR)/test_x86_smp
test-host: test-x86-runner-host
test-host: test-x86-runner-ownership-host
test-host: test-blk-pci-media-host
.PHONY: test-blk-pci-media-host
test-blk-pci-media-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iplatform/include tests/platform/test_blk_pci_media.c -o $(BUILD_TMP_DIR)/test-blk-pci-media
	$(BUILD_TMP_DIR)/test-blk-pci-media
.PHONY: test-x86-runner-ownership-host
test-x86-runner-ownership-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iplatform/include tests/platform/test_x86_runner_ownership.c -o $(BUILD_TMP_DIR)/test-x86-runner-ownership
	$(BUILD_TMP_DIR)/test-x86-runner-ownership
.PHONY: test-x86-runner-host
test-x86-runner-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include \
		-idirafter kernel/agentos-root-task/include tests/platform/test_x86_runner.c \
		platform/guest-vmm/x86_runner.c -o $(BUILD_TMP_DIR)/test_x86_runner
	$(BUILD_TMP_DIR)/test_x86_runner
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-DCONFIG_VTX -DCONFIG_X86_64_VTX_64BIT_GUESTS \
		-Itests/platform/runner-stubs -Iplatform/include \
		-I$(SEL4_SDK)/board/x86_64_generic/release/include \
		-idirafter kernel/agentos-root-task/include \
		tests/platform/test_x86_runner_client.c platform/guest-vmm/x86_runner_client.c \
		platform/guest-vmm/x86_runner.c -o $(BUILD_TMP_DIR)/test_x86_runner_client
	$(BUILD_TMP_DIR)/test_x86_runner_client
	@set -e; for mode in classic mcs; do \
		flags=; if test "$$mode" = mcs; then flags=-DCONFIG_KERNEL_MCS; fi; \
		$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
			-DCONFIG_VTX -DCONFIG_X86_64_VTX_64BIT_GUESTS $$flags \
			-Itests/platform/runner-stubs -Iplatform/include \
			-I$(SEL4_SDK)/board/x86_64_generic/release/include \
			-idirafter kernel/agentos-root-task/include \
			tests/platform/test_x86_runner_pd.c platform/guest-vmm/x86_runner_pd.c \
			platform/guest-vmm/x86_runner.c -o $(BUILD_TMP_DIR)/test_x86_runner_$$mode; \
		$(BUILD_TMP_DIR)/test_x86_runner_$$mode; \
	done
test-host: test-x86-string-host
test-host: test-x86-rtc-host

.PHONY: test-x86-rtc-host
test-x86-rtc-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_rtc.c platform/guest-vmm/x86_rtc.c -o $(BUILD_TMP_DIR)/test_x86_rtc
	$(BUILD_TMP_DIR)/test_x86_rtc

.PHONY: test-x86-string-host
test-x86-string-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_string.c platform/guest-vmm/x86_string.c platform/guest-vmm/x86_memory.c platform/guest-vmm/x86_config.c platform/guest-vmm/x86_rtc.c -o $(BUILD_TMP_DIR)/test_x86_string
	$(BUILD_TMP_DIR)/test_x86_string

.PHONY: test-x86-apic-host
test-x86-apic-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_apic.c platform/guest-vmm/x86_apic.c platform/guest-vmm/x86_memory.c -o $(BUILD_TMP_DIR)/test_x86_apic
	$(BUILD_TMP_DIR)/test_x86_apic

.PHONY: test-x86-config-host
test-x86-config-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_config.c platform/guest-vmm/x86_config.c platform/guest-vmm/x86_rtc.c -o $(BUILD_TMP_DIR)/test_x86_config
	$(BUILD_TMP_DIR)/test_x86_config

.PHONY: test-x86-cpu-host
test-x86-cpu-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_cpu.c platform/guest-vmm/x86_cpu.c -o $(BUILD_TMP_DIR)/test_x86_cpu
	$(BUILD_TMP_DIR)/test_x86_cpu
test-host: test-x86-acpi-host
test-host: test-serial-uart-host

.PHONY: test-serial-uart-host
test-serial-uart-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_serial_uart.c platform/serial-virt/uart.c platform/serial-virt/pump.c -o $(BUILD_TMP_DIR)/test_serial_uart
	$(BUILD_TMP_DIR)/test_serial_uart
test-host: test-x86-ioapic-host
test-host: test-x86-acpi-loader-host
test-host: test-x86-event-host
test-host: test-virtio-mmio-core-host
test-host: test-virtio-console-rx-host
test-host: test-x86-virtio-host
test-host: test-x86-console-host
test-host: test-x86-block-host
test-host: test-x86-net-host

.PHONY: test-x86-net-host
test-x86-net-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
		-fsanitize=address,undefined -g -ffunction-sections \
		-Xlinker $(if $(filter Darwin,$(UNAME_S)),-dead_strip,--gc-sections) \
		-Itests/platform/block-stubs -Itests/platform/virtio-stubs -Ilibvmm/include \
		-Ilibvmm/dep/sddf/include -Ilibvmm/dep/sddf/include/microkit \
		-Iplatform/include -idirafter kernel/agentos-root-task/include \
		tests/platform/test_x86_net.c platform/net-virt/vmm_virtio_net.c \
		platform/net-virt/net_virt_pump.c \
		platform/guest-vmm/x86_virtio.c platform/guest-vmm/x86_ioapic.c \
		libvmm/src/virtio/net.c libvmm/src/virtio/mmio.c libvmm/src/virtio/gpa.c \
		-o $(BUILD_TMP_DIR)/test_x86_net
	$(BUILD_TMP_DIR)/test_x86_net
	$(BUILD_TMP_DIR)/test_x86_net host-fixture

.PHONY: test-x86-block-host
test-x86-block-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
		-fsanitize=address,undefined -g -ffunction-sections \
		-Xlinker $(if $(filter Darwin,$(UNAME_S)),-dead_strip,--gc-sections) \
		-Itests/platform/block-stubs -Itests/platform/virtio-stubs -Ilibvmm/include \
		-Ilibvmm/dep/sddf/include -Iplatform/include -idirafter kernel/agentos-root-task/include \
		tests/platform/test_x86_block.c platform/blk-virt/vmm_virtio_blk.c \
		platform/blk-virt/blk_virt_pump.c \
		platform/guest-vmm/x86_virtio.c platform/guest-vmm/x86_ioapic.c \
		libvmm/src/virtio/block.c libvmm/src/virtio/mmio.c libvmm/src/virtio/gpa.c \
		libvmm/dep/sddf/util/fsmalloc.c libvmm/dep/sddf/util/bitarray.c \
		-o $(BUILD_TMP_DIR)/test_x86_block
	$(BUILD_TMP_DIR)/test_x86_block

.PHONY: test-x86-console-host
test-x86-console-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-sign-compare \
		-fsanitize=address,undefined -g -ffunction-sections \
		-Xlinker $(if $(filter Darwin,$(UNAME_S)),-dead_strip,--gc-sections) \
		-Itests/platform/virtio-stubs -Ilibvmm/include -Ilibvmm/dep/sddf/include -Iplatform/include \
		tests/platform/test_x86_console.c platform/serial-virt/vmm_virtio_console.c \
		platform/guest-vmm/x86_virtio.c platform/guest-vmm/x86_ioapic.c \
		libvmm/src/virtio/console.c libvmm/src/virtio/mmio.c libvmm/src/virtio/gpa.c \
		-o $(BUILD_TMP_DIR)/test_x86_console
	$(BUILD_TMP_DIR)/test_x86_console

.PHONY: test-x86-virtio-host
test-x86-virtio-host:
	@mkdir -p $(BUILD_TMP_DIR)
	gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function \
		-fsanitize=address,undefined -g -I tests/platform/virtio-stubs -I libvmm/include -I platform/include \
		tests/platform/test_x86_virtio.c platform/guest-vmm/x86_virtio.c \
		platform/guest-vmm/x86_ioapic.c libvmm/src/virtio/mmio.c libvmm/src/virtio/gpa.c \
		-o $(BUILD_TMP_DIR)/test_x86_virtio
	$(BUILD_TMP_DIR)/test_x86_virtio

.PHONY: test-virtio-console-rx-host
test-virtio-console-rx-host:
	@mkdir -p $(BUILD_TMP_DIR)
	gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-I libvmm/include tests/platform/test_virtio_console_rx_ring.c \
		-o $(BUILD_TMP_DIR)/test_virtio_console_rx_ring
	@$(BUILD_TMP_DIR)/test_virtio_console_rx_ring

.PHONY: test-virtio-mmio-core-host
test-virtio-mmio-core-host:
	@mkdir -p $(BUILD_TMP_DIR)
	gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function \
		-fsanitize=address,undefined -g -I tests/platform/virtio-stubs -I libvmm/include \
		tests/platform/test_virtio_mmio_core.c libvmm/src/virtio/mmio.c libvmm/src/virtio/gpa.c \
		-o $(BUILD_TMP_DIR)/test_virtio_mmio_core
	@$(BUILD_TMP_DIR)/test_virtio_mmio_core

# Compile the production virtio backends against real architecture-specific
# seL4 headers. Host stubs cannot detect accidental ARM VCPU dependencies.
# This is a build check, not a guest I/O qualification.
.PHONY: test-virtio-backends-build
test-virtio-backends-build: test-x86-vmenter-host
	@set -eu; for arch in aarch64 x86_64; do \
		case $$arch in aarch64) board=qemu_virt_aarch64 ;; x86_64) board=x86_64_generic ;; esac; \
		out="$(BUILD_TMP_DIR)/virtio-backends-$$arch"; mkdir -p "$$out"; \
		for backend in console net block; do \
			clang -target $$arch-unknown-elf -ffreestanding -O2 -Wall -Werror -Wno-unused-function \
				-I"$(SEL4_SDK)/board/$$board/release/include" \
				-Ilibvmm/include -Ilibvmm/dep/sddf/include \
				-Ilibvmm/dep/sddf/include/sddf/util/custom_libc \
				-Ilibvmm/dep/sddf/include/microkit \
				-Ikernel/agentos-root-task/include \
				-c libvmm/src/virtio/$$backend.c -o "$$out/$$backend.o"; \
		done; \
		clang -target $$arch-unknown-elf -ffreestanding -O2 -Wall -Werror -Wno-unused-function \
			-I"$(SEL4_SDK)/board/$$board/release/include" \
			-Ilibvmm/include -Ilibvmm/dep/sddf/include \
			-Ilibvmm/dep/sddf/include/sddf/util/custom_libc -Iplatform/include \
			-c platform/serial-virt/vmm_virtio_console.c -o "$$out/vmm_virtio_console.o"; \
		clang -target $$arch-unknown-elf -ffreestanding -O2 -Wall -Werror -Wno-unused-function \
			-I"$(SEL4_SDK)/board/$$board/release/include" \
			-Ilibvmm/include -Ilibvmm/dep/sddf/include \
			-Ilibvmm/dep/sddf/include/sddf/util/custom_libc -Iplatform/include \
			-Ikernel/agentos-root-task/include \
			-c platform/blk-virt/vmm_virtio_blk.c -o "$$out/vmm_virtio_blk.o"; \
		clang -target $$arch-unknown-elf -ffreestanding -O2 -Wall -Werror -Wno-unused-function \
			-I"$(SEL4_SDK)/board/$$board/release/include" \
			-Ilibvmm/include -Ilibvmm/dep/sddf/include \
			-Ilibvmm/dep/sddf/include/sddf/util/custom_libc -Iplatform/include \
			-Ikernel/agentos-root-task/include \
			-Ilibvmm/dep/sddf/include/microkit \
			-c platform/net-virt/vmm_virtio_net.c -o "$$out/vmm_virtio_net.o"; \
		if test "$$arch" = x86_64; then \
			clang -target x86_64-unknown-elf -ffreestanding -O2 -Wall -Werror -Wno-unused-function \
				-I"$(SEL4_SDK)/board/$$board/release/include" \
				-Ilibvmm/include -Iplatform/include \
				-c platform/guest-vmm/x86_virtio.c -o "$$out/x86_virtio.o"; \
		fi; \
		echo "PASS: production virtio console/net/block compile for $$arch"; \
	done

.PHONY: test-x86-vmenter-host
test-x86-vmenter-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
		-Itests/platform/virtio-stubs -Iplatform/include \
		-I$(SEL4_SDK)/board/x86_64_generic/release/include \
		-idirafter kernel/agentos-root-task/include \
		tests/platform/test_x86_vmenter.c platform/guest-vmm/x86_runner.c \
		-o $(BUILD_TMP_DIR)/test_x86_vmenter
	$(BUILD_TMP_DIR)/test_x86_vmenter

.PHONY: test-x86-event-host
test-x86-event-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_event.c platform/guest-vmm/x86_event.c platform/guest-vmm/x86_apic.c -o $(BUILD_TMP_DIR)/test_x86_event
	$(BUILD_TMP_DIR)/test_x86_event

.PHONY: test-x86-acpi-loader-host
test-x86-acpi-loader-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_acpi_loader.c platform/guest-vmm/x86_acpi.c platform/guest-vmm/x86_config.c platform/guest-vmm/x86_rtc.c -o $(BUILD_TMP_DIR)/test_x86_acpi_loader
	$(BUILD_TMP_DIR)/test_x86_acpi_loader

.PHONY: test-x86-ioapic-host
test-x86-ioapic-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_ioapic.c platform/guest-vmm/x86_ioapic.c platform/guest-vmm/x86_apic.c -o $(BUILD_TMP_DIR)/test_x86_ioapic
	$(BUILD_TMP_DIR)/test_x86_ioapic

.PHONY: test-x86-acpi-host
test-x86-acpi-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_x86_acpi.c platform/guest-vmm/x86_acpi.c -o $(BUILD_TMP_DIR)/test_x86_acpi
	$(BUILD_TMP_DIR)/test_x86_acpi

# Optional independent AML parser/interpreter qualification (ACPICA tools).
IASL ?= iasl
ACPIEXEC ?= acpiexec
.PHONY: test-x86-acpi-aml
test-x86-acpi-aml: test-x86-acpi-host test-x86-acpi-loader-host
	$(BUILD_TMP_DIR)/test_x86_acpi_loader $(BUILD_TMP_DIR)/x86-console.aml
	$(IASL) -p $(BUILD_TMP_DIR)/x86-console -d $(BUILD_TMP_DIR)/x86-console.aml
	$(IASL) -p $(BUILD_TMP_DIR)/x86-console-roundtrip $(BUILD_TMP_DIR)/x86-console.dsl
	$(BUILD_TMP_DIR)/test_x86_acpi $(BUILD_TMP_DIR)/x86-cpus.aml
	$(IASL) -p $(BUILD_TMP_DIR)/x86-cpus -d $(BUILD_TMP_DIR)/x86-cpus.aml
	$(IASL) -p $(BUILD_TMP_DIR)/x86-cpus-roundtrip $(BUILD_TMP_DIR)/x86-cpus.dsl
	$(ACPIEXEC) -b 'execute \_SB.C000._UID; execute \_SB.C010._UID; execute \_SB.C01F._UID; execute \_SB.C01F._HID' $(BUILD_TMP_DIR)/x86-cpus.aml > $(BUILD_TMP_DIR)/x86-cpus-eval.log 2>&1
	@rg -q '\[Integer\] = 0000000000000000' $(BUILD_TMP_DIR)/x86-cpus-eval.log
	@rg -q '\[Integer\] = 0000000000000010' $(BUILD_TMP_DIR)/x86-cpus-eval.log
	@rg -q '\[Integer\] = 000000000000001F' $(BUILD_TMP_DIR)/x86-cpus-eval.log
	@rg -q '"ACPI0007"' $(BUILD_TMP_DIR)/x86-cpus-eval.log
test-host: policy-check guest-profile-check lint-source test-integration test-operator-host test-log-ring-host test-framebuffer-host test-virtio-gpu-host test-input-host test-agentctl-frame-host test-agentctl-console-host test-ramfb-host test-display-host

.PHONY: test-display-host
.PHONY: test-display-init
.PHONY: test-display
test-display: test-display-host test-ramfb-host
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-framebuffer --assert-display --timeout-secs $(QEMU_TEST_TIMEOUT)

test-display-init:
	@mkdir -p $(BUILD_TMP_DIR)
	$(MAKE) test-framebuffer DISPLAY_RAMFB=1 QEMU_TEST_TIMEOUT=$(QEMU_TEST_TIMEOUT) > $(BUILD_TMP_DIR)/display-init.log 2>&1 || { cat $(BUILD_TMP_DIR)/display-init.log; exit 1; }
	rg -Fq '[display] private DMA and scanout banks ready' $(BUILD_TMP_DIR)/display-init.log
	@echo 'Display driver initialized; native framebuffer clients and observer passed (scanout not tested)'

test-display-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_display.c platform/display/service.c -o $(BUILD_TMP_DIR)/test_display
	$(BUILD_TMP_DIR)/test_display
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_display_producer.c platform/display/producer.c platform/display/service.c -o $(BUILD_TMP_DIR)/test_display_producer
	$(BUILD_TMP_DIR)/test_display_producer

.PHONY: test-ramfb-host
test-ramfb-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_ramfb.c platform/display/ramfb.c -o $(BUILD_TMP_DIR)/test_ramfb
	$(BUILD_TMP_DIR)/test_ramfb
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_ramfb_mmio.c platform/display/ramfb_mmio.c -o $(BUILD_TMP_DIR)/test_ramfb_mmio
	$(BUILD_TMP_DIR)/test_ramfb_mmio

.PHONY: test-virtio-gpu-host
# Override only for an explicit before/after CPU benchmark; qualification
# always compiles the source in this checkout.
GPU_2D_BENCH_SOURCE ?= libvmm/src/virtio/gpu_2d.c
.PHONY: benchmark-virtio-gpu-host
benchmark-virtio-gpu-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -O2 -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -DAGENTOS_GPU_BENCHMARK -I tests/platform/mmio-stubs -I platform/include -I libvmm/include tests/platform/test_virtio_gpu_2d.c libvmm/src/virtio/gpu.c libvmm/src/virtio/gpa.c $(GPU_2D_BENCH_SOURCE) libvmm/src/virtio/gpu_ring.c platform/gpu-virt/framebuffer_adapter.c platform/framebuffer/service.c -o $(BUILD_TMP_DIR)/bench_virtio_gpu_2d
	$(BUILD_TMP_DIR)/bench_virtio_gpu_2d

test-virtio-gpu-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -I tests/platform/mmio-stubs -I platform/include -I libvmm/include tests/platform/test_virtio_gpu_2d.c libvmm/src/virtio/gpu.c libvmm/src/virtio/gpa.c libvmm/src/virtio/gpu_2d.c libvmm/src/virtio/gpu_ring.c platform/gpu-virt/framebuffer_adapter.c platform/framebuffer/service.c -o $(BUILD_TMP_DIR)/test_virtio_gpu_2d
	$(BUILD_TMP_DIR)/test_virtio_gpu_2d
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -I tests/platform/mmio-stubs -I libvmm/include tests/platform/test_virtio_mmio.c libvmm/src/virtio/mmio.c libvmm/src/arch/aarch64/virtio_mmio.c -o $(BUILD_TMP_DIR)/test_virtio_mmio
	$(BUILD_TMP_DIR)/test_virtio_mmio
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare -ffunction-sections -fdata-sections -Wl,$(if $(filter Darwin,$(UNAME_S)),-dead_strip,--gc-sections) -I tests/platform/mmio-stubs -I libvmm/include -I libvmm/dep/sddf/include -I libvmm/dep/sddf/include/extern tests/platform/test_virtio_net_config.c libvmm/src/virtio/mmio.c libvmm/src/arch/aarch64/virtio_mmio.c -o $(BUILD_TMP_DIR)/test_virtio_net_config
	$(BUILD_TMP_DIR)/test_virtio_net_config
test-host: policy-check guest-profile-check lint-source test-integration test-operator-host test-log-ring-host test-framebuffer-host test-guest-block-drain-host

.PHONY: test-framebuffer-host
.PHONY: test-input-host
test-input-host:
	@mkdir -p $(ROOT_DIR)build/tmp
	$(CC) -std=gnu11 -Wall -Wextra -Werror -I tests/platform/mmio-stubs -I platform/include -I libvmm/include -iquote kernel/agentos-root-task/include tests/platform/test_input_adopt.c platform/input-virt/vmm_virtio_input.c platform/input-virt/service.c -o $(BUILD_TMP_DIR)/test_input_adopt
	$(BUILD_TMP_DIR)/test_input_adopt
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include -iquote kernel/agentos-root-task/include tests/platform/test_input_rebind.c platform/input-virt/service.c platform/input-virt/rebind_service.c -o $(BUILD_TMP_DIR)/test_input_rebind
	$(BUILD_TMP_DIR)/test_input_rebind
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_input_queue.c platform/input-virt/service.c -o $(ROOT_DIR)build/tmp/test_input_queue
	$(ROOT_DIR)build/tmp/test_input_queue
	$(CC) -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -I tests/platform/mmio-stubs -I platform/include -I libvmm/include tests/platform/test_virtio_input.c libvmm/src/virtio/input.c libvmm/src/virtio/mmio.c libvmm/src/arch/aarch64/virtio_mmio.c libvmm/src/virtio/gpa.c platform/input-virt/service.c -o $(BUILD_TMP_DIR)/test_virtio_input
	$(BUILD_TMP_DIR)/test_virtio_input
	$(CC) -std=c11 -Wall -Wextra -Werror -DAGENTOS_TEST_HOST -I platform/include -I kernel/agentos-root-task/include tests/platform/test_agentctl_input.c platform/input-virt/service.c platform/inspect/inspect_snapshot.c -o $(BUILD_TMP_DIR)/test_agentctl_input
	$(BUILD_TMP_DIR)/test_agentctl_input
ifeq ($(UNAME_S),Linux)
	$(CC) -std=c11 -Wall -Wextra -Werror tests/platform/test_guest_input_probe.c -o $(BUILD_TMP_DIR)/test_guest_input_probe
	$(BUILD_TMP_DIR)/test_guest_input_probe
endif

.PHONY: guest-input-probe
guest-input-probe:
	@mkdir -p $(BUILD_TMP_DIR)
	$(GUEST_LINUX_CC) -static -O2 -std=c11 -Wall -Wextra -Werror tests/guest/input_probe.c -o $(BUILD_TMP_DIR)/guest-input-probe-aarch64

.PHONY: guest-frame-pattern host-frame-pattern
guest-frame-pattern:
	@mkdir -p $(BUILD_TMP_DIR)
	$(GUEST_LINUX_CC) -static -O2 -std=c11 -Wall -Wextra -Werror tests/guest/frame_pattern.c -o $(BUILD_TMP_DIR)/guest-frame-pattern-aarch64

host-frame-pattern:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -O2 -std=c11 -Wall -Wextra -Werror tests/guest/frame_pattern.c -o $(BUILD_TMP_DIR)/host-frame-pattern

.PHONY: test-agentctl-console-host
test-agentctl-console-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -DAGENTOS_TEST_HOST -I platform/include -I kernel/agentos-root-task/include tests/platform/test_agentctl_console.c platform/inspect/inspect_snapshot.c -o $(BUILD_TMP_DIR)/test_agentctl_console
	$(BUILD_TMP_DIR)/test_agentctl_console

.PHONY: test-agentctl-frame-host
test-agentctl-frame-host:
	@mkdir -p $(ROOT_DIR)build/tmp
	$(CC) -std=c11 -Wall -Wextra -Werror -DAGENTOS_TEST_HOST -I platform/include -I kernel/agentos-root-task/include tests/platform/test_agentctl_frame_capture.c platform/framebuffer/observer.c platform/inspect/inspect_snapshot.c -o $(ROOT_DIR)build/tmp/test_agentctl_frame_capture
	$(ROOT_DIR)build/tmp/test_agentctl_frame_capture

test-framebuffer-host:
	@mkdir -p $(ROOT_DIR)build/tmp
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_framebuffer_queue.c platform/framebuffer/service.c -o $(ROOT_DIR)build/tmp/test_framebuffer_queue
	$(ROOT_DIR)build/tmp/test_framebuffer_queue
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_framebuffer_observer.c platform/framebuffer/service.c platform/framebuffer/observer.c -o $(ROOT_DIR)build/tmp/test_framebuffer_observer
	$(ROOT_DIR)build/tmp/test_framebuffer_observer

.PHONY: test-framebuffer
test-framebuffer: test-framebuffer-host
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-framebuffer --timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: test-framebuffer-isolation
test-framebuffer-isolation:
	@mkdir -p build/evidence/framebuffer-isolation
	@set -e; for mode in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do \
	    cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-framebuffer \
	        --framebuffer-isolation-probe $$mode --timeout-secs $(QEMU_TEST_TIMEOUT); \
	    cp build/qemu_virt_aarch64/agentos.img build/evidence/framebuffer-isolation/mode-$$mode.img; \
	done

.PHONY: test-log-ring-host
test-log-ring-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_log_ring.c -o $(BUILD_TMP_DIR)/test_log_ring
	$(BUILD_TMP_DIR)/test_log_ring

.PHONY: test-log-rings test-log-isolation
test-log-rings: test-log-ring-host
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-log-rings --timeout-secs $(QEMU_TEST_TIMEOUT)
test-log-isolation:
	@mkdir -p build/evidence/log-isolation
	@set -e; for mode in 1 2 3; do \
	    cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --log-isolation-probe $$mode --timeout-secs $(QEMU_TEST_TIMEOUT); \
	    cp build/qemu_virt_aarch64/agentos.img build/evidence/log-isolation/mode-$$mode.img; \
	done

.PHONY: test-operator-host test-operator-session
test-operator-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -I platform/include tests/platform/test_operator_session.c platform/operator-session/session.c platform/inspect/inspect_snapshot.c platform/serial-virt/pump.c -o $(BUILD_TMP_DIR)/test_operator_session
	$(BUILD_TMP_DIR)/test_operator_session
test-operator-session:
	$(MAKE) -C tools/agentctl
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-operator-session --timeout-secs $(QEMU_TEST_TIMEOUT)
.PHONY: test-operator-isolation
test-operator-isolation:
	@mkdir -p build/evidence/operator-isolation
	@set -e; for mode in 1 2 3 4 5 6 7; do \
	    cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none \
	        --operator-isolation-probe $$mode --timeout-secs $(QEMU_TEST_TIMEOUT); \
	    cp build/qemu_virt_aarch64/agentos.img build/evidence/operator-isolation/mode-$$mode.img; \
	done

# Host behavior plus real SDK compilation; this is not a native-PD boot proof.
.PHONY: test-rust-pd-abi test-native-rust
.PHONY: test-inspect
test-inspect:
	$(MAKE) -C tools/agentctl
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-inspect --timeout-secs $(QEMU_TEST_TIMEOUT)
.PHONY: test-inspect-readonly
test-inspect-readonly:
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --inspect-write-probe --timeout-secs $(QEMU_TEST_TIMEOUT)

test-native-rust:
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-native-rust --timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: test-native-network-isolation
.PHONY: test-native-with-guest
test-native-with-guest:
	cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu-live --assert-live --assert-native-guest --timeout-secs $(QEMU_TEST_TIMEOUT) --ssh-port $(QEMU_TEST_SSH_PORT)

test-native-network-isolation:
	@mkdir -p build/evidence/native-network-isolation
	@set -e; for mode in 1 2 3 4 5 6 7 8 9 10; do \
	    cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --assert-native-rust \
	        --native-network-isolation-probe $$mode --timeout-secs $(QEMU_TEST_TIMEOUT); \
	    cp build/qemu_virt_aarch64/agentos.img build/evidence/native-network-isolation/mode-$$mode.img; \
	done

test-rust-pd-abi:
	cargo test -p agentos-pd --features std
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=c11 -Wall -Wextra -Werror -Iplatform/include -c tests/native-rust/network_peer.c -o $(BUILD_TMP_DIR)/rust_net_peer.o
	$(CC) -std=c11 -Wall -Wextra -Werror -Iplatform/include -c platform/net-virt/net_virt_pump.c -o $(BUILD_TMP_DIR)/rust_net_pump.o
	$(AR) rcs $(BUILD_TMP_DIR)/librust_net_peer.a $(BUILD_TMP_DIR)/rust_net_peer.o $(BUILD_TMP_DIR)/rust_net_pump.o
	rustc --edition=2021 --test tests/native-rust/network_interop.rs -L native=$(BUILD_TMP_DIR) -l static=rust_net_peer -o $(BUILD_TMP_DIR)/rust_net_interop
	$(BUILD_TMP_DIR)/rust_net_interop
	$(CC) -std=c11 -Wall -Wextra -Werror -fno-builtin -DAGENTOS_TEST_HOST libs/rust-pd/runtime/memory.c tests/native-rust/memory_test.c -o $(BUILD_TMP_DIR)/rust_memory_test
	$(BUILD_TMP_DIR)/rust_memory_test
	$(MAKE) -C kernel/agentos-root-task BUILD_DIR=$(abspath build/rust-pd-abi-aarch64) AGENTOS_ARCH=aarch64 AGENTOS_BOARD=qemu_virt_aarch64 $(abspath build/rust-pd-abi-aarch64/rust_pd_ipc.o)
	$(MAKE) -C kernel/agentos-root-task BUILD_DIR=$(abspath build/rust-pd-abi-x86_64) AGENTOS_ARCH=x86_64 AGENTOS_BOARD=x86_64_generic $(abspath build/rust-pd-abi-x86_64/rust_pd_ipc.o)
	$(MAKE) -C kernel/agentos-root-task BUILD_DIR=$(abspath build/rust-pd-abi-aarch64) AGENTOS_ARCH=aarch64 AGENTOS_BOARD=qemu_virt_aarch64 $(abspath build/rust-pd-abi-aarch64/rust_pd_network.o)
	$(MAKE) -C kernel/agentos-root-task BUILD_DIR=$(abspath build/rust-pd-abi-x86_64) AGENTOS_ARCH=x86_64 AGENTOS_BOARD=x86_64_generic $(abspath build/rust-pd-abi-x86_64/rust_pd_network.o)

# lint-source: architecture-invariant lint over checked-in artifacts (headers,
# the compiled AArch64 topology, guest FDT templates, guest profiles, QEMU
# launch tooling).  See tests/platform/lint_source_invariants.c.  It proves no
# behaviour and is NOT a guest-path test; it fails when docs/TCB.md I/O
# invariants 1-5 stop being visible in the tree.
lint-source:
	@mkdir -p $(BUILD_TMP_DIR)
	@gcc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L \
		-I platform/include -I . -idirafter kernel/agentos-root-task/include \
		-DAOS_REPO_ROOT='"$(ROOT_DIR)"' \
		tests/platform/lint_source_invariants.c \
		kernel/agentos-root-task/src/system_desc_aarch64.c \
		-o $(BUILD_TMP_DIR)/lint_source_invariants
	@$(BUILD_TMP_DIR)/lint_source_invariants

guest-profile-check:
	@mkdir -p $(BUILD_TMP_DIR)/guest-profiles
	@cargo xtask guest-profile --check-all
	@cargo xtask guest-profile --profile buildroot.toml --output $(BUILD_TMP_DIR)/guest-profiles/buildroot.bin
	@cargo xtask guest-profile --profile ubuntu-e2e.toml --output $(BUILD_TMP_DIR)/guest-profiles/ubuntu-e2e.bin
	@cargo xtask guest-profile --profile freebsd.toml --output $(BUILD_TMP_DIR)/guest-profiles/freebsd.bin
	@gcc -std=c11 -Wall -Wextra -Werror -I platform/include \
		tests/platform/test_guest_profile.c \
		-o $(BUILD_TMP_DIR)/test_guest_profile
	@$(BUILD_TMP_DIR)/test_guest_profile \
		$(BUILD_TMP_DIR)/guest-profiles/buildroot.bin \
		$(BUILD_TMP_DIR)/guest-profiles/ubuntu-e2e.bin \
		$(BUILD_TMP_DIR)/guest-profiles/freebsd.bin
	@gcc -std=c11 -Wall -Wextra -Werror -I platform/include \
		tests/platform/test_guest_boot.c \
		-o $(BUILD_TMP_DIR)/test_guest_boot
	@$(BUILD_TMP_DIR)/test_guest_boot

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

# Guest I/O proof: boot buildroot Linux under linux_vmm and require the
# emulated virtio-net (IPA 0x0A010000) to probe, reach DRIVER_OK, and pump
# at least one guest TX frame back onto RX. GUEST_OS=none is a stub VMM and
# cannot prove this. The host-side tests/platform/test_virtio_net_guest_path.c
# (simulated virtq pump) and tests/platform/lint_source_invariants.c (source
# lint) are not this gate.
test-guest-net:
	@if [ "$(BOARD)" != "qemu_virt_aarch64" ]; then \
		echo "test-guest-net requires BOARD=qemu_virt_aarch64 (got BOARD=$(BOARD))"; \
		exit 1; \
	fi
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-net --ssh-port $(QEMU_TEST_SSH_PORT)

# Guest I/O proof: boot buildroot Linux under linux_vmm and require the
# emulated virtio-blk (IPA 0x0A020000) to probe, reach DRIVER_OK, and pump
# at least one guest request (partition scan of the RAM disk). The host-side
# tests/platform/test_blk_virt_pump.c and the source lint are not this gate.
# The combined Ubuntu device proof is make test-ubuntu-virtio.
.PHONY: test-block-isolation
.PHONY: test-serial-isolation
test-serial-isolation:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 1
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 2
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 3
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 4
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 5
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 6
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 7
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --serial-isolation-probe 8

.PHONY: test-network-isolation
test-network-isolation:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 1
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 2
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 3
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 4
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 5
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 6
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 7
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --network-isolation-probe 8

.PHONY: test-virtualizer-authority
test-virtualizer-authority:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --virtualizer-authority-probe 1
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --virtualizer-authority-probe 2

test-block-isolation:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 1
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 2
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 3
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 4
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 5
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 6
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 7
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --timeout-secs $(QEMU_TEST_TIMEOUT) --block-isolation-probe 8

.PHONY: test-guest-ram-recycle
.PHONY: test-guest-image-recycle
test-guest-image-recycle:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-console --assert-guest-ram-recycle --ssh-port $(QEMU_TEST_SSH_PORT)

test-guest-ram-recycle:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-blk --assert-guest-ram-recycle --ssh-port $(QEMU_TEST_SSH_PORT)

.PHONY: test-guest-block-drain-host
.PHONY: test-guest-block-drain
test-guest-block-drain: test-guest-block-drain-host
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-blk --assert-guest-block-drain --ssh-port $(QEMU_TEST_SSH_PORT)

test-guest-block-drain-host:
	@mkdir -p $(BUILD_TMP_DIR)
	$(CC) -std=gnu11 -g -Wall -include assert.h -I tests/platform/block-drain-stubs -I libvmm/include -I libvmm/dep/sddf/include tests/platform/test_virtio_blk_drain.c libvmm/src/virtio/block.c libvmm/src/virtio/gpa.c libvmm/dep/sddf/util/fsmalloc.c libvmm/dep/sddf/util/bitarray.c -o $(BUILD_TMP_DIR)/test_virtio_blk_drain
	$(BUILD_TMP_DIR)/test_virtio_blk_drain

test-guest-blk:
	@if [ "$(BOARD)" != "qemu_virt_aarch64" ]; then \
		echo "test-guest-blk requires BOARD=qemu_virt_aarch64 (got BOARD=$(BOARD))"; \
		exit 1; \
	fi
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os buildroot --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-blk --ssh-port $(QEMU_TEST_SSH_PORT)

# Boot Ubuntu to its login prompt over agentOS's emulated virtio-console,
# then inject input and require the guest to echo it back through sDDF queues.
test-guest-console:
	@if [ "$(BOARD)" != "qemu_virt_aarch64" ]; then \
		echo "test-guest-console requires BOARD=qemu_virt_aarch64 (got BOARD=$(BOARD))"; \
		exit 1; \
	fi
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-console --ssh-port $(QEMU_TEST_SSH_PORT)

.PHONY: test-console-backpressure
.PHONY: test-guest-teardown
.PHONY: test-guest-scheduling
test-guest-scheduling:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-console --assert-managed-guest --ssh-port $(QEMU_TEST_SSH_PORT)

test-guest-teardown:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-console --assert-guest-teardown --ssh-port $(QEMU_TEST_SSH_PORT)

.PHONY: test-guest-queue-recycle
test-guest-queue-recycle:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-console --assert-guest-teardown --assert-guest-queue-recycle --ssh-port $(QEMU_TEST_SSH_PORT)

.PHONY: test-guest-paging-recycle
test-guest-paging-recycle: test-guest-queue-recycle
.PHONY: test-guest-execution-recycle
test-guest-execution-recycle: test-guest-queue-recycle

test-console-backpressure:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-emulated-console --assert-console-backpressure --ssh-port $(QEMU_TEST_SSH_PORT)

# Deterministic initramfs device proof. Host media is owned by virtio_blk;
# Ubuntu's DTB advertises agentOS emulated devices only.
test-ubuntu-virtio:
	@if [ "$(BOARD)" != "qemu_virt_aarch64" ]; then \
		echo "test-ubuntu-virtio requires BOARD=qemu_virt_aarch64 (got BOARD=$(BOARD))"; \
		exit 1; \
	fi
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-agentos-virtio

# End-state proof: boot Ubuntu's real Casper initrd and ISO filesystem to a
# serial login while requiring real I/O through every agentOS VirtIO class.
.PHONY: test-debian-live
.PHONY: test-guest-gic-failure
test-guest-gic-failure:
	@for mode in 1 2 3; do \
		cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none \
			--guest-gic-failure-probe $$mode --timeout-secs 120 || exit $$?; \
	done

.PHONY: test-debian-nocloud-ssh
.PHONY: test-debian-nocloud-auto
.PHONY: test-debian-nocloud-graphics
.PHONY: test-debian-nocloud-graphics-teardown
test-debian-nocloud-graphics-teardown: QEMU_TEST_TIMEOUT = 1800
test-debian-nocloud-graphics-teardown: QEMU_TEST_SSH_PORT = 12223
test-debian-nocloud-graphics-teardown:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-nocloud-graphics-input \
		--seed-profile --assert-agentos-virtio --assert-guest-display --assert-guest-teardown --assert-guest-queue-recycle \
		--ssh-port $(QEMU_TEST_SSH_PORT) --timeout-secs $(QEMU_TEST_TIMEOUT)

test-debian-nocloud-graphics: QEMU_TEST_TIMEOUT = 1800
test-debian-nocloud-graphics: QEMU_TEST_SSH_PORT = 12223
test-debian-nocloud-graphics:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-nocloud-graphics-input \
		--seed-profile --assert-agentos-virtio --assert-guest-display \
		--ssh-port $(QEMU_TEST_SSH_PORT) --timeout-secs $(QEMU_TEST_TIMEOUT)

# Retain the same pinned, seeded graphics guest for external binary-IPC clients.
.PHONY: demo-debian-nocloud-graphics
demo-debian-nocloud-graphics: QEMU_TEST_TIMEOUT = 1800
demo-debian-nocloud-graphics: QEMU_TEST_SSH_PORT = 12223
demo-debian-nocloud-graphics:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-nocloud-graphics-input \
		--seed-profile --assert-agentos-virtio --assert-guest-display --keep-running \
		$(if $(filter 1,$(QEMU_RETAIN_FAILED)),--retain-failed-guest,) \
		--ssh-port $(QEMU_TEST_SSH_PORT) --timeout-secs $(QEMU_TEST_TIMEOUT)

test-debian-nocloud-auto: QEMU_TEST_TIMEOUT = 1200
test-debian-nocloud-auto: QEMU_TEST_SSH_PORT = 12222
test-debian-nocloud-auto:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-arm64-nocloud \
		--seed-profile --assert-agentos-virtio --ssh-port $(QEMU_TEST_SSH_PORT) --timeout-secs $(QEMU_TEST_TIMEOUT)

.PHONY: test-debian-nocloud-cold-boots
test-debian-nocloud-cold-boots: QEMU_TEST_TIMEOUT = 1200
test-debian-nocloud-cold-boots: QEMU_TEST_SSH_PORT = 12222
test-debian-nocloud-cold-boots:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-arm64-nocloud \
		--seed-profile --assert-seeded-cold-boots --assert-agentos-virtio \
		--ssh-port $(QEMU_TEST_SSH_PORT) --timeout-secs $(QEMU_TEST_TIMEOUT)

test-debian-nocloud-ssh:
	@test -n "$(SEEDED_SSH_KEY)" -a -n "$(QEMU_TEST_SSH_PORT)" || { echo 'Set SEEDED_SSH_KEY and QEMU_TEST_SSH_PORT'; exit 1; }
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-arm64-nocloud \
		--assert-agentos-virtio \
		--seeded-ssh-key "$(SEEDED_SSH_KEY)" --ssh-port "$(QEMU_TEST_SSH_PORT)" \
		$(if $(SEEDED_SSH_KNOWN_HOSTS),--seeded-ssh-known-hosts "$(SEEDED_SSH_KNOWN_HOSTS)",) \
		$(if $(SEEDED_DIRECTORY),--seeded-directory "$(SEEDED_DIRECTORY)",) \
		--timeout-secs $(QEMU_TEST_TIMEOUT)
.PHONY: test-guest-gpu
.PHONY: test-guest-input
.PHONY: test-guest-graphics-input
.PHONY: test-guest-display
.PHONY: demo-guest-display
# Spark input qualification takes about ten minutes through Debian boot and
# SSH provisioning. Preserve explicit environment/command-line timeout choices.
ifeq ($(origin QEMU_TEST_TIMEOUT),file)
test-guest-input test-guest-graphics-input demo-guest-display: QEMU_TEST_TIMEOUT = 1800
endif
test-guest-display:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-graphics-input --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --assert-guest-display --ssh-port $(QEMU_TEST_SSH_PORT)

demo-guest-display:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-graphics-input --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --assert-guest-display --ssh-port $(QEMU_TEST_SSH_PORT) --keep-running

test-guest-graphics-input:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-graphics-input --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --ssh-port $(QEMU_TEST_SSH_PORT)

test-guest-input:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-input --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --ssh-port $(QEMU_TEST_SSH_PORT)

test-guest-gpu:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian-gpu --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --ssh-port $(QEMU_TEST_GPU_SSH_PORT)

test-debian-live:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --ssh-port $(QEMU_TEST_SSH_PORT)

.PHONY: test-debian-persistence
test-debian-persistence:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os debian --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --assert-persistent-boots --ssh-port $(QEMU_TEST_SSH_PORT)

UBUNTU_BOOT_TIMING_RECEIPT ?=
DEBIAN_BOOT_TIMING_RECEIPT ?=
GUEST_BOOT_TIMING_COMPARISON ?= build/evidence/guest-boot-timing-comparison.json
.PHONY: test-guest-boot-timing-compare
# Compare existing successful launch-to-SSH receipts. This does not run guests
# or impose a performance threshold; it rejects incompatible evidence.
test-guest-boot-timing-compare:
	@test -n "$(UBUNTU_BOOT_TIMING_RECEIPT)" || { echo "set UBUNTU_BOOT_TIMING_RECEIPT to an Ubuntu boot-timing receipt"; exit 2; }
	@test -n "$(DEBIAN_BOOT_TIMING_RECEIPT)" || { echo "set DEBIAN_BOOT_TIMING_RECEIPT to a Debian boot-timing receipt"; exit 2; }
	@mkdir -p "$(dir $(GUEST_BOOT_TIMING_COMPARISON))"
	@cargo xtask guest-boot-timing-compare --ubuntu-receipt "$(UBUNTU_BOOT_TIMING_RECEIPT)" --debian-receipt "$(DEBIAN_BOOT_TIMING_RECEIPT)" --output "$(GUEST_BOOT_TIMING_COMPARISON)"

test-ubuntu-live:
	@if [ "$(BOARD)" != "qemu_virt_aarch64" ]; then \
		echo "test-ubuntu-live requires BOARD=qemu_virt_aarch64 (got BOARD=$(BOARD))"; \
		exit 1; \
	fi
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os ubuntu-live --timeout-secs $(QEMU_TEST_TIMEOUT) --assert-live --assert-agentos-virtio --ssh-port $(QEMU_TEST_SSH_PORT)

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
# via #ifdef AGENTOS_TEST_HOST.  No QEMU required.  Every suite here must
# exercise code; source-text checks belong in `make lint-source`, not here.
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
	    tests/test_snapshot_sched.c \
	    tests/test_proc_server.c \
	    tests/test_serial_pd.c \
	    tests/test_framebuffer_pd.c \
	    tests/test_framebuffer_unsupported_hardware.c \
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
	if gcc -DAGENTOS_TEST_HOST -I kernel/agentos-root-task/include \
	        tests/platform/test_log_serial.c tests/platform/log_serial_driver.c \
	        -o $(BUILD_TMP_DIR)/test_log_serial 2>&1 \
	    && $(BUILD_TMP_DIR)/test_log_serial; then \
	    echo "PASS: tests/platform/test_log_serial.c"; \
	else \
	    echo "FAIL: tests/platform/test_log_serial.c"; \
	    status=1; \
	fi; \
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
	if gcc -std=c11 -Wall -Wextra -Werror -DCONFIG_KERNEL_MCS \
	        -I tests/platform/loop-stubs -I platform/include \
	        tests/platform/test_net_server_loop.c -o $(BUILD_TMP_DIR)/test_net_server_loop \
	    && $(BUILD_TMP_DIR)/test_net_server_loop; then :; \
	else status=1; fi; \
	if gcc -I platform/include tests/platform/test_arm_vtimer.c \
	        -o $(BUILD_TMP_DIR)/test_arm_vtimer 2>&1 \
	    && $(BUILD_TMP_DIR)/test_arm_vtimer; then \
	    echo "PASS: tests/platform/test_arm_vtimer.c"; \
	else \
	    echo "FAIL: tests/platform/test_arm_vtimer.c"; \
	    status=1; \
	fi; \
	if gcc -I platform/include \
	        tests/platform/test_net_rx_drain.c \
	        -o $(BUILD_TMP_DIR)/test_net_rx_drain 2>&1 \
	    && $(BUILD_TMP_DIR)/test_net_rx_drain; then \
	    echo "PASS: tests/platform/test_net_rx_drain.c"; \
	else \
	    echo "FAIL: tests/platform/test_net_rx_drain.c"; \
	    status=1; \
	fi; \
	if gcc -DAGENTOS_TEST_HOST -I platform/include -I . \
	        -idirafter kernel/agentos-root-task/include \
	        tests/platform/test_net_host_fanout.c \
	        services/block-driver/virtio_host_transport.c \
	        -o $(BUILD_TMP_DIR)/test_net_host_fanout 2>&1 \
	    && $(BUILD_TMP_DIR)/test_net_host_fanout; then \
	    echo "PASS: tests/platform/test_net_host_fanout.c"; \
	else \
	    echo "FAIL: tests/platform/test_net_host_fanout.c"; \
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
	if gcc -I platform/include -I tests/platform \
	        tests/platform/test_virtio_net_guest_path.c \
	        tests/platform/virtio_mmio_net_emu.c \
	        platform/net-virt/net_virt_pump.c \
	        -o $(BUILD_TMP_DIR)/test_virtio_net_guest_path 2>&1 \
	    && $(BUILD_TMP_DIR)/test_virtio_net_guest_path; then \
	    echo "PASS: tests/platform/test_virtio_net_guest_path.c"; \
	else \
	    echo "FAIL: tests/platform/test_virtio_net_guest_path.c"; \
	    status=1; \
	fi; \
	if gcc -I platform/include \
	        tests/platform/test_gpa_translate.c \
	        platform/guest-ram/gpa_translate.c \
	        -o $(BUILD_TMP_DIR)/test_gpa_translate 2>&1 \
	    && $(BUILD_TMP_DIR)/test_gpa_translate; then \
	    echo "PASS: tests/platform/test_gpa_translate.c"; \
	else \
	    echo "FAIL: tests/platform/test_gpa_translate.c"; \
	    status=1; \
	fi; \
	if gcc -DAGENTOS_TEST_HOST \
	        -I platform/include -I kernel/agentos-root-task/include \
	        tests/platform/test_guest_vmm_runtime.c \
	        platform/guest-vmm/runtime.c \
	        -o $(BUILD_TMP_DIR)/test_guest_vmm_runtime 2>&1 \
	    && $(BUILD_TMP_DIR)/test_guest_vmm_runtime; then \
	    echo "PASS: tests/platform/test_guest_vmm_runtime.c"; \
	else \
	    echo "FAIL: tests/platform/test_guest_vmm_runtime.c"; \
	    status=1; \
	fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -I platform/include \
	        tests/platform/test_serial_virt_pump.c platform/serial-virt/pump.c \
	        -o $(BUILD_TMP_DIR)/test_serial_virt_pump \
	    && $(BUILD_TMP_DIR)/test_serial_virt_pump; then :; \
	else status=1; fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -I platform/include \
	        -iquote kernel/agentos-root-task/include \
	        tests/platform/test_serial_virt_authority.c \
	        -o $(BUILD_TMP_DIR)/test_serial_virt_authority \
	    && $(BUILD_TMP_DIR)/test_serial_virt_authority; then :; \
	else status=1; fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -I platform/include \
	        -iquote kernel/agentos-root-task/include \
	        tests/platform/test_serial_virt_service.c platform/serial-virt/service.c \
	        platform/serial-virt/pump.c -o $(BUILD_TMP_DIR)/test_serial_virt_service \
	    && $(BUILD_TMP_DIR)/test_serial_virt_service; then :; \
	else status=1; fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -I platform/include \
	        tests/platform/test_serial_endpoint.c platform/serial-virt/endpoint.c \
	        platform/serial-virt/pump.c -o $(BUILD_TMP_DIR)/test_serial_endpoint \
	    && $(BUILD_TMP_DIR)/test_serial_endpoint; then :; \
	else status=1; fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -DCONFIG_KERNEL_MCS -DAGENTOS_TEST_HOST \
	        -I tests/platform/loop-stubs -I platform/include -I . -idirafter kernel/agentos-root-task/include \
	        tests/platform/test_guest_vmm_notifications.c platform/guest-vmm/loop.c \
	        -o $(BUILD_TMP_DIR)/test_guest_vmm_notifications \
	    && $(BUILD_TMP_DIR)/test_guest_vmm_notifications; then :; \
	else status=1; fi; \
	if gcc -std=gnu11 -Wall -Wextra -Werror \
	        -DAGENTOS_GUEST_GRAPHICS -DAGENTOS_GUEST_INPUT \
	        -I tests/platform/teardown-stubs -I platform/include \
	        -iquote kernel/agentos-root-task/include \
	        tests/platform/test_guest_teardown.c platform/guest-vmm/teardown.c \
	        -o $(BUILD_TMP_DIR)/test_guest_teardown \
	    && $(BUILD_TMP_DIR)/test_guest_teardown; then :; \
	else status=1; fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -I libvmm/include \
	        tests/platform/test_virtio_console_tx.c -o $(BUILD_TMP_DIR)/test_virtio_console_tx \
	    && $(BUILD_TMP_DIR)/test_virtio_console_tx; then :; \
	else status=1; fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -I libvmm/include \
	        tests/platform/test_virtio_console_tx_ring.c -o $(BUILD_TMP_DIR)/test_virtio_console_tx_ring \
	    && $(BUILD_TMP_DIR)/test_virtio_console_tx_ring; then :; \
	else status=1; fi; \
	if gcc -I kernel/agentos-root-task/include -I . \
	        tests/platform/test_native_net_client.c \
	        services/legacy-pds/native_net_client.c \
	        -o $(BUILD_TMP_DIR)/test_native_net_client 2>&1 \
	    && $(BUILD_TMP_DIR)/test_native_net_client; then \
	    echo "PASS: tests/platform/test_native_net_client.c"; \
	else \
	    echo "FAIL: tests/platform/test_native_net_client.c"; \
	    status=1; \
	fi; \
	if gcc -I kernel/agentos-root-task/include \
	        tests/platform/test_cc_retry_cache.c \
	        services/command-console/cc_retry_cache.c \
	        -o $(BUILD_TMP_DIR)/test_cc_retry_cache 2>&1 \
	    && $(BUILD_TMP_DIR)/test_cc_retry_cache; then \
	    echo "PASS: tests/platform/test_cc_retry_cache.c"; \
	else \
	    echo "FAIL: tests/platform/test_cc_retry_cache.c"; \
	    status=1; \
	fi; \
	if gcc -std=c11 -Wall -Wextra -Werror -iquote kernel/agentos-root-task/include \
	        -I kernel/agentos-root-task/include/contracts \
	        -idirafter kernel/agentos-root-task/include \
	        tests/platform/test_virtualizer_authority.c \
	        -o $(BUILD_TMP_DIR)/test_virtualizer_authority \
	    && $(BUILD_TMP_DIR)/test_virtualizer_authority; then :; \
	else status=1; fi; \
	if gcc -DAGENTOS_TEST_HOST -include tests/microkit.h \
	        -iquote kernel/agentos-root-task/include \
	        tests/platform/test_cc_vm_client.c \
	        services/command-console/cc_vm_client.c \
	        -o $(BUILD_TMP_DIR)/test_cc_vm_client 2>&1 \
	    && $(BUILD_TMP_DIR)/test_cc_vm_client; then \
	    echo "PASS: tests/platform/test_cc_vm_client.c"; \
	else \
	    echo "FAIL: tests/platform/test_cc_vm_client.c"; \
	    status=1; \
	fi; \
	if gcc -DAGENTOS_GUEST_DUAL -DAGENTOS_GUEST_PRIMARY_LARGE \
	        -I platform/include \
	        tests/platform/test_guest_memory_layout.c \
	        -o $(BUILD_TMP_DIR)/test_guest_memory_layout 2>&1 \
	    && $(BUILD_TMP_DIR)/test_guest_memory_layout; then \
	    echo "PASS: tests/platform/test_guest_memory_layout.c"; \
	else \
	    echo "FAIL: tests/platform/test_guest_memory_layout.c"; \
	    status=1; \
	fi; \
	if gcc -I platform/include \
	        tests/platform/test_blk_virt_pump.c \
	        platform/blk-virt/blk_virt_pump.c \
	        -o $(BUILD_TMP_DIR)/test_blk_virt_pump 2>&1 \
	    && $(BUILD_TMP_DIR)/test_blk_virt_pump; then \
	    echo "PASS: tests/platform/test_blk_virt_pump.c"; \
	else \
	    echo "FAIL: tests/platform/test_blk_virt_pump.c"; \
	    status=1; \
	fi; \
	if gcc -I libvmm/include \
	        tests/platform/test_virtio_blk_chunk.c \
	        -o $(BUILD_TMP_DIR)/test_virtio_blk_chunk 2>&1 \
	    && $(BUILD_TMP_DIR)/test_virtio_blk_chunk; then \
	    echo "PASS: tests/platform/test_virtio_blk_chunk.c"; \
	else \
	    echo "FAIL: tests/platform/test_virtio_blk_chunk.c"; \
	    status=1; \
	fi; \
	if cargo test -p xtask --lib --quiet; then \
	    echo "PASS: xtask focused unit tests"; \
	else \
	    echo "FAIL: xtask focused unit tests"; \
	    status=1; \
	fi; \
	echo ""; \
	echo "Integration tests complete."; \
	echo ""; \
	exit $$status

# =============================================================================
# e2e: End-to-end integration test suite (agentOS + guests + SSH)
# =============================================================================
e2e: e2e-dual-os

e2e-guest:
	@chmod +x tests/e2e/suite_common.sh
	@bash tests/e2e/suite_common.sh

e2e-contract:
	@chmod +x tests/e2e/test_cc_contract.sh
	@BRIDGE_AVAILABLE=1 bash tests/e2e/test_cc_contract.sh

e2e-dual-os:
	@cargo xtask qemu-test --board $(BOARD) --guest-os both --timeout-secs $(DUAL_OS_TEST_TIMEOUT)

# Run the dual authenticated-SSH gate, retain both guests, and print commands
# for manual sessions. Press Enter in this terminal to stop QEMU cleanly.
run-dual-ssh:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os both --timeout-secs $(DUAL_OS_TEST_TIMEOUT) --keep-running

# Compatibility names for supported guest proofs.
e2e-ubuntu-amd64:
	@echo "ERROR: x86_64 guest execution is roadmap work; no release gate exists yet."
	@exit 1

e2e-ubuntu-arm64: test-ubuntu-live

e2e-nixos:
	@echo "ERROR: NixOS is not a supported agentOS guest release target."
	@exit 1

e2e-freebsd15:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os freebsd --assert-live --timeout-secs $(QEMU_TEST_TIMEOUT)

e2e-all: demo-test

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
# release: evidence-bound release workflow
# =============================================================================
release:
	@cargo xtask release plan --bump patch --claim $(or $(RELEASE_CLAIM),os) \
		$(foreach artifact,$(RELEASE_ARTIFACTS),--artifact $(artifact))

release-minor:
	@cargo xtask release plan --bump minor --claim $(or $(RELEASE_CLAIM),os) \
		$(foreach artifact,$(RELEASE_ARTIFACTS),--artifact $(artifact))

release-major:
	@cargo xtask release plan --bump major --claim $(or $(RELEASE_CLAIM),os) \
		$(foreach artifact,$(RELEASE_ARTIFACTS),--artifact $(artifact))

release-prepare:
	@test -n "$(RELEASE_VERSION)" && test -n "$(RELEASE_DATE)" || \
		(echo "Usage: make release-prepare RELEASE_VERSION=X.Y.Z RELEASE_DATE=YYYY-MM-DD" && exit 1)
	@cargo xtask release prepare --version $(RELEASE_VERSION) --date $(RELEASE_DATE)

release-check:
	@test -n "$(RELEASE_VERSION)" || \
		(echo "Usage: make release-check RELEASE_VERSION=X.Y.Z [RELEASE_CLAIM=tooling|os|guests|desktop]" && exit 1)
	@cargo xtask release check --version $(RELEASE_VERSION) --claim $(or $(RELEASE_CLAIM),os) \
		$(foreach artifact,$(RELEASE_ARTIFACTS),--artifact $(artifact))

release-publish:
	@test -n "$(RELEASE_VERSION)" && test -n "$(RELEASE_AUTHORIZE)" || \
		(echo "Usage: make release-publish RELEASE_VERSION=X.Y.Z RELEASE_AUTHORIZE=publish-vX.Y.Z" && exit 1)
	@cargo xtask release publish --version $(RELEASE_VERSION) --authorize $(RELEASE_AUTHORIZE)

release-verify:
	@test -n "$(RELEASE_VERSION)" || \
		(echo "Usage: make release-verify RELEASE_VERSION=X.Y.Z" && exit 1)
	@cargo xtask release verify --version $(RELEASE_VERSION)

PRESENTATION_EDITION ?= dev
PRESENTATION_PDF ?= build/presentations/agentos-systems-security-v$(PRESENTATION_EDITION).pdf

presentation-render:
	@cargo xtask render-deck --edition $(PRESENTATION_EDITION) --output $(PRESENTATION_PDF) \
		$(if $(filter 1,$(PRESENTATION_VISUAL_REVIEW)),--visual-review,)

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
	@echo "  SEL4_SDK        $(SEL4_SDK)"
	@echo "  BUILD_DIR       $(BUILD_DIR)"
	@echo "  AGENTOS_IMAGES  $(AGENTOS_IMAGES)"
	@echo ""
	@echo "Primary targets:"
	@echo "  make help             Show this help text"
	@echo "  make setup            Install host dependencies + shared Microkit SDK"
	@echo "  make demo             Boot, prove, and retain Ubuntu + FreeBSD for SSH"
	@echo "  make demo-test        Run the dual authenticated-SSH proof and exit"
	@echo "  make demo-desktop     Boot, prove, and retain an Ubuntu VNC desktop"
	@echo "  make demo-desktop-test Run the Ubuntu RFB frame proof and exit"
	@echo "  make demo-smoke       Fast host-only checks; no QEMU and not a boot proof"
	@echo "  make demo-check       Validate demo tools and SDK without building"
	@echo "  make test-operator-session   Verify native inspection over serial queues"
	@echo "  make test-operator-isolation Verify operator page-access restrictions"
	@echo "  make demo-clean       Remove demo sockets, logs, and generated SSH keys"
	@echo "  make install          Install host build dependencies (alias: make deps)"
	@echo "  make build            Fetch the selected guest image and build agentOS"
	@echo "  make run              Build native agentOS and boot QEMU with CC-PD socket"
	@echo "                        Uses QEMU_RUN_MEM=3G automatically for GUEST_OS=both"
	@echo "  make run GUEST_OS=buildroot"
	@echo "                        Boot linux_vmm hosting buildroot Linux to a '#' prompt"
	@echo "                        (no outer ISO; guest is packaged inside guest_vmm_primary.elf)"
	@echo "  make run-fast         Same as run, plus multi-threaded TCG"
	@echo "                        No-op on Linux/KVM hosts where HW accel is already on"
	@echo "                        Recommended dev loop on Apple Silicon:"
	@echo "                        make run-fast GUEST_OS=buildroot"
	@echo "  make test             Build and run the QEMU boot/API smoke test"
	@echo "  make gate             MANDATORY dual-arch QEMU gate before OS-level claims"
	@echo "                        (host suite + aarch64 + x86_64 GUEST_OS=none boot tests)"
	@echo "  make test-guest-login Boot Ubuntu and FreeBSD to an interactive serial prompt"
	@echo "  make test-guest-net   Boot buildroot and prove one packet through emulated virtio-net"
	@echo "  make test-guest-blk   Boot buildroot and prove one request through emulated virtio-blk"
	@echo "  make test-guest-console Boot Ubuntu and prove login I/O through emulated virtio-console"
	@echo "  make test-ubuntu-virtio Require Ubuntu login and I/O on agentOS net/blk/console only"
	@echo "  make test-ubuntu-live Boot Ubuntu Casper userspace on agentOS VirtIO only"
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
	@echo "  make test-guest-net   Guest packet proof: emulated virtio-net (buildroot)"
	@echo "  make test-guest-blk   Guest I/O proof: emulated virtio-blk (buildroot)"
	@echo "  make test-guest-console Guest I/O proof: emulated virtio-console (Ubuntu)"
	@echo "  make test-ubuntu-virtio End-state Ubuntu agentOS VirtIO proof"
	@echo "  make test-ubuntu-live Full Ubuntu Casper filesystem/login proof"
	@echo "  make test-integration Run host-side contract/integration tests"
	@echo "  make e2e              Run the default QEMU/guest/CC end-to-end suite"
	@echo "  make e2e-dual-os      Run Ubuntu and FreeBSD guest E2E coverage"
	@echo "  make run-dual-ssh     Keep verified dual guests running for manual SSH"
	@echo "  make demo-desktop-test Prove a tunnel-confined Ubuntu RFB frame"
	@echo "  make e2e-all          Run E2E suites for every staged guest image"
	@echo ""
	@echo "Cleanup/tooling:"
	@echo "  make clean            Remove build artifacts for the selected board"
	@echo "  make clean-all        Remove all build artifacts under build/"
	@echo "  make clean-images     Remove staged guest images"
	@echo "  make build-tools      Build Rust host tools in release mode"
	@echo "  make policy-check     Enforce language/UI policy and xtask formatting"
	@echo "  make lint-source      Source lint for docs/TCB.md I/O invariants (not a test)"
	@echo "  make release          Print a read-only patch-release plan"
	@echo "  make release-prepare/check/publish/verify  Advance explicit release states"
	@echo "  make presentation-render PRESENTATION_EDITION=X.Y.Z  Render and validate the release PDF"
	@echo ""
	@echo "Quick start:"
	@echo "  make setup"
	@echo "  make demo"
	@echo ""
	@echo "Common examples:"
	@echo "  make demo                         # interactive dual-guest SSH showcase"
	@echo "  make demo-test                    # automated dual-guest SSH acceptance"
	@echo "  make demo-desktop                 # interactive Ubuntu desktop over SSH/VNC"
	@echo "  make demo-desktop-test            # automated RFB handshake + frame evidence"
	@echo "  make demo-smoke                   # fast host-only preflight"
	@echo "  make build TARGET_ARCH=aarch64 GUEST_OS=ubuntu"
	@echo "  make build TARGET_ARCH=aarch64 GUEST_OS=both"
	@echo "  make run GUEST_OS=freebsd"
	@echo "  make run-fast GUEST_OS=buildroot   # fast dev loop on Apple Silicon"
	@echo "  make gate                          # full release gate, both arches"
	@echo "  make test-guest-login QEMU_TEST_TIMEOUT=420"
	@echo "  make test-guest-net QEMU_TEST_TIMEOUT=480"
	@echo "  make test-guest-blk QEMU_TEST_TIMEOUT=480"
	@echo "  make test-guest-console QEMU_TEST_TIMEOUT=480"
	@echo "  make test-ubuntu-virtio QEMU_TEST_TIMEOUT=480"
	@echo "  make test-ubuntu-live QEMU_TEST_TIMEOUT=3600"
	@echo "  cd ../agentos_gui && make run"
	@echo ""
