# ── agentOS board: QEMU AArch64 ───────────────────────────────────────────────
# Primary development platform.
# Uses QEMU's virt machine with software emulation (TCG) on Apple Silicon
# where HVF has seL4-incompatible assertion failures, and KVM on Linux.
BOARD_NAME     := qemu-aarch64
MICROKIT_BOARD := qemu_virt_aarch64
BOARD_ARCH     := aarch64
BOARD_NATIVE   := 0

# Console UART: PL011 primary at 0x9000000 on QEMU virt AArch64.
# The console_shell PD maps this region for UART output.
# On QEMU, UART RX is owned by linux_vmm (IRQ 33); console_shell uses
# the ring buffer + bridge.rs API for interactive input instead.
BOARD_UART_PHYS  := 0x9000000
BOARD_UART_SIZE  := 0x1000
BOARD_UART_TYPE  := pl011
BOARD_UART_IRQ   :=  # IRQ 33 owned by linux_vmm on QEMU; console_shell uses ring

# QEMU launch configuration
QEMU_BIN     := qemu-system-aarch64
QEMU_MACHINE := -machine virt,virtualization=on,highmem=off,secure=off
QEMU_CPU_TCG := cortex-a53
QEMU_MEM     := -m 2G
QEMU_DISPLAY := -display none -monitor none

QEMU_SERIAL_FLAGS := \
  -chardev socket,id=char0,path=/tmp/agentos-serial.sock,server=on,wait=off \
  -serial chardev:char0

QEMU_NET_FLAGS := \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
  -device virtio-net-device,netdev=net0

QEMU_BOOT_FLAGS = \
  -device loader,file=$(IMAGE),addr=0x70000000,cpu-num=0

# How to build a bootable image for this board (only for native boards)
DEPLOY_SCRIPT :=
