# kernel/loader/Makefile — agentOS pre-seL4 ELF loader
#
# Builds loader.elf: a standalone bare-metal binary that QEMU loads as a
# proper ELF.  It reads agentos.img (placed in DRAM by a second QEMU loader
# device), copies the seL4 kernel and root_task ELFs to their physical
# addresses, sets up the bootstrap page tables, enables paging, and jumps to
# the seL4 kernel.
#
# Two architectures are built here; everything else is skipped silently.
#   aarch64  start.S + mmu.S + main.c          — EL2, 4-level LPAE, TTBR0_EL2
#   riscv64  start_riscv64.S + main_riscv64.c  — S-mode (OpenSBI ran already),
#                                                Sv39 gigapages, satp
#
# Usage (called from kernel/agentos-root-task/Makefile):
#   $(MAKE) -C kernel/loader BUILD_DIR=<path> AGENTOS_ARCH=<aarch64|riscv64>

BUILD_DIR     ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../_build/loader)
AGENTOS_ARCH  ?= aarch64

LOADER_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# Architectures without a loader arm skip silently.
ifeq ($(filter $(AGENTOS_ARCH),aarch64 riscv64),)
all:
	@echo "[loader] skipping (arch=$(AGENTOS_ARCH))"
clean:
	@true
.PHONY: all clean
else

# ── Toolchain ──────────────────────────────────────────────────────────────
# Prefer Homebrew LLVM on macOS to ensure we have a working cross toolchain
# regardless of what the system clang supports.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  _BREW_CLANG := $(shell \
    for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do \
      [ -x "$$d/clang" ] && echo "$$d" && break; \
    done 2>/dev/null)
  ifneq ($(_BREW_CLANG),)
    CLANG := $(_BREW_CLANG)/clang
  else
    CLANG := clang
  endif
  # ld.lld may live in lld's own keg or in the main bin
  _BREW_LLD := $(shell \
    for d in /opt/homebrew/opt/llvm/bin /opt/homebrew/bin /usr/local/opt/llvm/bin /usr/local/bin; do \
      [ -x "$$d/ld.lld" ] && echo "$$d/ld.lld" && break; \
    done 2>/dev/null)
  ifneq ($(_BREW_LLD),)
    LD := $(_BREW_LLD)
  else
    LD := ld.lld
  endif
else
  CLANG := clang
  LD    := ld.lld
endif

OBJCOPY ?= llvm-objcopy

# ── Per-architecture sources, flags and linker script ──────────────────────
ifeq ($(AGENTOS_ARCH),aarch64)

CC          := $(CLANG) -target aarch64-unknown-elf
CFLAGS_ARCH := -mstrict-align
LOADER_LD   := $(LOADER_DIR)/loader.ld
SRCS_C      := $(LOADER_DIR)/main.c
SRCS_S      := $(LOADER_DIR)/start.S $(LOADER_DIR)/mmu.S
HDRS        := $(LOADER_DIR)/page_tables.h

else

# riscv64: no compressed instructions are required, but rv64imac matches the
# SDK's own loader.  -mcmodel=medany is mandatory: the loader is linked at
# 0x81000000 and medlow can only reach the low 2 GiB.
CC          := $(CLANG) -target riscv64-unknown-elf
CFLAGS_ARCH := -march=rv64imac_zicsr_zifencei -mabi=lp64 -mcmodel=medany
LOADER_LD   := $(LOADER_DIR)/loader_riscv64.ld
SRCS_C      := $(LOADER_DIR)/main_riscv64.c
SRCS_S      := $(LOADER_DIR)/start_riscv64.S
HDRS        :=

endif

CFLAGS := \
    -nostdlib -ffreestanding \
    -g -O2 \
    -Wall -Wextra \
    -Wno-unused-parameter \
    $(CFLAGS_ARCH) \
    -I$(LOADER_DIR)

OBJS_C   := $(patsubst $(LOADER_DIR)/%.c, $(BUILD_DIR)/loader_%.o, $(SRCS_C))
OBJS_S   := $(patsubst $(LOADER_DIR)/%.S, $(BUILD_DIR)/loader_%.o, $(SRCS_S))
OBJS     := $(OBJS_S) $(OBJS_C)

LOADER_ELF := $(BUILD_DIR)/loader.elf

# ── Rules ──────────────────────────────────────────────────────────────────

all: $(LOADER_ELF)

$(LOADER_ELF): $(OBJS) $(LOADER_LD)
	@mkdir -p $(BUILD_DIR)
	$(LD) -T$(LOADER_LD) $(OBJS) -o $@
	@echo "[loader] built $@ (arch=$(AGENTOS_ARCH))"

$(BUILD_DIR)/loader_%.o: $(LOADER_DIR)/%.c $(HDRS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/loader_%.o: $(LOADER_DIR)/%.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJS) $(LOADER_ELF)

.PHONY: all clean

endif
