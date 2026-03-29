#!/usr/bin/env bash
#
# agentOS SDK Setup
#
# Downloads and installs the correct Microkit SDK for the current host arch.
# The repo ships the linux-x86-64 SDK (built on do-host1). On AArch64 hosts
# (e.g. Sparky GB10), this script fetches the linux-aarch64 SDK and places it
# at the expected path so builds work without repo changes.
#
# Usage:
#   ./scripts/setup-sdk.sh [--force]
#
# Flags:
#   --force   Re-download even if SDK already exists at the correct path.
#
# Copyright (c) 2026 The agentOS Project
# SPDX-License-Identifier: BSD-2-Clause

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

SDK_VERSION="2.1.0"
SDK_BASE_URL="https://github.com/seL4/microkit/releases/download/1.4.1"
SDK_DIR="${PROJECT_DIR}/microkit-sdk-${SDK_VERSION}"

HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"  # linux / darwin
HOST_ARCH="$(uname -m)"  # x86_64 / aarch64 / arm64

FORCE=false
if [[ "${1:-}" == "--force" ]]; then
    FORCE=true
fi

echo "╔═══════════════════════════════════════════╗"
echo "║         agentOS SDK Setup                  ║"
echo "╚═══════════════════════════════════════════╝"
echo ""
echo "Host: ${HOST_OS} / ${HOST_ARCH}"
echo "SDK path: ${SDK_DIR}"
echo ""

# Normalize arch
case "${HOST_ARCH}" in
    x86_64)          SDK_ARCH="x86-64" ;;
    aarch64|arm64)   SDK_ARCH="aarch64" ;;
    *)
        echo "ERROR: Unsupported host architecture: ${HOST_ARCH}"
        echo "Supported: x86_64, aarch64/arm64"
        exit 1
        ;;
esac

# Normalize OS
case "${HOST_OS}" in
    linux)   SDK_OS="linux" ;;
    darwin)  SDK_OS="macos" ;;
    *)
        echo "ERROR: Unsupported OS: ${HOST_OS}"
        exit 1
        ;;
esac

SDK_TARBALL="microkit-sdk-${SDK_VERSION}-${SDK_OS}-${SDK_ARCH}.tar.gz"
SDK_URL="${SDK_BASE_URL}/${SDK_TARBALL}"

# Check if SDK at target path is already correct for this arch
check_sdk_arch() {
    local microkit_bin="${SDK_DIR}/bin/microkit"
    if [[ ! -f "${microkit_bin}" ]]; then
        return 1  # not present
    fi
    local bin_arch
    bin_arch="$(file "${microkit_bin}" 2>/dev/null)"
    case "${HOST_ARCH}" in
        x86_64)
            echo "${bin_arch}" | grep -q "x86-64" && return 0 || return 1
            ;;
        aarch64|arm64)
            echo "${bin_arch}" | grep -q "aarch64\|ARM aarch64" && return 0 || return 1
            ;;
    esac
}

if ! $FORCE && check_sdk_arch; then
    echo "✓ SDK at ${SDK_DIR} already matches host arch (${SDK_ARCH}). Nothing to do."
    echo "  Use --force to re-download."
    exit 0
fi

if check_sdk_arch 2>/dev/null; then
    echo "✓ Correct SDK already present (skipping download)."
    echo "  Use --force to re-download."
    exit 0
fi

echo "[1/3] SDK arch mismatch or missing — need ${SDK_OS}-${SDK_ARCH} variant."
echo "      Downloading: ${SDK_URL}"
echo ""

# Download to a temp file
TMP_TAR="$(mktemp /tmp/microkit-sdk-XXXXXX.tar.gz)"
trap 'rm -f "${TMP_TAR}"' EXIT

if command -v curl &>/dev/null; then
    curl -fL --progress-bar -o "${TMP_TAR}" "${SDK_URL}"
elif command -v wget &>/dev/null; then
    wget -q --show-progress -O "${TMP_TAR}" "${SDK_URL}"
else
    echo "ERROR: Neither curl nor wget found. Install one and retry."
    exit 1
fi

echo ""
echo "[2/3] Extracting SDK..."

# Back up existing SDK if present
if [[ -e "${SDK_DIR}" && ! -L "${SDK_DIR}" ]]; then
    BACKUP="${SDK_DIR}.bak-$(date +%s)"
    echo "  Backing up existing SDK → ${BACKUP}"
    mv "${SDK_DIR}" "${BACKUP}"
elif [[ -L "${SDK_DIR}" ]]; then
    echo "  Removing existing symlink at ${SDK_DIR}"
    rm "${SDK_DIR}"
fi

EXTRACT_TMP="${PROJECT_DIR}/.sdk-extract-tmp"
rm -rf "${EXTRACT_TMP}"
mkdir -p "${EXTRACT_TMP}"

tar -xzf "${TMP_TAR}" -C "${EXTRACT_TMP}"

# SDK tarballs typically extract to microkit-sdk-VERSION/
EXTRACTED="$(find "${EXTRACT_TMP}" -maxdepth 1 -type d | grep -v "^${EXTRACT_TMP}$" | head -1)"
if [[ -z "${EXTRACTED}" ]]; then
    echo "ERROR: Could not find extracted SDK directory."
    exit 1
fi

mv "${EXTRACTED}" "${SDK_DIR}"
rm -rf "${EXTRACT_TMP}"

echo "[3/3] Verifying SDK..."

MICROKIT_BIN="${SDK_DIR}/bin/microkit"
if [[ -f "${MICROKIT_BIN}" ]]; then
    echo "  microkit binary: $(file "${MICROKIT_BIN}")"
    echo ""
    echo "╔═══════════════════════════════════════════╗"
    echo "║  SDK setup complete ✓                      ║"
    echo "╠═══════════════════════════════════════════╣"
    echo "║  SDK:  ${SDK_DIR}"
    echo "║  Arch: ${SDK_OS}-${SDK_ARCH}"
    echo "║                                           ║"
    echo "║  Build with:                              ║"
    echo "║    make BOARD=qemu_virt_aarch64           ║"
    echo "║         MICROKIT_SDK=${SDK_DIR}"
    echo "╚═══════════════════════════════════════════╝"
else
    echo "WARNING: microkit binary not found at expected path."
    echo "  Check SDK tarball structure and update this script if needed."
    exit 1
fi
