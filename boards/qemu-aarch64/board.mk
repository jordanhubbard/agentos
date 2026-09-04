# ── agentOS board: QEMU AArch64 ───────────────────────────────────────────────
# Primary development platform.
# Uses QEMU's virt machine with software emulation (TCG) on Apple Silicon
# where HVF has seL4-incompatible assertion failures, and KVM on Linux.
BOARD_NAME     := qemu-aarch64
MICROKIT_BOARD := qemu_virt_aarch64
BOARD_ARCH     := aarch64
BOARD_NATIVE   := 0

# Console UART: serial_pd exclusively owns QEMU virt PL011 after root-task
# bootstrap.  VMMs and other PDs receive only serial_pd endpoint capabilities.
BOARD_UART_PHYS  := 0x09000000
BOARD_UART_SIZE  := 0x1000
BOARD_UART_TYPE  := pl011
BOARD_UART_IRQ   := 33

# QEMU launch configuration
QEMU_BIN     := qemu-system-aarch64
QEMU_MACHINE := -machine virt,virtualization=on,highmem=off,secure=off
QEMU_CPU_TCG := cortex-a53
QEMU_MEM     := -m 2G
QEMU_DISPLAY := -display none -monitor none

QEMU_SERIAL_FLAGS := \
  -chardev socket,id=char0,path=/tmp/agentos-serial.sock,server=on,wait=off \
  -serial chardev:char0 \
  -chardev socket,id=cc_pd_char,path=build/cc_pd.sock,server=on,wait=off \
  -serial chardev:cc_pd_char

QEMU_NET_FLAGS := \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
  -device virtio-net-device,netdev=net0

QEMU_BOOT_FLAGS = \
  -device loader,file=$(IMAGE),addr=0x70000000,cpu-num=0

# How to build a bootable image for this board (only for native boards)
DEPLOY_SCRIPT :=
