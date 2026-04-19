#
# agentOS — Top-Level CMake Build
#
# Builds the agentOS system image on top of seL4.
# Supports QEMU simulation and hardware targets.
#
# Usage:
#   mkdir build && cd build
#   cmake -G Ninja \
#     -DPLATFORM=qemu-arm-virt \
#     -DSIMULATION=1 \
#     -DCROSS_COMPILER_PREFIX=aarch64-linux-gnu- \
#     ..
#   ninja
#
# Copyright (c) 2026 The agentOS Project
# SPDX-License-Identifier: BSD-2-Clause
#

cmake_minimum_required(VERSION 3.18)
project(agentOS C ASM)

# =============================================================================
# seL4 Kernel Configuration
# =============================================================================

# Import seL4 build system
find_package(seL4 REQUIRED)
find_package(elfloader-tool REQUIRED)

# seL4 platform settings
set(KernelArch "aarch64" CACHE STRING "Target architecture")
set(KernelPlatform "qemu-arm-virt" CACHE STRING "Target platform")
set(KernelMaxNumNodes 1 CACHE STRING "Max CPU cores")

# Enable MCS scheduling (mixed criticality — better for agent priority mgmt)
set(KernelIsMCS ON CACHE BOOL "Use MCS scheduling model")

# Debug settings (disable for production)
set(KernelDebugBuild ON CACHE BOOL "Debug build")
set(KernelPrinting ON CACHE BOOL "Kernel debug printing")

# =============================================================================
# agentOS Libraries
# =============================================================================

# libagent — agentOS SDK
add_library(libagent STATIC
    libs/libagent/libagent.c
)

target_include_directories(libagent PUBLIC
    libs/libagent/include
)

target_link_libraries(libagent
    sel4
    muslc
    sel4muslcsys
    sel4utils
    sel4platsupport
)

# =============================================================================
# System Services
# =============================================================================

# CapStore — Capability database
add_library(capstore STATIC
    services/capstore/capstore.c
)
target_include_directories(capstore PUBLIC services/capstore)
target_link_libraries(capstore libagent)

# MsgBus — Inter-agent communication
add_library(msgbus STATIC
    services/msgbus/msgbus.c
    services/msgbus/msgbus_seL4.c
)
target_include_directories(msgbus PUBLIC services/msgbus)
target_link_libraries(msgbus libagent)

# MemFS — Virtual filesystem
add_library(memfs STATIC
    services/memfs/memfs.c
)
target_include_directories(memfs PUBLIC services/memfs)
target_link_libraries(memfs libagent)

# ToolSvc — Tool registry and dispatch
add_library(toolsvc STATIC
    services/toolsvc/toolsvc.c
)
target_include_directories(toolsvc PUBLIC services/toolsvc)
target_link_libraries(toolsvc libagent)

# ModelSvc — Model inference proxy
add_library(modelsvc STATIC
    services/modelsvc/modelsvc.c
)
target_include_directories(modelsvc PUBLIC services/modelsvc)
target_link_libraries(modelsvc libagent)

# NetStack and BlobSvc are NOT present as services/ C prototypes.
# Real network is handled by the net_server.c Microkit PD.
# Block storage is handled by virtio_blk.c Microkit PD.
# These targets have been removed to fix the broken build.

# LogSvc — Audit and logging
add_library(logsvc STATIC
    services/logsvc/logsvc.c
)
target_include_directories(logsvc PUBLIC services/logsvc)
target_link_libraries(logsvc libagent)

# =============================================================================
# Init Task (Root Task)
# =============================================================================

add_executable(agentos-init
    agents/init/main.c
    agents/init/init_services.c
    agents/init/init_agents.c
)

target_link_libraries(agentos-init
    libagent
    capstore
    msgbus
    memfs
    toolsvc
    modelsvc
    logsvc
    sel4
    muslc
    sel4muslcsys
    sel4utils
    sel4platsupport
    sel4simple-default
    sel4allocman
    # NOTE: netstack and blobsvc removed — source files do not exist.
    # Real networking: net_server.c Microkit PD.
    # Real block storage: virtio_blk.c Microkit PD.
)

# =============================================================================
# Hello Agent
# =============================================================================

add_executable(agentos-hello
    agents/hello/main.c
)

target_link_libraries(agentos-hello
    libagent
    sel4
    muslc
    sel4muslcsys
    sel4utils
)

# =============================================================================
# System Image
# =============================================================================

# Build the bootable system image with elfloader
# The init task is loaded first, then it bootstraps everything else
include(rootserver)
DeclareRootserver(agentos-init)

# Include agent binaries in the boot image
# (init task will load these from the image and start them)
# TODO: Proper CPIO archive of agent binaries

# =============================================================================
# QEMU Simulation Target
# =============================================================================

if(SIMULATION)
    add_custom_target(simulate
        COMMAND qemu-system-aarch64
            -machine virt,virtualization=on,highmem=off,secure=off
            -cpu cortex-a53
            -nographic
            -m 512
            -kernel ${CMAKE_BINARY_DIR}/images/agentos-init-image-arm-qemu-arm-virt
        DEPENDS agentos-init
        COMMENT "Running agentOS in QEMU (aarch64)"
    )
endif()

# =============================================================================
# Meta
# =============================================================================

message(STATUS "")
message(STATUS "╔═══════════════════════════════════════════╗")
message(STATUS "║         agentOS Build Configuration        ║")
message(STATUS "╠═══════════════════════════════════════════╣")
message(STATUS "║  Platform:    ${KernelPlatform}")
message(STATUS "║  Arch:        ${KernelArch}")
message(STATUS "║  MCS:         ${KernelIsMCS}")
message(STATUS "║  Debug:       ${KernelDebugBuild}")
message(STATUS "║  Simulation:  ${SIMULATION}")
message(STATUS "╚═══════════════════════════════════════════╝")
message(STATUS "")
