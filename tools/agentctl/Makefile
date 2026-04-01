# agentctl — agentOS Interactive Launcher
#
# Builds agentctl using the system ncurses library.
# Requires: pkg-config, ncurses (libncurses-dev on Debian/Ubuntu,
#           ncurses via brew on macOS)
#
# Usage:
#   make            — build agentctl
#   make clean      — remove agentctl binary
#   make install    — install to PREFIX/bin (default: /usr/local/bin)

PREFIX ?= /usr/local

# Detect host OS for ncurses pkg-config name
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
  # Homebrew ncurses (needed for ncurses 6 on macOS)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null || echo /usr/local)
  BREW_NCURSES := $(BREW_PREFIX)/opt/ncurses
  ifeq ($(wildcard $(BREW_NCURSES)/lib/libncurses.*),)
    # Fall back to system ncurses on macOS (Xcode ships it)
    NCURSES_CFLAGS :=
    NCURSES_LIBS   := -lncurses
  else
    NCURSES_CFLAGS := -I$(BREW_NCURSES)/include
    NCURSES_LIBS   := -L$(BREW_NCURSES)/lib -lncurses
  endif
else
  # Linux: use pkg-config
  NCURSES_CFLAGS := $(shell pkg-config --cflags ncurses 2>/dev/null)
  NCURSES_LIBS   := $(shell pkg-config --libs   ncurses 2>/dev/null || echo -lncurses)
endif

CC      ?= cc
CFLAGS  := -O2 -Wall -Wextra -std=c11 $(NCURSES_CFLAGS)
LDFLAGS := $(NCURSES_LIBS)

TARGET  := agentctl
SRC     := agentctl.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "Built: $(TARGET)"

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	@echo "Installed: $(DESTDIR)$(PREFIX)/bin/$(TARGET)"
