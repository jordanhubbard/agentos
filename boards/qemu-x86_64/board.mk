# ── agentOS board: QEMU x86_64 ────────────────────────────────────────────────
# x86_64 development / CI target.  Uses the QEMU q35 machine model.
# QEMU boots the seL4 kernel directly and passes root_task.elf as the initial
# module; build/x86_64_generic/agentos.img remains the agentOS flat container.
BOARD_NAME     := qemu-x86_64
MICROKIT_BOARD := x86_64_generic
BOARD_ARCH     := x86_64
BOARD_NATIVE   := 0

# Console UART: NS16550 COM1 at I/O port 0x3F8.
# Note: x86 I/O port access from a seL4 PD requires explicit port cap
# delegation.  The root task issues that cap during bootstrap and writes COM1
# directly for early diagnostics.
BOARD_UART_PHYS  := 0x3F8   # I/O port (not MMIO)
BOARD_UART_SIZE  := 0x8
BOARD_UART_TYPE  := ns16550
BOARD_UART_IRQ   :=  # not assigned on x86 QEMU

# QEMU launch configuration
QEMU_BIN     := qemu-system-x86_64
QEMU_MACHINE := -machine q35
QEMU_CPU_TCG := qemu64
QEMU_MEM     := -m 2G
QEMU_DISPLAY := -display none -monitor none

QEMU_SERIAL_FLAGS := -serial stdio

QEMU_NET_FLAGS := \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
  -device e1000,netdev=net0

QEMU_BOOT_FLAGS = \
  -kernel microkit-sdk-2.1.0/board/x86_64_generic/release/elf/sel4_32.elf \
  -initrd build/x86_64_generic/root_task.elf

DEPLOY_SCRIPT :=
