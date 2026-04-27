# ── agentOS board: QEMU AArch64 ───────────────────────────────────────────────
# Primary development platform.
# Uses QEMU's virt machine with software emulation (TCG) on Apple Silicon
# where HVF has seL4-incompatible assertion failures, and KVM on Linux.
BOARD_NAME     := qemu-aarch64
MICROKIT_BOARD := qemu_virt_aarch64
BOARD_ARCH     := aarch64
BOARD_NATIVE   := 0

# Console UART: on QEMU the PL011 at 0x9000000 (IRQ 33) is owned by
# linux_vmm for guest passthrough — console_shell must not map it.
# console_shell output goes through microkit_dbg_puts() (seL4 debug serial)
# and the ring buffer; UART MMIO code is excluded by leaving these unset.
BOARD_UART_PHYS  :=
BOARD_UART_SIZE  :=
BOARD_UART_TYPE  :=
BOARD_UART_IRQ   :=

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
