#!/bin/bash
# fetch-freebsd-guest.sh
# Downloads FreeBSD 14 AArch64 VM images and EDK2 UEFI firmware
# for use as agentOS VM guest binaries.
#
# Usage: ./scripts/fetch-freebsd-guest.sh [--output-dir DIR]
#
# Produces in guest-images/:
#   freebsd-14-arm64.img  — raw disk image (UFS, bootable with EFI)
#   edk2-aarch64-code.fd  — EDK2 AArch64 UEFI firmware (read-only flash)
#   edk2-aarch64-vars.fd  — EDK2 UEFI variable store (read-write flash)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="${1:-$REPO_ROOT/guest-images}"

FREEBSD_VERSION="14.2"
FREEBSD_ARCH="arm64-aarch64"
FREEBSD_BASE_URL="https://download.freebsd.org/releases/VM-IMAGES/${FREEBSD_VERSION}-RELEASE/${FREEBSD_ARCH}/Latest"
FREEBSD_IMAGE="FreeBSD-${FREEBSD_VERSION}-RELEASE-${FREEBSD_ARCH}.qcow2.xz"

# EDK2 — use Ubuntu/Debian package (pre-built, well-tested)
EDK2_DEB_URL="http://archive.ubuntu.com/ubuntu/pool/main/e/edk2/ovmf_2024.02-2_all.deb"
EDK2_DEB_FILE="ovmf.deb"

info() { echo "[fetch-freebsd-guest] $*"; }
die()  { echo "[fetch-freebsd-guest] ERROR: $*" >&2; exit 1; }

command -v curl   >/dev/null 2>&1 || die "curl is required"
command -v qemu-img >/dev/null 2>&1 || die "qemu-img is required (apt install qemu-utils)"

mkdir -p "$OUTPUT_DIR"
cd "$OUTPUT_DIR"

# ─── FreeBSD disk image ───────────────────────────────────────────────────────
FREEBSD_RAW="freebsd-${FREEBSD_VERSION}-arm64.img"

if [[ -f "$FREEBSD_RAW" ]]; then
    info "FreeBSD image already present: $FREEBSD_RAW"
else
    info "Downloading FreeBSD ${FREEBSD_VERSION} AArch64..."
    curl -L --progress-bar -o "$FREEBSD_IMAGE" \
        "${FREEBSD_BASE_URL}/${FREEBSD_IMAGE}" \
        || die "Failed to download FreeBSD image"

    info "Decompressing FreeBSD image..."
    xz -d "$FREEBSD_IMAGE"
    FREEBSD_QCOW2="${FREEBSD_IMAGE%.xz}"

    info "Converting qcow2 → raw..."
    qemu-img convert -f qcow2 -O raw "$FREEBSD_QCOW2" "$FREEBSD_RAW"
    rm -f "$FREEBSD_QCOW2"
    info "FreeBSD raw image: $OUTPUT_DIR/$FREEBSD_RAW"
fi

# ─── EDK2 UEFI firmware ───────────────────────────────────────────────────────
EDK2_CODE="edk2-aarch64-code.fd"
EDK2_VARS="edk2-aarch64-vars.fd"

if [[ -f "$EDK2_CODE" ]]; then
    info "EDK2 firmware already present: $EDK2_CODE"
else
    # Try apt first (fastest on Debian/Ubuntu)
    if command -v dpkg >/dev/null 2>&1; then
        info "Trying apt for EDK2..."
        if apt-get install -y --no-install-recommends ovmf qemu-efi-aarch64 2>/dev/null; then
            # Ubuntu/Debian paths
            for src in \
                /usr/share/AAVMF/AAVMF_CODE.fd \
                /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
                /usr/share/ovmf/OVMF_CODE.fd; do
                if [[ -f "$src" ]]; then
                    cp "$src" "$EDK2_CODE"
                    info "Copied EDK2 from $src"
                    break
                fi
            done
            for src in \
                /usr/share/AAVMF/AAVMF_VARS.fd \
                /usr/share/qemu-efi-aarch64/QEMU_VARS.fd; do
                if [[ -f "$src" ]]; then
                    cp "$src" "$EDK2_VARS"
                    break
                fi
            done
        fi
    fi

    # Fallback: extract from deb manually (no sudo needed)
    if [[ ! -f "$EDK2_CODE" ]]; then
        info "Downloading EDK2 .deb package..."
        curl -L --progress-bar -o "$EDK2_DEB_FILE" "$EDK2_DEB_URL" \
            || die "Failed to download EDK2 .deb"

        mkdir -p edk2-extract
        cd edk2-extract
        ar x "../$EDK2_DEB_FILE"
        tar xf data.tar.* 2>/dev/null || tar xf data.tar.xz || tar xf data.tar.gz
        cd ..

        for candidate in \
            edk2-extract/usr/share/AAVMF/AAVMF_CODE.fd \
            edk2-extract/usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
            edk2-extract/usr/share/ovmf/OVMF_CODE.fd; do
            if [[ -f "$candidate" ]]; then
                cp "$candidate" "$EDK2_CODE"
                info "Extracted EDK2 from .deb: $candidate"
                break
            fi
        done

        # Try vars
        for candidate in \
            edk2-extract/usr/share/AAVMF/AAVMF_VARS.fd \
            edk2-extract/usr/share/qemu-efi-aarch64/QEMU_VARS.fd; do
            if [[ -f "$candidate" ]]; then
                cp "$candidate" "$EDK2_VARS"
                break
            fi
        done

        rm -rf edk2-extract "$EDK2_DEB_FILE"
    fi

    [[ -f "$EDK2_CODE" ]] || die "Could not obtain EDK2 firmware. Install manually: apt install qemu-efi-aarch64"
    info "EDK2 firmware: $OUTPUT_DIR/$EDK2_CODE"
fi

# Create a writable vars copy (EDK2 needs r/w access to vars store at runtime)
if [[ ! -f "$EDK2_VARS" ]]; then
    info "Creating empty EDK2 vars store (64MB)..."
    dd if=/dev/zero of="$EDK2_VARS" bs=1M count=64 2>/dev/null
fi

# ─── Summary ──────────────────────────────────────────────────────────────────
info ""
info "Guest images ready in: $OUTPUT_DIR"
info "  FreeBSD disk: $FREEBSD_RAW"
info "  EDK2 code:    $EDK2_CODE"
info "  EDK2 vars:    $EDK2_VARS"
info ""
info "Now build the VMM:"
info "  make BOARD=qemu_virt_aarch64 SYSTEM=freebsd"
info ""
info "Then launch FreeBSD under agentOS:"
info "  make BOARD=qemu_virt_aarch64 SYSTEM=freebsd demo-freebsd"
