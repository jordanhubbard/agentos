#!/usr/bin/env bash
# flash-usb.sh — Prepare a bootable UEFI USB drive for the Intel NUC board.
#
# Usage:
#   make BOARD_NAME=intel-nuc build
#   sudo ./boards/intel-nuc/flash-usb.sh /dev/sdX build/x86_64_generic/agentos.img
#
# Requirements:
#   - grub-efi-amd64-bin (apt) or grub (brew)
#   - mtools (for mformat/mcopy)
#
# The resulting USB drive:
#   - FAT32 ESP partition labelled "AGENTOS_EFI"
#   - /EFI/BOOT/BOOTX64.EFI  (GRUB EFI stub)
#   - /EFI/BOOT/grub.cfg      (chainloads seL4 image)
#   - /boot/agentos.img       (Microkit seL4 image, ELF multiboot2)

set -euo pipefail

DEVICE="${1:?Usage: $0 <device> <image>}"
IMAGE="${2:?Usage: $0 <device> <image>}"

if [[ ! -b "$DEVICE" ]]; then
    echo "ERROR: $DEVICE is not a block device." >&2
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "ERROR: image $IMAGE not found." >&2
    exit 1
fi

echo "==> Preparing UEFI USB boot drive on $DEVICE"
echo "    agentOS image: $IMAGE"
echo ""
echo "WARNING: ALL DATA ON $DEVICE WILL BE ERASED."
read -rp "Type 'yes' to continue: " CONFIRM
[[ "$CONFIRM" == "yes" ]] || { echo "Aborted."; exit 1; }

# ── Partition the drive ───────────────────────────────────────────────────────
echo "==> Partitioning $DEVICE ..."
sgdisk --zap-all "$DEVICE"
sgdisk \
  --new=1:0:+512M  --typecode=1:ef00 --change-name=1:"EFI System" \
  "$DEVICE"

PART="${DEVICE}1"
sleep 1   # give kernel time to re-read partition table

# ── Format as FAT32 ──────────────────────────────────────────────────────────
echo "==> Formatting ${PART} as FAT32 ..."
mkfs.vfat -F 32 -n "AGENTOS_EFI" "$PART"

# ── Mount and install files ──────────────────────────────────────────────────
MNT=$(mktemp -d)
mount "$PART" "$MNT"
trap "umount '$MNT'; rmdir '$MNT'" EXIT

mkdir -p "$MNT/EFI/BOOT" "$MNT/boot"

# Install GRUB EFI stub
if command -v grub-mkimage >/dev/null 2>&1; then
    grub-mkimage \
        -O x86_64-efi \
        -o "$MNT/EFI/BOOT/BOOTX64.EFI" \
        -p /EFI/BOOT \
        part_gpt fat normal search multiboot2 echo
else
    echo "ERROR: grub-mkimage not found. Install grub-efi-amd64-bin." >&2
    exit 1
fi

# GRUB config: load seL4 image via multiboot2
cat > "$MNT/EFI/BOOT/grub.cfg" <<'GRUBCFG'
set timeout=3
set default=0

menuentry "agentOS (seL4/Microkit)" {
    insmod part_gpt
    insmod fat
    insmod multiboot2
    multiboot2 /boot/agentos.img
    boot
}
GRUBCFG

# Copy agentOS image
cp "$IMAGE" "$MNT/boot/agentos.img"

echo ""
echo "==> Done! USB drive prepared at $DEVICE"
echo "    Boot the NUC from this USB drive."
echo "    Physical serial (COM1, 115200 8N1) provides the console_shell CLI."
echo "    Connect a browser to the NUC's IP address for the web dashboard."
