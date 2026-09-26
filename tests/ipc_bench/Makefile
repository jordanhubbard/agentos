# tests/ipc_bench/Makefile — agentOS IPC benchmark (host build)
#
# Targets:
#   ipc_bench_host   build the POSIX test-host binary (default)
#   test             build and run; exit non-zero on TAP failure
#   clean            remove build artefacts
#
# The host build defines AGENTOS_TEST_HOST so seL4 headers are not required.
# The bare-metal AArch64 build is driven by kernel/agentos-root-task/Makefile.

CC      ?= cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
          -Wno-unused-parameter \
          -DAGENTOS_TEST_HOST
LDFLAGS =
BUILD_DIR := $(abspath ../../_build/tests/ipc_bench)
TARGET := $(BUILD_DIR)/ipc_bench_host

# All source is in one translation unit; no need for -lm (no floating point).
ipc_bench_host: $(TARGET)

$(TARGET): ipc_bench.c ipc_bench.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ipc_bench.c

test: ipc_bench_host
	$(TARGET)

clean:
	rm -rf $(BUILD_DIR)

.PHONY: ipc_bench_host test clean
