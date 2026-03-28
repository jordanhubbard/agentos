#!/usr/bin/env bash
#
# agentOS Development Environment Setup
#
# This script sets up everything needed to build and run agentOS:
#   1. Install dependencies (via Homebrew on macOS, apt on Linux)
#   2. Clone seL4 repositories
#   3. Set up cross-compilation toolchain
#   4. Configure QEMU for simulation
#
# Usage:
#   ./scripts/setup-dev.sh
#
# Copyright (c) 2026 The agentOS Project
# SPDX-License-Identifier: BSD-2-Clause

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEPS_DIR="${PROJECT_DIR}/deps"

echo "╔═══════════════════════════════════════════╗"
echo "║     agentOS Development Environment Setup  ║"
echo "╚═══════════════════════════════════════════╝"
echo ""
echo "Project directory: ${PROJECT_DIR}"
echo "Dependencies: ${DEPS_DIR}"
echo ""

# =============================================================================
# Detect OS
# =============================================================================

OS="$(uname -s)"
ARCH="$(uname -m)"

echo "[1/6] Detected: ${OS} ${ARCH}"

# =============================================================================
# Install system dependencies
# =============================================================================

echo "[2/6] Installing system dependencies..."

if [[ "$OS" == "Darwin" ]]; then
    # macOS via Homebrew
    if ! command -v brew &>/dev/null; then
        echo "ERROR: Homebrew not found. Install from https://brew.sh"
        exit 1
    fi
    
    brew install --quiet \
        cmake ninja python3 \
        qemu \
        aarch64-elf-gcc \
        xmllint \
        repo \
        dtc \
        protobuf \
        coreutils \
        2>/dev/null || true
    
    # Python deps for seL4 build tools
    pip3 install --quiet --user \
        sel4-deps \
        aenum \
        jinja2 \
        plyplus \
        pyelftools \
        orderedset \
        pyfdt \
        future \
        six \
        lxml \
        2>/dev/null || true

elif [[ "$OS" == "Linux" ]]; then
    # Linux (Ubuntu/Debian)
    if command -v apt-get &>/dev/null; then
        sudo apt-get update -qq
        sudo apt-get install -y -qq \
            build-essential cmake ninja-build \
            gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
            qemu-system-arm qemu-system-aarch64 \
            python3 python3-pip python3-venv \
            device-tree-compiler \
            libxml2-utils \
            git repo \
            protobuf-compiler \
            curl wget \
            ccache
        
        pip3 install --user sel4-deps
    else
        echo "WARNING: Non-Debian Linux detected. Install deps manually."
        echo "Need: cmake, ninja, aarch64 cross-compiler, qemu, python3, repo"
    fi
else
    echo "ERROR: Unsupported OS: ${OS}"
    exit 1
fi

echo "  Dependencies installed."

# =============================================================================
# Clone seL4 repositories
# =============================================================================

echo "[3/6] Setting up seL4 repositories..."

mkdir -p "${DEPS_DIR}"

# seL4 kernel
if [[ ! -d "${DEPS_DIR}/seL4" ]]; then
    echo "  Cloning seL4 kernel..."
    git clone --depth 1 https://github.com/seL4/seL4.git "${DEPS_DIR}/seL4"
else
    echo "  seL4 kernel already present."
fi

# seL4 libraries (sel4utils, sel4platsupport, etc.)
if [[ ! -d "${DEPS_DIR}/seL4_libs" ]]; then
    echo "  Cloning seL4 libraries..."
    git clone --depth 1 https://github.com/seL4/seL4_libs.git "${DEPS_DIR}/seL4_libs"
else
    echo "  seL4 libraries already present."
fi

# seL4 tools (build system, elfloader)
if [[ ! -d "${DEPS_DIR}/seL4_tools" ]]; then
    echo "  Cloning seL4 tools..."
    git clone --depth 1 https://github.com/seL4/seL4_tools.git "${DEPS_DIR}/seL4_tools"
else
    echo "  seL4 tools already present."
fi

# CAmkES (component framework)
if [[ ! -d "${DEPS_DIR}/camkes-tool" ]]; then
    echo "  Cloning CAmkES..."
    git clone --depth 1 https://github.com/seL4/camkes-tool.git "${DEPS_DIR}/camkes-tool"
else
    echo "  CAmkES already present."
fi

# musllibc (C library for seL4)
if [[ ! -d "${DEPS_DIR}/musllibc" ]]; then
    echo "  Cloning musllibc..."
    git clone --depth 1 https://github.com/seL4/musllibc.git "${DEPS_DIR}/musllibc"
else
    echo "  musllibc already present."
fi

# sel4runtime
if [[ ! -d "${DEPS_DIR}/sel4runtime" ]]; then
    echo "  Cloning sel4runtime..."
    git clone --depth 1 https://github.com/seL4/sel4runtime.git "${DEPS_DIR}/sel4runtime"
else
    echo "  sel4runtime already present."
fi

# util_libs
if [[ ! -d "${DEPS_DIR}/util_libs" ]]; then
    echo "  Cloning util_libs..."
    git clone --depth 1 https://github.com/seL4/util_libs.git "${DEPS_DIR}/util_libs"
else
    echo "  util_libs already present."
fi

echo "  seL4 repositories ready."

# =============================================================================
# Verify cross-compiler
# =============================================================================

echo "[4/6] Verifying cross-compilation toolchain..."

if [[ "$OS" == "Darwin" ]]; then
    CROSS_PREFIX="aarch64-elf-"
else
    CROSS_PREFIX="aarch64-linux-gnu-"
fi

if command -v "${CROSS_PREFIX}gcc" &>/dev/null; then
    echo "  Cross-compiler: $(${CROSS_PREFIX}gcc --version | head -1)"
else
    echo "  WARNING: ${CROSS_PREFIX}gcc not found in PATH"
    echo "  You may need to install it or adjust PATH"
fi

# =============================================================================
# Verify QEMU
# =============================================================================

echo "[5/6] Verifying QEMU..."

if command -v qemu-system-aarch64 &>/dev/null; then
    echo "  QEMU: $(qemu-system-aarch64 --version | head -1)"
else
    echo "  WARNING: qemu-system-aarch64 not found"
    echo "  Install QEMU for simulation support"
fi

# =============================================================================
# Create build configuration
# =============================================================================

echo "[6/6] Creating build configuration..."

# Write a settings.cmake that points to all deps
cat > "${PROJECT_DIR}/settings.cmake" << EOF
#
# agentOS Build Settings (auto-generated by setup-dev.sh)
# Generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)
#

# seL4 dependencies
set(SEL4_KERNEL_DIR "${DEPS_DIR}/seL4" CACHE PATH "seL4 kernel source")
set(SEL4_LIBS_DIR "${DEPS_DIR}/seL4_libs" CACHE PATH "seL4 libraries")
set(SEL4_TOOLS_DIR "${DEPS_DIR}/seL4_tools" CACHE PATH "seL4 tools")
set(CAMKES_DIR "${DEPS_DIR}/camkes-tool" CACHE PATH "CAmkES tool")
set(MUSLLIBC_DIR "${DEPS_DIR}/musllibc" CACHE PATH "musl libc")
set(SEL4_RUNTIME_DIR "${DEPS_DIR}/sel4runtime" CACHE PATH "seL4 runtime")
set(UTIL_LIBS_DIR "${DEPS_DIR}/util_libs" CACHE PATH "Utility libraries")

# Cross-compiler
set(CROSS_COMPILER_PREFIX "${CROSS_PREFIX}" CACHE STRING "Cross-compiler prefix")

# Default target
set(PLATFORM "qemu-arm-virt" CACHE STRING "Target platform")
set(SIMULATION ON CACHE BOOL "Build for simulation")
EOF

echo "  Build settings written to settings.cmake"

echo ""
echo "╔═══════════════════════════════════════════╗"
echo "║          Setup Complete! 🫎                ║"
echo "╠═══════════════════════════════════════════╣"
echo "║                                           ║"
echo "║  To build:                                ║"
echo "║    mkdir build && cd build                ║"
echo "║    cmake -G Ninja                         ║"
echo "║      -C ../settings.cmake                ║"
echo "║      ..                                   ║"
echo "║    ninja                                  ║"
echo "║                                           ║"
echo "║  To simulate:                             ║"
echo "║    ninja simulate                         ║"
echo "║                                           ║"
echo "╚═══════════════════════════════════════════╝"
