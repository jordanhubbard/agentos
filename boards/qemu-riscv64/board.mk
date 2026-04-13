# ── agentOS board: QEMU RISC-V 64 ─────────────────────────────────────────────
# RISC-V development / reference target.
BOARD_NAME     := qemu-riscv64
MICROKIT_BOARD := qemu_virt_riscv64
BOARD_ARCH     := riscv64
BOARD_NATIVE   := 0

# Console UART: NS16550A at 0x10000000 on QEMU virt RISC-V.
BOARD_UART_PHYS  := 0x10000000
BOARD_UART_SIZE  := 0x100
BOARD_UART_TYPE  := ns16550
BOARD_UART_IRQ   :=

# QEMU launch configuration
QEMU_BIN     := qemu-system-riscv64
QEMU_MACHINE := -machine virt
QEMU_CPU_TCG := rv64
QEMU_MEM     := -m 2G
QEMU_DISPLAY := -display none -monitor none

QEMU_SERIAL_FLAGS := -serial unix:/tmp/agentos-serial.sock,server=on,wait=off

# BIOS: OpenSBI (required for RISC-V boot)
QEMU_BIOS_FLAGS = $(if $(BIOS),-bios $(BIOS),)

QEMU_NET_FLAGS :=

QEMU_BOOT_FLAGS = $(QEMU_BIOS_FLAGS) -kernel $(IMAGE)

DEPLOY_SCRIPT :=
