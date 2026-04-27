# kernel/loader/Makefile — AArch64 pre-seL4 ELF loader
#
# Builds loader.elf: a standalone AArch64 bare-metal binary that QEMU loads
# as a proper ELF.  It reads agentos.img (loaded by QEMU at 0x48000000),
# copies the seL4 kernel and root_task ELFs to their physical addresses,
# sets up EL2 page tables, enables the MMU, and jumps to the seL4 kernel.
#
# Usage (called from kernel/agentos-root-task/Makefile):
#   $(MAKE) -C kernel/loader BUILD_DIR=<path> AGENTOS_ARCH=aarch64

BUILD_DIR     ?= /tmp/agentos-loader-build
AGENTOS_ARCH  ?= aarch64

# This loader is AArch64-only; skip silently on other architectures.
ifneq ($(AGENTOS_ARCH),aarch64)
all:
	@echo "[loader] skipping (arch=$(AGENTOS_ARCH))"
clean:
	@true
.PHONY: all clean
else

LOADER_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

# ── Toolchain ──────────────────────────────────────────────────────────────
# Prefer Homebrew LLVM on macOS to ensure we have a working AArch64 cross
# toolchain regardless of what the system clang supports.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  _BREW_CLANG := $(shell \
    for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin; do \
      [ -x "$$d/clang" ] && echo "$$d" && break; \
    done 2>/dev/null)
  ifneq ($(_BREW_CLANG),)
    CC := $(_BREW_CLANG)/clang -target aarch64-unknown-elf
  else
    CC := clang -target aarch64-unknown-elf
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
  CC  := clang -target aarch64-unknown-elf
  LD  := ld.lld
endif

OBJCOPY ?= llvm-objcopy

CFLAGS := \
    -nostdlib -ffreestanding \
    -g -O2 \
    -Wall -Wextra \
    -Wno-unused-parameter \
    -mstrict-align \
    -I$(LOADER_DIR)

LOADER_LD := $(LOADER_DIR)/loader.ld

# ── Sources ────────────────────────────────────────────────────────────────
SRCS_C   := $(LOADER_DIR)/main.c
SRCS_S   := $(LOADER_DIR)/start.S $(LOADER_DIR)/mmu.S

OBJS_C   := $(patsubst $(LOADER_DIR)/%.c, $(BUILD_DIR)/loader_%.o, $(SRCS_C))
OBJS_S   := $(patsubst $(LOADER_DIR)/%.S, $(BUILD_DIR)/loader_%.o, $(SRCS_S))
OBJS     := $(OBJS_S) $(OBJS_C)

LOADER_ELF := $(BUILD_DIR)/loader.elf

# ── Rules ──────────────────────────────────────────────────────────────────

all: $(LOADER_ELF)

$(LOADER_ELF): $(OBJS) $(LOADER_LD)
	@mkdir -p $(BUILD_DIR)
	$(LD) -T$(LOADER_LD) $(OBJS) -o $@
	@echo "[loader] built $@"

$(BUILD_DIR)/loader_%.o: $(LOADER_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/loader_%.o: $(LOADER_DIR)/%.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJS) $(LOADER_ELF)

.PHONY: all clean

endif
