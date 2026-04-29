# agentctl — agentOS C&C Reference Consumer
#
# External consumer of cc_contract.h — zero kernel/ or services/ dependencies.
#
# Usage:
#   make              Build agentctl
#   make test         Run unit tests (no QEMU required)
#   make install      Install to PREFIX/bin (default: /usr/local/bin)
#   make clean        Remove build artifacts

PREFIX ?= /usr/local

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra -std=c11 -DAGENTOS_TEST_HOST \
           -I../../kernel/agentos-root-task/include
LDFLAGS :=

TARGET  := agentctl
SRC     := agentctl.c

.PHONY: all test install clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $(TARGET)"

# Unit test: verify binary compiles and --help exits 0
test: $(TARGET)
	@echo "[test] --help exits 0"
	./$(TARGET) --help >/dev/null 2>&1; \
	  if [ $$? -eq 0 ]; then echo "[PASS] --help"; else echo "[FAIL] --help"; exit 1; fi
	@echo "[test] unknown flag exits non-zero"
	./$(TARGET) --not-a-flag >/dev/null 2>&1; \
	  if [ $$? -ne 0 ]; then echo "[PASS] unknown flag"; else echo "[FAIL] unknown flag"; exit 1; fi
	@echo "[test] --batch requires socket (exits non-zero without agentOS)"
	CC_PD_SOCK=/tmp/agentctl_no_such_socket ./$(TARGET) --batch list-guests >/dev/null 2>&1; \
	  if [ $$? -ne 0 ]; then echo "[PASS] no-socket exits 1"; else echo "[FAIL] no-socket should exit 1"; exit 1; fi
	@echo "[test] all passed"

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@echo "Installed: $(DESTDIR)$(PREFIX)/bin/$(TARGET)"

clean:
	rm -f $(TARGET)
