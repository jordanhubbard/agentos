#!/bin/bash
# fetch-ubuntu-guest.sh
# Downloads Ubuntu Server LTS AArch64 cloud image for use as an agentOS VM guest.
#
# Usage: ./scripts/fetch-ubuntu-guest.sh [--output-dir DIR]
#
# Produces in guest-images/:
#   ubuntu-<version>-aarch64.img  — raw disk image, bootable under libvmm
#
# Idempotent: exits 0 immediately if the image is already present.
# Run 'make clean-images' to force a fresh download.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="${1:-$REPO_ROOT/guest-images}"

# Ubuntu 24.04 LTS (Noble Numbat) — current LTS as of 2026
UBUNTU_VERSION="24.04"
UBUNTU_CODENAME="noble"
UBUNTU_ARCH="arm64"
UBUNTU_BASE_URL="https://cloud-images.ubuntu.com/${UBUNTU_CODENAME}/current"
UBUNTU_CLOUD_IMG="${UBUNTU_CODENAME}-server-cloudimg-${UBUNTU_ARCH}.img"
UBUNTU_RAW="ubuntu-${UBUNTU_VERSION}-aarch64.img"

info() { echo "[fetch-ubuntu-guest] $*"; }
die()  { echo "[fetch-ubuntu-guest] ERROR: $*" >&2; exit 1; }

command -v curl     >/dev/null 2>&1 || die "curl is required"
command -v qemu-img >/dev/null 2>&1 || die "qemu-img is required (brew install qemu / apt install qemu-utils)"

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

if [[ -f "$UBUNTU_RAW" ]]; then
    info "Ubuntu ${UBUNTU_VERSION} image already present: $OUTPUT_DIR/$UBUNTU_RAW"
    exit 0
fi

info "Downloading Ubuntu ${UBUNTU_VERSION} LTS AArch64 cloud image..."
curl -L --progress-bar -o "$UBUNTU_CLOUD_IMG" \
    "${UBUNTU_BASE_URL}/${UBUNTU_CLOUD_IMG}" \
    || die "Failed to download Ubuntu cloud image"

# Sanity-check: real image is hundreds of MB
local_size=$(wc -c < "$UBUNTU_CLOUD_IMG")
if [[ "$local_size" -lt 1048576 ]]; then
    rm -f "$UBUNTU_CLOUD_IMG"
    die "Download looks wrong (only ${local_size} bytes). Check the URL: ${UBUNTU_BASE_URL}/${UBUNTU_CLOUD_IMG}"
fi

info "Converting qcow2 → raw disk image..."
qemu-img convert -f qcow2 -O raw "$UBUNTU_CLOUD_IMG" "$UBUNTU_RAW"
rm -f "$UBUNTU_CLOUD_IMG"

info "Ubuntu raw image: $OUTPUT_DIR/$UBUNTU_RAW"
info ""
info "Now build the VMM:"
info "  make GUEST_OS=ubuntu"
info ""
info "The Ubuntu console will appear as the linux_vm slot in the agentOS console."
