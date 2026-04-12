# Makefile — ipc_bridge_daemon
#
# Build the Linux-side IPC bridge daemon for the agentOS guest.
# Requires a C99-capable gcc.  Cross-compile by setting CC, e.g.:
#   make CC=aarch64-linux-gnu-gcc

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c99 \
           -D_GNU_SOURCE \
           -Wno-unused-parameter

TARGET  := ipc_bridge_daemon
SRCS    := ipc_bridge_daemon.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET)
