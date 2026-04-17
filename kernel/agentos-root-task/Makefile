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

CFLAGS := -nostdlib -ffreestanding -g -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
           -I$(BOARD_DIR)/include -Iinclude -Ilwip/include \
           $(CFLAGS_ARCH) \
           -MMD -MP

# wasm3-specific flags: suppress warnings from vendored code, include bare-metal shim
CFLAGS_WASM3 := $(CFLAGS) -Iwasm3 -include $(CURDIR)/include/m3_bare_metal.h \
                -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers \
                -Wno-cast-align -Wno-implicit-fallthrough -Dd_m3HasWASI=0

# Swap slot needs wasm3 headers but NOT the bare-metal shim pre-include
# (the shim is only for vendored wasm3 sources that lack seL4 headers)
CFLAGS_SWAPSLOT := $(CFLAGS) -Iwasm3

LDFLAGS := -L$(BOARD_DIR)/lib
LIBS    := -lmicrokit -Tmicrokit.ld

# ─── Board-specific UART config for console_shell ────────────────────────────
# Passed down from the top-level Makefile via boards/<BOARD_NAME>/board.mk.
# Defaults are empty (ring-buffer-only mode for QEMU builds).
BOARD_NAME      ?=
BOARD_NATIVE    ?= 0
BOARD_UART_PHYS ?=
BOARD_UART_SIZE ?=
BOARD_UART_TYPE ?=
BOARD_UART_IRQ  ?=

CFLAGS_CONSOLESHELL := $(CFLAGS)
ifeq ($(BOARD_UART_TYPE),pl011)
  CFLAGS_CONSOLESHELL += -DBOARD_UART_PL011=1
else ifeq ($(BOARD_UART_TYPE),ns16550)
  CFLAGS_CONSOLESHELL += -DBOARD_UART_NS16550=1
endif
ifneq ($(BOARD_UART_PHYS),)
  CFLAGS_CONSOLESHELL += \
    -DBOARD_UART_PHYS=$(BOARD_UART_PHYS) \
    -DBOARD_UART_SIZE=$(BOARD_UART_SIZE) \
    -DBOARD_UART_IRQ=$(BOARD_UART_IRQ)
endif
ifeq ($(BOARD_NATIVE),1)
  CFLAGS_CONSOLESHELL += -DBOARD_NATIVE=1
endif

# Protection Domain ELFs
# monitor/controller: includes vibe_swap for the hot-swap controller-side logic
# m3_bare_metal.c provides memset/memcpy/memmove needed by compiler-generated code
PD_MONITOR_SRCS   := src/monitor.c src/cap_broker.c src/agent_pool.c src/vibe_swap.c src/m3_bare_metal.c src/log.c src/boot_integrity.c src/ed25519_verify.c src/monocypher.c src/cap_policy.c src/verify.c
PD_EVENTBUS_SRCS  := src/event_bus.c src/m3_bare_metal.c src/log.c
PD_INITAGENT_SRCS := src/init_agent.c src/m3_bare_metal.c src/log.c
PD_WORKER_SRCS    := src/worker.c src/m3_bare_metal.c src/log.c
# agentfs needs m3_bare_metal for memset/sqrtf (no wasm3 headers needed)
PD_AGENTFS_SRCS     := src/agentfs.c src/m3_bare_metal.c src/log.c
PD_VIBEENGINE_SRCS   := src/vibe_engine.c src/m3_bare_metal.c src/log.c
PD_SWAPSLOT_SRCS     := src/swap_slot.c src/wasm3_host.c src/m3_bare_metal.c src/log.c
PD_LOG_DRAIN_SRCS    := src/log_drain.c src/m3_bare_metal.c src/log.c
PD_MEMPROFILER_SRCS  := src/mem_profiler.c src/m3_bare_metal.c src/log.c
PD_NETISOLATOR_SRCS      := src/net_isolator.c src/m3_bare_metal.c src/log.c
PD_SNAPSHOTSCHED_SRCS    := src/snapshot_sched.c src/m3_bare_metal.c src/log.c
PD_PERFCOUNTERS_SRCS     := src/perf_counters.c src/m3_bare_metal.c src/log.c
PD_TIMEPARTITION_SRCS    := src/time_partition.c src/m3_bare_metal.c src/log.c
PD_TRACERECORDER_SRCS    := src/trace_recorder.c src/m3_bare_metal.c src/log.c
PD_NAMESERVER_SRCS       := src/nameserver.c src/m3_bare_metal.c src/log.c
PD_VIRTIO_BLK_SRCS       := src/virtio_blk.c src/m3_bare_metal.c src/log.c
PD_VFS_SERVER_SRCS       := src/vfs_server.c src/m3_bare_metal.c src/log.c
PD_SPAWN_SERVER_SRCS     := src/spawn_server.c src/sha256_mini.c src/m3_bare_metal.c src/log.c
PD_APP_SLOT_SRCS         := src/app_slot.c src/sha256_mini.c src/m3_bare_metal.c src/log.c
PD_NET_SERVER_SRCS       := src/net_server.c src/lwip_sys.c src/lwip_shim.c src/m3_bare_metal.c src/log.c
PD_NET_PD_SRCS           := src/net_pd.c src/lwip_sys.c src/lwip_shim.c src/m3_bare_metal.c src/log.c
PD_JS_RUNTIME_SRCS       := src/js_runtime.c src/m3_bare_metal.c src/log.c
PD_JS_RUNTIME_VENDOR_SRCS := vendor/quickjs/quickjs_stub.c
PD_WG_NET_SRCS           := src/wg_net.c src/monocypher.c src/m3_bare_metal.c src/log.c
PD_APP_MANAGER_SRCS      := src/app_manager.c src/m3_bare_metal.c src/log.c
PD_HTTP_SVC_SRCS         := src/http_svc.c src/m3_bare_metal.c src/log.c
PD_VM_MANAGER_SRCS       := src/vm_manager.c src/vmm_mux_stub.c src/m3_bare_metal.c src/log.c
PD_AUTH_SERVER_SRCS      := src/auth_server.c src/m3_bare_metal.c src/log.c
PD_EXT2FS_SRCS           := src/ext2fs.c src/m3_bare_metal.c src/log.c
PD_PROC_SERVER_SRCS      := src/proc_server.c src/m3_bare_metal.c src/log.c
PD_VM_SNAPSHOT_SRCS      := src/vm_snapshot.c src/m3_bare_metal.c src/log.c
PD_PFLOCAL_SERVER_SRCS   := src/pflocal_server.c src/m3_bare_metal.c src/log.c
PD_EXEC_SERVER_SRCS      := src/exec_server.c src/m3_bare_metal.c src/log.c
PD_TERM_SERVER_SRCS      := src/term_server.c src/m3_bare_metal.c src/log.c
PD_CONSOLESHELL_SRCS     := src/console_shell.c src/m3_bare_metal.c src/log.c

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
LOG_DRAIN_OBJS     := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_LOG_DRAIN_SRCS))
SWAPSLOT_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_SWAPSLOT_SRCS))
MEMPROFILER_OBJS   := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_MEMPROFILER_SRCS))
NETISOLATOR_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_NETISOLATOR_SRCS))
SNAPSHOTSCHED_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_SNAPSHOTSCHED_SRCS))
PERFCOUNTERS_OBJS     := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_PERFCOUNTERS_SRCS))
TIMEPARTITION_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_TIMEPARTITION_SRCS))
TRACERECORDER_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_TRACERECORDER_SRCS))
NAMESERVER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_NAMESERVER_SRCS))
VIRTIO_BLK_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VIRTIO_BLK_SRCS))
VFS_SERVER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VFS_SERVER_SRCS))
SPAWN_SERVER_OBJS     := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_SPAWN_SERVER_SRCS))
APP_SLOT_OBJS         := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_APP_SLOT_SRCS))
NET_SERVER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_NET_SERVER_SRCS))
NET_PD_OBJS           := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_NET_PD_SRCS))
JS_RUNTIME_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_JS_RUNTIME_SRCS)) \
                         $(patsubst vendor/%.c, $(BUILD_DIR)/vendor_%.o, $(PD_JS_RUNTIME_VENDOR_SRCS))
WG_NET_OBJS           := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_WG_NET_SRCS))
APP_MANAGER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_APP_MANAGER_SRCS))
HTTP_SVC_OBJS         := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_HTTP_SVC_SRCS))
VM_MANAGER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VM_MANAGER_SRCS))
AUTH_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_AUTH_SERVER_SRCS))
EXT2FS_OBJS           := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EXT2FS_SRCS))
PROC_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_PROC_SERVER_SRCS))
VM_SNAPSHOT_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VM_SNAPSHOT_SRCS))
PFLOCAL_SERVER_OBJS   := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_PFLOCAL_SERVER_SRCS))
EXEC_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EXEC_SERVER_SRCS))
TERM_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_TERM_SERVER_SRCS))
WASM3_OBJS            := $(patsubst wasm3/%.c, $(BUILD_DIR)/wasm3_%.o, $(WASM3_SRCS))
CONSOLESHELL_OBJS     := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_CONSOLESHELL_SRCS))

IMAGES := controller.elf event_bus.elf init_agent.elf worker.elf agentfs.elf vibe_engine.elf log_drain.elf swap_slot.elf mem_profiler.elf net_isolator.elf perf_counters.elf time_partition.elf trace_recorder.elf nameserver.elf virtio_blk.elf vfs_server.elf spawn_server.elf app_slot.elf net_server.elf net_pd.elf js_runtime.elf wg_net.elf app_manager.elf http_svc.elf vm_manager.elf auth_server.elf ext2fs.elf proc_server.elf vm_snapshot.elf pflocal_server.elf exec_server.elf term_server.elf console_shell.elf

# Optional: periodic WASM slot snapshot scheduler
ifdef AGENTOS_SNAPSHOT_SCHED
  CFLAGS += -DAGENTOS_SNAPSHOT_SCHED=1
  IMAGES += snapshot_sched.elf
endif

# Guest OS selection: none (default) | linux | freebsd
GUEST_OS ?= none

# AArch64: include VMM based on GUEST_OS
ifeq ($(ARCH),aarch64)
  ifeq ($(GUEST_OS),freebsd)
    IMAGES      += freebsd_vmm.elf
    SYSTEM_FILE := ../../manifests/agentos-freebsd.system
  else
    # Default aarch64: include Linux VMM (stub or full)
    IMAGES      += linux_vmm.elf
    SYSTEM_FILE := agentos-aarch64.system
  endif
else ifeq ($(ARCH),x86_64)
  # Include linux_vmm stub so the system description can reference it
  IMAGES      += linux_vmm.elf
  SYSTEM_FILE := ../../manifests/agentos-linux-x86.system
else
  SYSTEM_FILE := agentos.system
endif

# Board-specific system file override: native hardware targets supply their own
# agentos.system in boards/<BOARD_NAME>/agentos.system (repo root).
ifneq ($(BOARD_NAME),)
  _BOARD_SYSTEM := ../../boards/$(BOARD_NAME)/agentos.system
  ifneq ($(wildcard $(_BOARD_SYSTEM)),)
    SYSTEM_FILE := $(_BOARD_SYSTEM)
  endif
endif

IMAGE_FILE  := $(BUILD_DIR)/agentos.img
REPORT_FILE := $(BUILD_DIR)/agentos-report.txt

build: $(IMAGE_FILE)

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

# Vendored QuickJS stub
$(BUILD_DIR)/vendor_quickjs/%.o: vendor/quickjs/%.c
	@mkdir -p $(BUILD_DIR)/vendor_quickjs
	$(CC) -c $(CFLAGS) $< -o $@

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

$(BUILD_DIR)/log_drain.elf: $(LOG_DRAIN_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/swap_slot.elf: $(SWAPSLOT_OBJS) $(WASM3_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/mem_profiler.elf: $(MEMPROFILER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_isolator.elf: $(NETISOLATOR_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/snapshot_sched.elf: $(SNAPSHOTSCHED_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/perf_counters.elf: $(PERFCOUNTERS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/time_partition.elf: $(TIMEPARTITION_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/trace_recorder.elf: $(TRACERECORDER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/nameserver.elf: $(NAMESERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/virtio_blk.elf: $(VIRTIO_BLK_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vfs_server.elf: $(VFS_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/spawn_server.elf: $(SPAWN_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/app_slot.elf: $(APP_SLOT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_server.elf: $(NET_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/net_pd.elf: $(NET_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/js_runtime.elf: $(JS_RUNTIME_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/wg_net.elf: $(WG_NET_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/app_manager.elf: $(APP_MANAGER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/http_svc.elf: $(HTTP_SVC_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# vm_manager: multi-VM lifecycle manager (wraps vmm_mux; AArch64 libvmm optional)
# Compiled with -I../freebsd-vmm so vmm_mux.h resolves, and -I../../libvmm/include
# so the transitive <libvmm/guest.h> include resolves on AArch64 builds.
$(BUILD_DIR)/vm_manager.o: src/vm_manager.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -I../freebsd-vmm -I../../libvmm/include $< -o $@

# vmm_mux_stub also needs both paths for the same reason
$(BUILD_DIR)/vmm_mux_stub.o: src/vmm_mux_stub.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -I../freebsd-vmm -I../../libvmm/include $< -o $@

$(BUILD_DIR)/vm_manager.elf: $(VM_MANAGER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/auth_server.elf: $(AUTH_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/ext2fs.elf: $(EXT2FS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/proc_server.elf: $(PROC_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/vm_snapshot.elf: $(VM_SNAPSHOT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/pflocal_server.elf: $(PFLOCAL_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/exec_server.elf: $(EXEC_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/term_server.elf: $(TERM_SERVER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# console_shell uses board-specific UART CFLAGS
$(BUILD_DIR)/console_shell.o: src/console_shell.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_CONSOLESHELL) $< -o $@

$(BUILD_DIR)/console_shell.elf: $(CONSOLESHELL_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# ─── Linux VMM (AArch64 only) ─────────────────────────────────────────────
ifeq ($(ARCH),aarch64)
ifneq ($(GUEST_OS),freebsd)
ifeq ($(BOARD_NATIVE),1)
# Native AArch64 board (e.g., RPi5): libvmm has QEMU-specific GIC addresses;
# use a stub instead until board-specific VMM support is added.
$(BUILD_DIR)/linux_vmm.o: src/linux_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DLINUX_VMM_NATIVE_STUB=1 $< -o $@

$(BUILD_DIR)/linux_vmm.elf: $(BUILD_DIR)/linux_vmm.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
else
# QEMU AArch64: use libvmm for full Linux VM support
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
endif
endif
endif

# ─── FreeBSD VMM (AArch64 + GUEST_OS=freebsd) ─────────────────────────────
ifeq ($(ARCH),aarch64)
ifeq ($(GUEST_OS),freebsd)
FREEBSD_VMM_DIR := $(abspath $(CURDIR)/../freebsd-vmm)

$(BUILD_DIR)/freebsd_vmm.elf: FORCE
	@echo ""
	@echo "═══ Building FreeBSD VMM (libvmm + EDK2) ═══"
	$(MAKE) -C $(FREEBSD_VMM_DIR) \
		BUILD_DIR=$(BUILD_DIR) \
		MICROKIT_SDK=$(MICROKIT_SDK) \
		BOARD=$(MICROKIT_BOARD) \
		all
	@echo "═══ freebsd_vmm.elf ✓ ═══"
endif
endif

# ─── Linux VMM stub (x86_64) ──────────────────────────────────────────────
# Compiled directly from linux_vmm.c with -DARCH_X86_64.
# No libvmm dependency — stub implementation only.
ifeq ($(ARCH),x86_64)
$(BUILD_DIR)/linux_vmm.o: src/linux_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -DARCH_X86_64 -DLINUX_VMM_X86_STUB=1 $< -o $@

$(BUILD_DIR)/linux_vmm.elf: $(BUILD_DIR)/linux_vmm.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
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

.PHONY: build clean FORCE

# ─── Auto-generated header dependency files ───────────────────────────────────
# -MMD -MP in CFLAGS causes clang to emit a .d file alongside each .o.
# Including them here makes 'make' recompile any .o whose headers changed.
-include $(wildcard $(BUILD_DIR)/*.d)
