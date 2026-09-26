# ── agentOS board: QEMU x86_64 VMX/EPT qualification ───────────────────────
#
# A KVM-only board used by make gate-x86_64-vtx.  It boots one VMM PD that
# enters VMX non-root mode into a single EPT-mapped HLT instruction, then
# asserts the resulting VM exit.  It is not an x86 guest OS target.
BOARD_NAME     := qemu-x86_64-vtx
MICROKIT_BOARD := x86_64_generic_vtx
BOARD_ARCH     := x86_64
BOARD_NATIVE   := 0

BOARD_UART_PHYS  := 0x3F8
BOARD_UART_SIZE  := 0x8
BOARD_UART_TYPE  := ns16550
BOARD_UART_IRQ   :=

QEMU_BIN     := qemu-system-x86_64
QEMU_MACHINE := -machine q35
QEMU_MEM     := -m 2G
QEMU_DISPLAY := -display none -monitor none
QEMU_SERIAL_FLAGS := -serial stdio
QEMU_BOOT_FLAGS = \
  -kernel $(SEL4_SDK)/board/x86_64_generic_vtx/release/elf/sel4_32.elf \
  -initrd _build/x86_64_generic_vtx/root_task.elf

DEPLOY_SCRIPT :=
