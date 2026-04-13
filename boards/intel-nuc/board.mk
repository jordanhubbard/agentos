# ── agentOS board: Intel NUC (x86_64 UEFI native hardware) ───────────────────
#
# Supports any x86_64 machine with UEFI firmware and a COM1 serial port.
# Tested on Intel NUC 12/13 series.
#
# Boot path:
#   UEFI firmware → GRUB (EFI) → Microkit seL4 image (ELF multiboot2)
#   See boards/intel-nuc/flash-usb.sh to prepare a bootable USB drive.
#
# DOES NOT support legacy BIOS boot.
BOARD_NAME     := intel-nuc
MICROKIT_BOARD := x86_64_generic
BOARD_ARCH     := x86_64
BOARD_NATIVE   := 1

# Console UART: NS16550 COM1 MMIO (LPSS UART on modern Intel PCH).
# NUC12/13 expose COM1 at MMIO 0xFE034000 (LPSS UART2).
# If the board has a traditional 16550 at I/O 0x3F8, set TYPE := ns16550io.
BOARD_UART_PHYS  := 0xFE034000
BOARD_UART_SIZE  := 0x1000
BOARD_UART_TYPE  := ns16550
BOARD_UART_IRQ   := 4    # COM1 IRQ (standard PC)

# No QEMU — runs directly on the hardware
QEMU_BIN         :=
QEMU_MACHINE     :=
QEMU_CPU_TCG     :=
QEMU_MEM         :=
QEMU_DISPLAY     :=
QEMU_SERIAL_FLAGS :=
QEMU_NET_FLAGS   :=
QEMU_BOOT_FLAGS  :=

# Deployment: prepare a bootable UEFI USB drive
DEPLOY_SCRIPT := $(dir $(lastword $(MAKEFILE_LIST)))flash-usb.sh

# Network interface for native HTTP dashboard (Intel I225/I219 on NUC)
# The SDDF ethernet driver for I225 is required; see docs/native-networking.md
BOARD_NET_TYPE   := intel_i225
BOARD_NET_PCI    := 0000:00:1f.6   # default NUC PCI slot; override if needed
