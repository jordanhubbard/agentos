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
LDFLAGS := -L$(BOARD_DIR)/lib
LIBS    := -lmicrokit -Tmicrokit.ld

# Protection Domain ELFs
PD_MONITOR_SRCS  := src/monitor.c src/cap_broker.c src/log.c
PD_EVENTBUS_SRCS := src/event_bus.c src/log.c
PD_INITAGENT_SRCS := src/init_agent.c src/log.c

CONTROLLER_OBJS := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_MONITOR_SRCS))
EVENTBUS_OBJS  := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_EVENTBUS_SRCS))
INITAGENT_OBJS := $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(PD_INITAGENT_SRCS))

IMAGES := controller.elf event_bus.elf init_agent.elf
IMAGE_FILE  := $(BUILD_DIR)/agentos.img
REPORT_FILE := $(BUILD_DIR)/agentos-report.txt

all: $(IMAGE_FILE)

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/controller.elf: $(CONTROLLER_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/event_bus.elf: $(EVENTBUS_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD_DIR)/init_agent.elf: $(INITAGENT_OBJS)
	$(LD) $(LDFLAGS) $^ $(LIBS) -o $@

$(IMAGE_FILE) $(REPORT_FILE): $(addprefix $(BUILD_DIR)/, $(IMAGES)) agentos.system
	$(MICROKIT_TOOL) agentos.system \
		--search-path $(BUILD_DIR) \
		--board $(MICROKIT_BOARD) \
		--config $(MICROKIT_CONFIG) \
		-o $(IMAGE_FILE) \
		-r $(REPORT_FILE)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all clean
