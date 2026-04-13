# ── agentOS board: Raspberry Pi 5 (AArch64 native hardware) ──────────────────
#
# Target: Raspberry Pi 5 (BCM2712, Cortex-A76, 4/8GB LPDDR5).
#
# SDK NOTE: Microkit SDK 2.1.0 does not yet include a dedicated RPi5 board
# definition.  This configuration uses rpi4b_4gb as a starting point (both
# are aarch64) but will NOT run correctly on RPi5 hardware due to different
# memory maps, UART addresses, and GIC topology.
#
# Track upstream Microkit RPi5 support:
#   https://github.com/seL4/microkit/issues
#
# Once the Microkit SDK ships an rpi5 board, change MICROKIT_BOARD below and
# update BOARD_UART_PHYS to the BCM2712's UART0 address (0x107D001000).
BOARD_NAME     := rpi5
# TEMPORARY: rpi4b_4gb used as placeholder until Microkit SDK adds rpi5 support
MICROKIT_BOARD := rpi4b_4gb
BOARD_ARCH     := aarch64
BOARD_NATIVE   := 1

# Console UART: PL011 primary.
# RPi4 (BCM2711): UART0 at 0xFE201000
# RPi5 (BCM2712): UART0 at 0x107D001000  ← UPDATE HERE when SDK is ready
BOARD_UART_PHYS  := 0xFE201000   # RPi4 address — RPi5 is 0x107D001000
BOARD_UART_SIZE  := 0x1000
BOARD_UART_TYPE  := pl011
BOARD_UART_IRQ   := 57           # UART0 GIC SPI 57 (RPi4/BCM2711)

# No QEMU — runs on real hardware
QEMU_BIN         :=
QEMU_MACHINE     :=
QEMU_CPU_TCG     :=
QEMU_MEM         :=
QEMU_DISPLAY     :=
QEMU_SERIAL_FLAGS :=
QEMU_NET_FLAGS   :=
QEMU_BOOT_FLAGS  :=

# Deployment: write a bootable SD card image
DEPLOY_SCRIPT := $(dir $(lastword $(MAKEFILE_LIST)))flash-sd.sh

# Network: RPi5 uses the BCM2712's built-in Ethernet (ARM MAC + PHY).
# SDDF driver for BCM2712 Ethernet required; see docs/native-networking.md
BOARD_NET_TYPE := bcm2712_eth
