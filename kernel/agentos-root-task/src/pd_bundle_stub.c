/*
 * pd_bundle_stub.c — empty compilation unit for the PD bundle facility
 *
 * Every architecture this tree builds embeds a PD bundle:
 *
 *   AArch64, x86_64:
 *     __pd_bundle_start / __pd_bundle_end and __pd_manifest_start /
 *     __pd_manifest_end come from the objcopy-generated objects
 *     (agentos_pd_bundle.o, agentos_pd_manifest.o) linked into
 *     root_task.elf.  The sections are placed by tools/ld/root_task.ld
 *     (AArch64) or tools/ld/agentos.ld (x86_64).
 *
 *   RISC-V:
 *     the same four symbols come from src/pd_bundle_riscv64.S, which
 *     .incbin's the same two blobs.  objcopy's binary input mode cannot be
 *     used there — its output carries the soft-float ABI tag and ld.lld
 *     refuses to link it against the lp64d root task.  Sections are placed
 *     by tools/ld/root_task_riscv64.ld.
 *
 * This file intentionally has no code.  It exists to ensure that the
 * ROOT_TASK_SRCS list has a stable entry for this facility across all targets.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* (intentionally empty) */
