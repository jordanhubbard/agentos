# ── agentOS board: QEMU x86_64 ────────────────────────────────────────────────
# x86_64 development / CI target.  Uses the QEMU q35 machine model.
# UEFI capable; legacy BIOS boot NOT supported.
BOARD_NAME     := qemu-x86_64
MICROKIT_BOARD := x86_64_generic
BOARD_ARCH     := x86_64
BOARD_NATIVE   := 0

# Console UART: NS16550 COM1 at I/O port 0x3F8.
# Note: x86 I/O port access from a seL4 PD requires explicit port cap
# delegation; on QEMU x86, UART output goes via microkit_dbg_putc() which
# handles the IN/OUT instructions internally.  console_shell uses ring
# buffer + bridge.rs for interactive input on this platform.
BOARD_UART_PHYS  := 0x3F8   # I/O port (not MMIO)
BOARD_UART_SIZE  := 0x8
BOARD_UART_TYPE  := ns16550
BOARD_UART_IRQ   :=  # not assigned; console_shell uses ring on x86 QEMU

# QEMU launch configuration
QEMU_BIN     := qemu-system-x86_64
QEMU_MACHINE := -machine q35
QEMU_CPU_TCG := qemu64
QEMU_MEM     := -m 2G
QEMU_DISPLAY := -display none -monitor none

QEMU_SERIAL_FLAGS := -serial unix:/tmp/agentos-serial.sock,server=on,wait=off

QEMU_NET_FLAGS := \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
  -device e1000,netdev=net0

QEMU_BOOT_FLAGS = -kernel $(IMAGE)

DEPLOY_SCRIPT :=
