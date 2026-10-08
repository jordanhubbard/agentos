/*
 * pd_bundle_riscv64.S — embed the PD bundle and signed boot manifest
 *
 * AArch64 and x86_64 turn `xtask gen-pd-bundle`'s two binary outputs into
 * relocatable objects with `llvm-objcopy --input-target=binary`.  That does
 * not work on RISC-V: objcopy emits an ELF whose e_flags carry the default
 * soft-float ABI, and ld.lld then refuses to link it against the rest of the
 * root task, which is built lp64d:
 *
 *   ld.lld: error: agentos_pd_bundle.o: cannot link object files with
 *   different floating-point ABI from rt_start_riscv64.o
 *
 * Assembling the blobs with `.incbin` instead produces objects built by the
 * same compiler driver and the same -march/-mabi flags as every other root
 * task object, so the ABI tags match by construction rather than by a
 * post-hoc e_flags patch.
 *
 * This defines the four symbols main.c actually consumes --
 * __pd_bundle_start/__pd_bundle_end and __pd_manifest_start/
 * __pd_manifest_end -- so main.c and tools/ld/root_task_riscv64.ld are
 * unchanged.  Note this is a SUBSET of what objcopy emits on the other
 * architectures: objcopy also produces __pd_bundle_size and
 * __pd_manifest_size, which are NOT defined here.  main.c computes both
 * lengths by pointer subtraction, so nothing needs them today -- but code
 * that referenced them would link on aarch64/x86_64 and fail only on
 * RISC-V.  Add them here before using them anywhere.
 *
 * AGENTOS_PD_BUNDLE_PATH / AGENTOS_PD_MANIFEST_PATH are passed as -D by
 * kernel/agentos-root-task/Makefile.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

    .section .pd_bundle, "a", @progbits
    .balign 16
    .globl __pd_bundle_start
    .globl __pd_bundle_end
__pd_bundle_start:
    .incbin AGENTOS_PD_BUNDLE_PATH
__pd_bundle_end:

    .section .pd_manifest, "a", @progbits
    .balign 16
    .globl __pd_manifest_start
    .globl __pd_manifest_end
__pd_manifest_start:
    .incbin AGENTOS_PD_MANIFEST_PATH
__pd_manifest_end:
