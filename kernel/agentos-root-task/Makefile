# agentOS Root Task Makefile
# This builds the agentOS personality layer that boots on top of seL4/Microkit

ifeq ($(strip $(BUILD_DIR)),)
$(error BUILD_DIR must be specified)
endif

ifeq ($(strip $(MICROKIT_SDK)),)
$(error MICROKIT_SDK must be specified)
endif

ifeq ($(strip $(MICROKIT_BOARD)),)
$(error MICROKIT_BOARD must be specified)
endif

MICROKIT_CONFIG ?= debug
MICROKIT_BOARD  ?= qemu_virt_riscv64

BOARD_DIR := $(MICROKIT_SDK)/board/$(MICROKIT_BOARD)/$(MICROKIT_CONFIG)
ARCH      := ${shell grep 'CONFIG_SEL4_ARCH  ' $(BOARD_DIR)/include/kernel/gen_config.h | cut -d' ' -f4}

ifeq ($(ARCH),riscv64)
  CC     := clang -target riscv64-unknown-elf
  LD     := ld.lld
  CFLAGS_ARCH := -march=rv64imafdc_zicsr_zifencei -mabi=lp64d
else ifeq ($(ARCH),x86_64)
  CC     := clang -target x86_64-unknown-elf
  LD     := ld.lld
  CFLAGS_ARCH := -march=x86-64
else ifeq ($(ARCH),aarch64)
  CC     := clang -target aarch64-unknown-elf
  LD     := ld.lld
  CFLAGS_ARCH := -mstrict-align
else
  $(error Unsupported arch: $(ARCH))
endif

MICROKIT_TOOL ?= $(MICROKIT_SDK)/bin/microkit

CFLAGS := -nostdlib -ffreestanding -g -O2 -Wall -Wextra -Wno-unused-function \
           -I$(BOARD_DIR)/include -Iinclude \
           $(CFLAGS_ARCH)

# wasm3-specific flags: suppress warnings from vendored code, include bare-metal shim
CFLAGS_WASM3 := $(CFLAGS) -Iwasm3 -include $(CURDIR)/include/m3_bare_metal.h \
                -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers \
                -Wno-cast-align -Wno-implicit-fallthrough -Dd_m3HasWASI=0

# Swap slot needs wasm3 headers but NOT the bare-metal shim pre-include
# (the shim is only for vendored wasm3 sources that lack seL4 headers)
CFLAGS_SWAPSLOT := $(CFLAGS) -Iwasm3

LDFLAGS := -L$(BOARD_DIR)/lib
LIBS    := -lmicrokit -Tmicrokit.ld

# Protection Domain ELFs
# monitor/controller: includes vibe_swap for the hot-swap controller-side logic
# m3_bare_metal.c provides memset/memcpy/memmove needed by compiler-generated code
PD_MONITOR_SRCS   := src/monitor.c src/cap_broker.c src/agent_pool.c src/vibe_swap.c src/m3_bare_metal.c src/log.c
PD_EVENTBUS_SRCS  := src/event_bus.c src/m3_bare_metal.c src/log.c
PD_INITAGENT_SRCS := src/init_agent.c src/m3_bare_metal.c src/log.c
PD_WORKER_SRCS    := src/worker.c src/m3_bare_metal.c src/log.c
# agentfs needs m3_bare_metal for memset/sqrtf (no wasm3 headers needed)
PD_AGENTFS_SRCS     := src/agentfs.c src/m3_bare_metal.c src/log.c
PD_VIBEENGINE_SRCS   := src/vibe_engine.c src/m3_bare_metal.c src/log.c
PD_SWAPSLOT_SRCS     := src/swap_slot.c src/wasm3_host.c src/m3_bare_metal.c src/log.c
PD_CONSOLEMUX_SRCS   := src/console_mux.c src/m3_bare_metal.c src/log.c
PD_MEMPROFILER_SRCS  := src/mem_profiler.c src/m3_bare_metal.c src/log.c

# wasm3 vendored sources (core interpreter only)
WASM3_SRCS := wasm3/m3_core.c wasm3/m3_env.c wasm3/m3_code.c wasm3/m3_compile.c \
              wasm3/m3_exec.c wasm3/m3_bind.c wasm3/m3_parse.c wasm3/m3_module.c \
              wasm3/m3_function.c wasm3/m3_info.c

CONTROLLER_OBJS   := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_MONITOR_SRCS))
EVENTBUS_OBJS     := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EVENTBUS_SRCS))
INITAGENT_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_INITAGENT_SRCS))
WORKER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_WORKER_SRCS))
AGENTFS_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_AGENTFS_SRCS))
VIBEENGINE_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VIBEENGINE_SRCS))
CONSOLEMUX_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_CONSOLEMUX_SRCS))
SWAPSLOT_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_SWAPSLOT_SRCS))
MEMPROFILER_OBJS   := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_MEMPROFILER_SRCS))
WASM3_OBJS         := $(patsubst wasm3/%.c, $(BUILD_DIR)/wasm3_%.o, $(WASM3_SRCS))

IMAGES := controller.elf event_bus.elf init_agent.elf worker.elf agentfs.elf vibe_engine.elf console_mux.elf swap_slot.elf mem_profiler.elf

# AArch64 builds include the Linux VMM
ifeq ($(ARCH),aarch64)
  IMAGES += linux_vmm.elf
  SYSTEM_FILE := agentos-aarch64.system
else ifeq ($(ARCH),x86_64)
  SYSTEM_FILE := agentos-x86_64.system
else
  SYSTEM_FILE := agentos.system
endif

IMAGE_FILE  := $(BUILD_DIR)/agentos.img
REPORT_FILE := $(BUILD_DIR)/agentos-report.txt

all: $(IMAGE_FILE)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# Swap slot sources need wasm3 headers + bare-metal shim
$(BUILD_DIR)/swap_slot.o: src/swap_slot.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SWAPSLOT) $< -o $@

$(BUILD_DIR)/wasm3_host.o: src/wasm3_host.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SWAPSLOT) $< -o $@

$(BUILD_DIR)/m3_bare_metal.o: src/m3_bare_metal.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# Vendored wasm3 sources — compiled with bare-metal shim
$(BUILD_DIR)/wasm3_%.o: wasm3/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_WASM3) $< -o $@

$(BUILD_DIR)/controller.elf: $(CONTROLLER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/event_bus.elf: $(EVENTBUS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/init_agent.elf: $(INITAGENT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/worker.elf: $(WORKER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/agentfs.elf: $(AGENTFS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vibe_engine.elf: $(VIBEENGINE_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/console_mux.elf: $(CONSOLEMUX_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/swap_slot.elf: $(SWAPSLOT_OBJS) $(WASM3_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/mem_profiler.elf: $(MEMPROFILER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# ─── Linux VMM (AArch64 only) ─────────────────────────────────────────────
ifeq ($(ARCH),aarch64)
$(BUILD_DIR)/linux_vmm.elf: FORCE
	@echo ""
	@echo "═══ Building Linux VMM (libvmm) ═══"
	$(MAKE) -f vmm.mk \
		BUILD_DIR=$(BUILD_DIR) \
		MICROKIT_SDK=$(MICROKIT_SDK) \
		MICROKIT_BOARD=$(MICROKIT_BOARD) \
		MICROKIT_CONFIG=$(MICROKIT_CONFIG) \
		BOARD_DIR=$(BOARD_DIR) \
		vmm-all
FORCE:
endif

$(IMAGE_FILE) $(REPORT_FILE): $(addprefix $(BUILD_DIR)/, $(IMAGES)) $(SYSTEM_FILE)
	$(MICROKIT_TOOL) $(SYSTEM_FILE) \
		--search-path $(BUILD_DIR) \
		--board $(MICROKIT_BOARD) \
		--config $(MICROKIT_CONFIG) \
		-o $(IMAGE_FILE) \
		-r $(REPORT_FILE)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
