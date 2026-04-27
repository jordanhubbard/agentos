# agentOS Root Task Makefile
# This builds the agentOS personality layer that boots on top of seL4.
# Image packing uses cargo xtask gen-image (no external Microkit SDK required).

ifeq ($(strip $(BUILD_DIR)),)
$(error BUILD_DIR must be specified)
endif

# AGENTOS_ARCH: target architecture; derived from AGENTOS_BOARD when not set
AGENTOS_ARCH  ?= riscv64
AGENTOS_BOARD ?= qemu_virt_riscv64

# seL4 kernel headers: shipped in the Microkit SDK board package.
# We use these headers only — libmicrokit is NOT linked.
# The board package provides arch-specific seL4 API headers (sel4/sel4.h, etc.)
REPO_ROOT_MK  := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SEL4_SDK      ?= $(REPO_ROOT_MK)/microkit-sdk-2.1.0
SEL4_INC_DIR  := $(SEL4_SDK)/board/$(AGENTOS_BOARD)/release/include

# Linker script: agentOS-owned replacement for microkit.ld
AGENTOS_LD    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../tools/ld/agentos.ld)

ARCH := $(AGENTOS_ARCH)

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

CFLAGS := -nostdlib -ffreestanding -g -O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
           -Iinclude -Ilwip/include -I$(SEL4_INC_DIR) \
           $(CFLAGS_ARCH) \
           -DCONFIG_KERNEL_MCS \
           -MMD -MP

# wasm3-specific flags: suppress warnings from vendored code, include bare-metal shim
CFLAGS_WASM3 := $(CFLAGS) -Iwasm3 -include $(CURDIR)/include/m3_bare_metal.h \
                -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers \
                -Wno-cast-align -Wno-implicit-fallthrough -Dd_m3HasWASI=0

# Swap slot needs wasm3 headers but NOT the bare-metal shim pre-include
# (the shim is only for vendored wasm3 sources that lack seL4 headers)
CFLAGS_SWAPSLOT := $(CFLAGS) -Iwasm3

LDFLAGS :=
LIBS    := -T$(AGENTOS_LD)

# Repository root — two levels above this Makefile (kernel/agentos-root-task/)
REPO_ROOT    := $(abspath $(CURDIR)/../..)
SERVICES_DIR := $(REPO_ROOT)/services

# Extended CFLAGS for service PDs that reference top-level contracts/ headers
# (e.g. "contracts/vibeos/interface.h" resolved from repo root)
CFLAGS_SERVICES := $(CFLAGS) -I$(REPO_ROOT)

# AArch64 loader and custom root_task linker script
LOADER_DIR      := $(REPO_ROOT)/kernel/loader
ROOT_TASK_LD    := $(REPO_ROOT)/tools/ld/root_task.ld
# Resolve llvm-objcopy: prefer the same LLVM installation that provides CC/LD.
# On macOS, llvm-objcopy lives alongside clang in the Homebrew LLVM keg.
ifeq ($(UNAME_S),Darwin)
  UNAME_S := $(shell uname -s)
  _BREW_LLVM := $(shell \
    for d in /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin \
              /opt/homebrew/Cellar/llvm/*/bin /usr/local/Cellar/llvm/*/bin; do \
      [ -x "$$d/llvm-objcopy" ] && echo "$$d/llvm-objcopy" && break; \
    done 2>/dev/null)
  ifneq ($(_BREW_LLVM),)
    OBJCOPY := $(_BREW_LLVM)
  else
    OBJCOPY ?= llvm-objcopy
  endif
else
  OBJCOPY ?= llvm-objcopy
endif

# ─── Root task seL4 boot binary ──────────────────────────────────────────────
# main.c is the seL4 root task entry point and boot sequence orchestrator.
# It is NOT a Microkit PD — it links without libmicrokit and uses its own
# minimal linker flags.  The resulting root_task.elf is loaded directly by
# the seL4 kernel as the first user-mode process.
#
# Board-specific defines passed to main.c so it selects the correct
# system_desc_<arch>.c at compile time:
#   -DBOARD_qemu_virt_aarch64  (for AArch64 QEMU)
#   (default: RISC-V — no define needed; system_desc_riscv64 is the fallback)
ROOT_TASK_SRCS := src/main.c src/ep_alloc.c src/cap_accounting.c \
                  src/pd_tcb.c src/m3_bare_metal.c \
                  src/sel4_crt.c src/log.c \
                  src/ut_alloc.c src/pd_vspace.c src/cap_audit.c \
                  src/vos_create.c src/pd_bundle_stub.c

ifeq ($(ARCH),aarch64)
ROOT_TASK_SRCS += src/system_desc_aarch64.c
CFLAGS_ROOT_TASK := $(CFLAGS) -DBOARD_$(AGENTOS_BOARD)
ROOT_TASK_ASM_OBJS := $(BUILD_DIR)/rt_start_aarch64.o
else
ROOT_TASK_SRCS += src/system_desc_riscv64.c
CFLAGS_ROOT_TASK := $(CFLAGS)
ROOT_TASK_ASM_OBJS :=
endif

ROOT_TASK_OBJS := $(patsubst src/%.c, $(BUILD_DIR)/rt_%.o, $(ROOT_TASK_SRCS))

# Compile root task objects with rt_ prefix to avoid colliding with PD objects
$(BUILD_DIR)/rt_%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_ROOT_TASK) $< -o $@

# Assemble AArch64 startup stub
$(BUILD_DIR)/rt_start_aarch64.o: src/start_aarch64.S
	@mkdir -p $(BUILD_DIR)
	$(CC) -c -target aarch64-unknown-elf $< -o $@

# ─── AArch64: root_task uses custom LD script + embedded PD bundle ───────────
# On AArch64 the loader (kernel/loader/) loads root_task.elf from DRAM at
# 0x41000000.  The custom root_task.ld places it there and provides the
# .pd_bundle section that holds embedded PD ELFs.
#
# Build sequence:
#   1. Build loader.elf (kernel/loader/)
#   2. Generate PD bundle (xtask gen-pd-bundle) — requires all PD ELFs
#   3. Convert bundle to a linkable .o (objcopy --input binary)
#   4. Link root_task.elf with root_task.ld + bundle .o
#
# Non-AArch64 builds keep the original flat agentos.ld link.
ifeq ($(ARCH),aarch64)

PD_BUNDLE       := $(BUILD_DIR)/agentos_pd_bundle.bin
PD_BUNDLE_OBJ   := $(BUILD_DIR)/agentos_pd_bundle.o
# Deferred reference: SYSTEM_TOML is set later in this Makefile
SYSTEM_TOML_FOR_BUNDLE = $(SYSTEM_TOML)

# Step 1: Build the loader
$(BUILD_DIR)/loader.elf: FORCE
	$(MAKE) -C $(LOADER_DIR) \
		BUILD_DIR=$(BUILD_DIR) \
		AGENTOS_ARCH=$(AGENTOS_ARCH)

# Step 2: Generate the PD bundle (after all IMAGES are built)
$(PD_BUNDLE): $(addprefix $(BUILD_DIR)/, $(IMAGES)) $(SYSTEM_TOML_FOR_BUNDLE)
	cargo run -p xtask -- gen-pd-bundle \
		--system $(SYSTEM_TOML_FOR_BUNDLE) \
		--pd-dir $(BUILD_DIR) \
		--out $@

# Step 3: Turn the binary bundle into a relocatable ELF64 object.
#
# objcopy --input binary derives symbol names from the input file path,
# mangling every non-alphanumeric character to '_'.  To get predictable
# symbol names we invoke objcopy from BUILD_DIR with just the basename, so
# the mangled name is always:
#   _binary_agentos_pd_bundle_bin_{start,end,size}
$(PD_BUNDLE_OBJ): $(PD_BUNDLE)
	cd $(BUILD_DIR) && $(OBJCOPY) \
		--input-target=binary \
		--output-target=elf64-littleaarch64 \
		--binary-architecture=aarch64 \
		--rename-section .data=.pd_bundle,alloc,load,data,contents \
		--redefine-sym _binary_agentos_pd_bundle_bin_start=__pd_bundle_start \
		--redefine-sym _binary_agentos_pd_bundle_bin_end=__pd_bundle_end \
		--redefine-sym _binary_agentos_pd_bundle_bin_size=__pd_bundle_size \
		agentos_pd_bundle.bin agentos_pd_bundle.o

# Step 4: Link root_task.elf with the custom AArch64 LD script + bundle object
$(BUILD_DIR)/root_task.elf: $(ROOT_TASK_ASM_OBJS) $(ROOT_TASK_OBJS) $(PD_BUNDLE_OBJ) $(ROOT_TASK_LD)
	$(LD) $(LDFLAGS) -T$(ROOT_TASK_LD) $(filter-out $(ROOT_TASK_LD),$^) -o $@

else

# Non-AArch64: simple link with the standard PD linker script
$(BUILD_DIR)/root_task.elf: $(ROOT_TASK_OBJS)
	$(LD) $(LDFLAGS) $^ -o $@

endif

# ─── Board identification (passed from top-level Makefile) ───────────────────
BOARD_NAME      ?=
BOARD_NATIVE    ?= 0
BOARD_UART_PHYS ?=
BOARD_UART_SIZE ?=
BOARD_UART_TYPE ?=
BOARD_UART_IRQ  ?=

# Protection Domain ELFs
# monitor/controller: includes vibe_swap for the hot-swap controller-side logic
# m3_bare_metal.c provides memset/memcpy/memmove needed by compiler-generated code
PD_MONITOR_SRCS   := src/monitor.c src/cap_broker.c src/agent_pool.c src/vibe_swap.c src/m3_bare_metal.c src/log.c src/boot_integrity.c src/ed25519_verify.c src/monocypher.c src/cap_policy.c src/verify.c src/pd_tcb.c src/ut_alloc.c
PD_EVENTBUS_SRCS  := src/m3_bare_metal.c src/log.c   # event_bus.c → services/event-bus/ (E9-S1)
PD_INITAGENT_SRCS := src/init_agent.c src/m3_bare_metal.c src/log.c
PD_WORKER_SRCS    := src/worker.c src/m3_bare_metal.c src/log.c
# agentfs needs m3_bare_metal for memset/sqrtf (no wasm3 headers needed)
PD_AGENTFS_SRCS      := src/m3_bare_metal.c src/log.c   # agentfs.c → services/agentfs/ (E9-S1)
PD_VIBEENGINE_SRCS   := src/cap_policy.c src/m3_bare_metal.c src/log.c   # vibe_engine.c → services/vibe-engine/ (E9-S1)
PD_SWAPSLOT_SRCS     := src/swap_slot.c src/wasm3_host.c src/m3_bare_metal.c src/log.c
PD_LOG_DRAIN_SRCS    := src/log_drain.c src/m3_bare_metal.c src/log.c
PD_SERIAL_PD_SRCS    := src/m3_bare_metal.c src/log.c   # serial_pd.c → services/serial-mux/ (E9-S3)
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
PD_NET_SERVER_SRCS       := src/lwip_sys.c src/lwip_shim.c src/m3_bare_metal.c src/log.c   # net_server.c → services/net-server/ (E9-S3)
PD_NET_PD_SRCS           := src/lwip_sys.c src/lwip_shim.c src/m3_bare_metal.c src/log.c   # net_pd.c → services/net-service/ (E9-S3)
PD_WG_NET_SRCS           := src/wg_net.c src/monocypher.c src/m3_bare_metal.c src/log.c
PD_APP_MANAGER_SRCS      := src/app_manager.c src/m3_bare_metal.c src/log.c
PD_HTTP_SVC_SRCS         := src/http_svc.c src/m3_bare_metal.c src/log.c
PD_VM_MANAGER_SRCS       := src/vm_manager.c src/vmm_mux_stub.c src/m3_bare_metal.c src/log.c
PD_AUTH_SERVER_SRCS      := src/auth_server.c src/m3_bare_metal.c src/log.c
PD_EXT2FS_SRCS           := src/ext2fs.c src/m3_bare_metal.c src/log.c
PD_PROC_SERVER_SRCS      := src/proc_server.c src/m3_bare_metal.c src/log.c
PD_VM_SNAPSHOT_SRCS      := src/vm_snapshot.c src/m3_bare_metal.c src/log.c
PD_PFLOCAL_SERVER_SRCS   := src/pflocal_server.c src/m3_bare_metal.c src/log.c
PD_PFLOCAL_SRCS          := src/pflocal_server.c src/m3_bare_metal.c src/log.c
PD_EXEC_SERVER_SRCS      := src/exec_server.c src/m3_bare_metal.c src/log.c
PD_TERM_SERVER_SRCS      := src/term_server.c src/m3_bare_metal.c src/log.c
PD_BLOCK_PD_SRCS         := src/block_pd.c src/m3_bare_metal.c src/log.c
PD_FB_PD_SRCS            := src/framebuffer_pd.c src/m3_bare_metal.c src/log.c
PD_CC_PD_SRCS            := src/cc_pd.c src/m3_bare_metal.c src/log.c
PD_GPU_SCHED_SRCS        := src/gpu_sched.c src/gpu_shmem.c src/m3_bare_metal.c src/log.c
PD_IRQ_PD_SRCS           := src/irq_pd.c src/m3_bare_metal.c src/log.c
PD_TIMER_PD_SRCS         := src/timer_pd.c src/m3_bare_metal.c src/log.c

# ─── Inject seL4 CRT into every service PD ────────────────────────────────────
# sel4_crt.c provides __sel4_ipc_buffer (libmicrokit replacement) needed by
# any PD that includes agentos.h → log_drain_write → seL4_SetMR.
_PD_SRC_VARS := \
    PD_MONITOR_SRCS PD_EVENTBUS_SRCS PD_INITAGENT_SRCS PD_WORKER_SRCS \
    PD_AGENTFS_SRCS PD_VIBEENGINE_SRCS PD_SWAPSLOT_SRCS PD_LOG_DRAIN_SRCS \
    PD_SERIAL_PD_SRCS PD_MEMPROFILER_SRCS PD_NETISOLATOR_SRCS PD_SNAPSHOTSCHED_SRCS \
    PD_PERFCOUNTERS_SRCS PD_TIMEPARTITION_SRCS PD_TRACERECORDER_SRCS PD_NAMESERVER_SRCS \
    PD_VIRTIO_BLK_SRCS PD_VFS_SERVER_SRCS PD_SPAWN_SERVER_SRCS PD_APP_SLOT_SRCS \
    PD_NET_SERVER_SRCS PD_NET_PD_SRCS PD_WG_NET_SRCS PD_APP_MANAGER_SRCS \
    PD_HTTP_SVC_SRCS PD_VM_MANAGER_SRCS PD_AUTH_SERVER_SRCS PD_EXT2FS_SRCS \
    PD_PROC_SERVER_SRCS PD_VM_SNAPSHOT_SRCS PD_PFLOCAL_SERVER_SRCS PD_PFLOCAL_SRCS \
    PD_EXEC_SERVER_SRCS PD_TERM_SERVER_SRCS PD_BLOCK_PD_SRCS PD_FB_PD_SRCS \
    PD_CC_PD_SRCS PD_GPU_SCHED_SRCS PD_IRQ_PD_SRCS PD_TIMER_PD_SRCS
$(foreach v,$(_PD_SRC_VARS),$(eval $(v) += src/sel4_crt.c))
# pd_entry.c provides _start (ELF entry) + IPC buffer fix for all service PDs.
# It is NOT added to ROOT_TASK_SRCS — the root task has its own _start in
# start_aarch64.S.
$(foreach v,$(_PD_SRC_VARS),$(eval $(v) += src/pd_entry.c))

# wasm3 vendored sources (core interpreter only)
WASM3_SRCS := wasm3/m3_core.c wasm3/m3_env.c wasm3/m3_code.c wasm3/m3_compile.c \
              wasm3/m3_exec.c wasm3/m3_bind.c wasm3/m3_parse.c wasm3/m3_module.c \
              wasm3/m3_function.c wasm3/m3_info.c

CONTROLLER_OBJS   := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_MONITOR_SRCS))
EVENTBUS_OBJS     := $(BUILD_DIR)/event_bus.o \
                    $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EVENTBUS_SRCS))
INITAGENT_OBJS    := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_INITAGENT_SRCS))
WORKER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_WORKER_SRCS))
AGENTFS_OBJS      := $(BUILD_DIR)/agentfs.o \
                    $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_AGENTFS_SRCS))
VIBEENGINE_OBJS   := $(BUILD_DIR)/vibe_engine.o \
                    $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VIBEENGINE_SRCS))
LOG_DRAIN_OBJS     := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_LOG_DRAIN_SRCS))
SERIAL_PD_OBJS     := $(BUILD_DIR)/serial_pd.o \
                    $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_SERIAL_PD_SRCS))
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
NET_SERVER_OBJS       := $(BUILD_DIR)/net_server.o \
                    $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_NET_SERVER_SRCS))
NET_PD_OBJS           := $(BUILD_DIR)/net_pd.o \
                    $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_NET_PD_SRCS))
WG_NET_OBJS           := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_WG_NET_SRCS))
APP_MANAGER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_APP_MANAGER_SRCS))
HTTP_SVC_OBJS         := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_HTTP_SVC_SRCS))
VM_MANAGER_OBJS       := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VM_MANAGER_SRCS))
AUTH_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_AUTH_SERVER_SRCS))
EXT2FS_OBJS           := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EXT2FS_SRCS))
PROC_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_PROC_SERVER_SRCS))
VM_SNAPSHOT_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_VM_SNAPSHOT_SRCS))
PFLOCAL_SERVER_OBJS   := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_PFLOCAL_SERVER_SRCS))
PFLOCAL_OBJS          := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_PFLOCAL_SRCS))
EXEC_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EXEC_SERVER_SRCS))
TERM_SERVER_OBJS      := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_TERM_SERVER_SRCS))
WASM3_OBJS            := $(patsubst wasm3/%.c, $(BUILD_DIR)/wasm3_%.o, $(WASM3_SRCS))
BLOCK_PD_OBJS         := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_BLOCK_PD_SRCS))
FB_PD_OBJS            := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_FB_PD_SRCS))
CC_PD_OBJS            := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_CC_PD_SRCS))
GPU_SCHED_OBJS        := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_GPU_SCHED_SRCS))
IRQ_PD_OBJS           := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_IRQ_PD_SRCS))
TIMER_PD_OBJS         := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_TIMER_PD_SRCS))

IMAGES := controller.elf event_bus.elf init_agent.elf worker.elf agentfs.elf vibe_engine.elf vibeos.elf log_drain.elf serial_pd.elf swap_slot.elf mem_profiler.elf net_isolator.elf perf_counters.elf time_partition.elf trace_recorder.elf nameserver.elf virtio_blk.elf vfs_server.elf spawn_server.elf app_slot.elf net_server.elf net_pd.elf wg_net.elf app_manager.elf http_svc.elf vm_manager.elf auth_server.elf ext2fs.elf proc_server.elf vm_snapshot.elf pflocal_server.elf exec_server.elf term_server.elf block_pd.elf framebuffer_pd.elf cc_pd.elf gpu_sched.elf timer_svc.elf entropy_svc.elf block_svc.elf console_mux.elf usb_svc.elf model_svc.elf tool_svc.elf irq_pd.elf timer_pd.elf

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
    IMAGES += freebsd_vmm.elf
  else
    # Default aarch64: include Linux VMM (stub or full)
    IMAGES += linux_vmm.elf
  endif
else ifeq ($(ARCH),x86_64)
  # Include linux_vmm stub so the image can reference it
  IMAGES += linux_vmm.elf
else ifeq ($(ARCH),riscv64)
  # RISC-V 64: process-in-PD VMMs (no hypervisor extension)
  IMAGES += linux_vmm.elf freebsd_vmm.elf
endif

IMAGE_FILE  := $(BUILD_DIR)/agentos.img

build: $(IMAGE_FILE) $(BUILD_DIR)/root_task.elf

# ─── Extracted service PDs (E9: sources live in services/, not src/) ─────────
# CFLAGS already includes -Iinclude (kernel/agentos-root-task/include) so all
# seL4 and agentos headers resolve identically to when the files lived in src/.
# CFLAGS_SERVICES adds -I$(REPO_ROOT) for files that include "contracts/…/…".
$(BUILD_DIR)/agentfs.o: $(SERVICES_DIR)/agentfs/agentfs.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/event_bus.o: $(SERVICES_DIR)/event-bus/event_bus.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/vibe_engine.o: $(SERVICES_DIR)/vibe-engine/vibe_engine.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/serial_pd.o: $(SERVICES_DIR)/serial-mux/serial_pd.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/net_server.o: $(SERVICES_DIR)/net-server/net_server.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/net_pd.o: $(SERVICES_DIR)/net-service/net_pd.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# ─── Stub PDs (E9-S4): placeholders for contracts with no implementation yet ──
$(BUILD_DIR)/timer_svc.o: $(SERVICES_DIR)/timer-service/timer_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/entropy_svc.o: $(SERVICES_DIR)/entropy-service/entropy_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/block_svc.o: $(SERVICES_DIR)/block-service/block_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/console_mux.o: $(SERVICES_DIR)/console-mux/console_mux.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/usb_svc.o: $(SERVICES_DIR)/usb-service/usb_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/model_svc.o: $(SERVICES_DIR)/modelsvc/model_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/tool_svc.o: $(SERVICES_DIR)/toolsvc/tool_svc.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# vibeos — VOS lifecycle PD entry point (IPC dispatch loop — future work)
# vos_create/destroy/snapshot/restore are compiled for the host test suite;
# the bare-metal PD dispatch loop will link them in a future sprint.
$(BUILD_DIR)/vibeos_main.o: $(SERVICES_DIR)/vibeos/vibeos_main.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

# vos_*.o kept for reference: compiled by xtask test (AGENTOS_TEST_HOST)
$(BUILD_DIR)/vos_create.o: $(SERVICES_DIR)/vibeos/vos_create.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/vos_destroy.o: $(SERVICES_DIR)/vibeos/vos_destroy.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/vos_snapshot.o: $(SERVICES_DIR)/vibeos/vos_snapshot.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SERVICES) $< -o $@

$(BUILD_DIR)/vos_restore.o: $(SERVICES_DIR)/vibeos/vos_restore.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS_SERVICES) $< -o $@

VIBEOS_OBJS := $(BUILD_DIR)/vibeos_main.o

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

$(BUILD_DIR)/vibeos.elf: $(VIBEOS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# Stub ELFs — each links a single minimal stub object
$(BUILD_DIR)/timer_svc.elf: $(BUILD_DIR)/timer_svc.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/entropy_svc.elf: $(BUILD_DIR)/entropy_svc.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/block_svc.elf: $(BUILD_DIR)/block_svc.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/console_mux.elf: $(BUILD_DIR)/console_mux.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/usb_svc.elf: $(BUILD_DIR)/usb_svc.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/model_svc.elf: $(BUILD_DIR)/model_svc.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/tool_svc.elf: $(BUILD_DIR)/tool_svc.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/log_drain.elf: $(LOG_DRAIN_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/serial_pd.elf: $(SERIAL_PD_OBJS)
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


$(BUILD_DIR)/block_pd.elf: $(BLOCK_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/framebuffer_pd.elf: $(FB_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/cc_pd.elf: $(CC_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/gpu_sched.elf: $(GPU_SCHED_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/irq_pd.elf: $(IRQ_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/timer_pd.elf: $(TIMER_PD_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

# ─── RISC-V 64 VMMs (process-in-PD, no hypervisor extension) ─────────────
ifeq ($(ARCH),riscv64)
$(BUILD_DIR)/fdt_builder.o: src/fdt_builder.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/linux_vmm.o: src/linux_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/linux_vmm.elf: $(BUILD_DIR)/linux_vmm.o $(BUILD_DIR)/fdt_builder.o \
                             $(BUILD_DIR)/sel4_crt.o $(BUILD_DIR)/m3_bare_metal.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/freebsd_vmm.o: src/freebsd_vmm.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/freebsd_vmm.elf: $(BUILD_DIR)/freebsd_vmm.o $(BUILD_DIR)/fdt_builder.o \
                               $(BUILD_DIR)/sel4_crt.o \
                               $(BUILD_DIR)/m3_bare_metal.o
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@
endif

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
		AGENTOS_ARCH=$(AGENTOS_ARCH) \
		AGENTOS_BOARD=$(AGENTOS_BOARD) \
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
		AGENTOS_ARCH=$(AGENTOS_ARCH) \
		BOARD=$(AGENTOS_BOARD) \
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

# SYSTEM_TOML: per-arch/board TOML system descriptor consumed by xtask gen-image.
# The TOML mirrors the PD list from the corresponding .system XML but omits
# memory-region / channel details (handled by the root task at runtime).
# A minimal fallback TOML is provided; board-specific overrides live in boards/.
SYSTEM_TOML_DEFAULT := $(abspath $(CURDIR)/agentos.toml)
_BOARD_TOML := ../../boards/$(BOARD_NAME)/agentos.toml
SYSTEM_TOML := $(if $(wildcard $(_BOARD_TOML)),$(_BOARD_TOML),$(SYSTEM_TOML_DEFAULT))

# Kernel ELF: pulled directly from the Microkit SDK release package
KERNEL_ELF  := $(SEL4_SDK)/board/$(AGENTOS_BOARD)/release/elf/sel4.elf

# On AArch64, loader.elf is also a build artifact (needed at QEMU launch time).
# We add it as a dependency so it is rebuilt whenever the loader sources change.
ifeq ($(ARCH),aarch64)
_LOADER_DEP := $(BUILD_DIR)/loader.elf
else
_LOADER_DEP :=
endif

$(IMAGE_FILE): $(addprefix $(BUILD_DIR)/, $(IMAGES)) $(BUILD_DIR)/root_task.elf $(SYSTEM_TOML) $(KERNEL_ELF) $(_LOADER_DEP)
	cargo run -p xtask -- gen-image \
		--arch $(ARCH) \
		--system $(SYSTEM_TOML) \
		--kernel $(KERNEL_ELF) \
		--root-task $(BUILD_DIR)/root_task.elf \
		--pd-dir $(BUILD_DIR) \
		--out $(IMAGE_FILE)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: build clean FORCE

# ─── Auto-generated header dependency files ───────────────────────────────────
# -MMD -MP in CFLAGS causes clang to emit a .d file alongside each .o.
# Including them here makes 'make' recompile any .o whose headers changed.
-include $(wildcard $(BUILD_DIR)/*.d)
