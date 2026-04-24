# tests/ipc_bench/Makefile — agentOS IPC benchmark (host build)
#
# Targets:
#   ipc_bench_host   build the POSIX test-host binary (default)
#   test             build and run; exit non-zero on TAP failure
#   clean            remove build artefacts
#
# The host build defines AGENTOS_TEST_HOST so seL4 headers are not required.
# The bare-metal AArch64 build is driven by the top-level CMake/seL4 build
# system — see the CMakeLists.txt in the repo root.

CC      ?= cc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic \
          -Wno-unused-parameter \
          -DAGENTOS_TEST_HOST
LDFLAGS =

# All source is in one translation unit; no need for -lm (no floating point).
ipc_bench_host: ipc_bench.c ipc_bench.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ipc_bench.c

test: ipc_bench_host
	./ipc_bench_host

clean:
	rm -f ipc_bench_host

.PHONY: test clean
