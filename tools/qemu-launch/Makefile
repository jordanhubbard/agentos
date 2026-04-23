CC      ?= cc
CFLAGS  += -Wall -Wextra -O2 $(shell pkg-config --cflags ncurses)
LDFLAGS += $(shell pkg-config --libs ncurses)

all: qemu-launch

qemu-launch: qemu_launch.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f qemu-launch

.PHONY: all clean
