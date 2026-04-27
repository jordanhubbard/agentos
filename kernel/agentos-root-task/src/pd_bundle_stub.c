/*
 * pd_bundle_stub.c — empty compilation unit for the PD bundle facility
 *
 * On AArch64:
 *   __pd_bundle_start / __pd_bundle_end are provided by the objcopy-generated
 *   object (agentos_pd_bundle.o) which is linked into root_task.elf.
 *   The .pd_bundle section is placed by tools/ld/root_task.ld.
 *
 * On non-AArch64 (RISC-V, x86_64):
 *   __pd_bundle_start / __pd_bundle_end are zero-size linker-script symbols
 *   defined in tools/ld/agentos.ld.  bundle_size() in main.c returns 0 and
 *   the bundle path is skipped in favour of the seL4 extra BootInfo scan.
 *
 * This file intentionally has no code.  It exists to ensure that the
 * ROOT_TASK_SRCS list has a stable entry for this facility across all targets.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* (intentionally empty) */
