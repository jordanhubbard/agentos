#!/usr/bin/env bash
# flash-sd.sh — Prepare a bootable SD card for the Raspberry Pi 5.
#
# Usage:
#   make BOARD_NAME=rpi5 build
#   sudo ./boards/rpi5/flash-sd.sh /dev/sdX build/rpi4b_4gb/agentos.img
#
# NOTE: Until the Microkit SDK adds native RPi5 support, this image uses
# the rpi4b_4gb board definition and will not boot correctly on RPi5 hardware.
#
# The SD card layout:
#   Partition 1: FAT32 boot partition (firmware, config.txt, start4.elf, etc.)
#   Partition 2: (unused — seL4 runs entirely from the boot partition ELF)
#
# Requirements:
#   - Raspberry Pi firmware files (start4.elf, fixup4.dat, bootcode.bin)
#     Download from: https://github.com/raspberrypi/firmware/tree/master/boot

set -euo pipefail

DEVICE="${1:?Usage: $0 <device> <image>}"
IMAGE="${2:?Usage: $0 <device> <image>}"
FIRMWARE_DIR="${3:-/opt/rpi-firmware}"

if [[ ! -b "$DEVICE" ]]; then
    echo "ERROR: $DEVICE is not a block device." >&2
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "ERROR: image $IMAGE not found." >&2
    exit 1
fi

echo "==> Preparing RPi5 SD card on $DEVICE"
echo "    agentOS image:  $IMAGE"
echo "    RPi firmware:   $FIRMWARE_DIR"
echo ""
echo "WARNING: ALL DATA ON $DEVICE WILL BE ERASED."
read -rp "Type 'yes' to continue: " CONFIRM
[[ "$CONFIRM" == "yes" ]] || { echo "Aborted."; exit 1; }

# ── Partition ─────────────────────────────────────────────────────────────────
echo "==> Partitioning $DEVICE ..."
parted -s "$DEVICE" mklabel msdos
parted -s "$DEVICE" mkpart primary fat32 4MiB 256MiB
PART="${DEVICE}1"
sleep 1

# ── Format ───────────────────────────────────────────────────────────────────
echo "==> Formatting ${PART} as FAT32 ..."
mkfs.vfat -F 32 -n "AGENTOS_BOOT" "$PART"

# ── Mount and install ─────────────────────────────────────────────────────────
MNT=$(mktemp -d)
mount "$PART" "$MNT"
trap "umount '$MNT'; rmdir '$MNT'" EXIT

# RPi firmware (required to load the kernel ELF)
for f in start4.elf fixup4.dat bootcode.bin; do
    if [[ -f "$FIRMWARE_DIR/$f" ]]; then
        cp "$FIRMWARE_DIR/$f" "$MNT/"
    else
        echo "WARNING: missing $FIRMWARE_DIR/$f — firmware may be incomplete"
    fi
done

# config.txt: tell the RPi bootloader to load our ELF as the kernel
cat > "$MNT/config.txt" <<'CONFIG'
# agentOS seL4/Microkit config for Raspberry Pi 4/5
arm_64bit=1
kernel=agentos.img
enable_uart=1
# Disable Bluetooth to free the primary PL011 UART for agentOS console
dtoverlay=disable-bt
# CPU governor (optional — seL4 manages this)
arm_freq=1500
over_voltage=2
CONFIG

# Copy seL4 image
cp "$IMAGE" "$MNT/agentos.img"

echo ""
echo "==> Done! SD card prepared at $DEVICE"
echo "    Boot the Raspberry Pi with this SD card."
echo "    Physical UART (GPIO 14/15, 115200 8N1) provides the console_shell CLI."
echo "    Connect a browser to the Pi's IP address for the web dashboard."
echo ""
echo "NOTE: This build uses the rpi4b_4gb board definition (Microkit SDK 2.1.0"
echo "      does not yet include a dedicated rpi5 board). Update MICROKIT_BOARD"
echo "      in boards/rpi5/board.mk once upstream support is added."
