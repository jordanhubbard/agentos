# x86 architectural context dependency

The approved CR2 candidate passed the strict Linux SMP gate in both managed
generations at `de1458c`; see the
[Intel receipt](evidence/2026-09-19-spark/cr2-intel-smp.json). Default SDK
adoption and final release qualification remain incomplete. Earlier strict
runs rejected recurring userspace faults with both the reused disk and a
journal-recovered baseline copy. Disabling nested VMX VPID did not eliminate
those failures.

At `41b3e9b`, native scratch-RAM tests pass for distinct GPR, x87 and SSE
values across two runner switches. Separate guest page-table roots also
retain their mappings, including guest MOV CR3 changes and subsequent peer
execution. Both tests repeat after reconstruction; the full Spark gate passes.
See [the receipt](evidence/2026-09-18-spark/x86-native-context.json).
These bounded tests do not establish complete architectural context isolation.

## Source-level concern

Microkit 2.3.0 pins seL4 revision
`6e7c3b733d296cfd88d5fbf635c96e447a882374`. Inspection of its
[VCPU storage](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/include/arch/x86/arch/object/vcpu.h),
[entry path](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/src/arch/x86/64/c_traps.c),
[exit path](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/src/arch/x86/64/traps.S)
and VCPU invocation implementation found no per-VCPU CR2 storage or
save/restore path, nor a userspace invocation for managing that register.
CR2 contains the guest page-fault address. This is an architectural-context
dependency and a plausible explanation for the observed Linux faults;
the passing candidate comparison does not establish that it explains every
previous Linux fault.

## Upstream VMX fix found on 2026-09-19

Upstream [seL4 PR 1732](https://github.com/seL4/seL4/pull/1732) is merged at
`e60776acc31097ca063806c257f07a3ec05eacf8`. It fixes VM execution when
multiple VCPUs share a physical core, as well as host SMP VMX setup. This is
the first upstream dependency candidate tested before applying the CR2 patch.
It passed managed single-CPU qualification but still failed the strict SMP
workload during boot, as recorded below.

[Microkit 2.3.1 release notes](https://docs.sel4.systems/releases/microkit/2.3.1)
explicitly exclude this fix and warn about multiple VMs and x86 VTX SMP.
Installing the 2.3.1 release archive alone therefore cannot qualify this
dependency. The release points to PR 1732 for the future correction.

The unmodified qualification candidate uses these exact upstream
source revisions:

| Component | Revision |
| --- | --- |
| Microkit 2.3.1 | `ec86afdcd662b5976d11d4994acf1b11a2979882` |
| seL4 with merged VMX fixes | `e60776acc31097ca063806c257f07a3ec05eacf8` |

Build the SDK in a separate external directory, with an explicit candidate
version, using the upstream SDK builder. Retain source identities, clean-tree
checks, toolchain versions, build configuration, complete build logs and
artifact hashes. Build the x86 VTX release configuration first; qualify the
remaining configurations required by `make gate` before considering adoption.
Do not overwrite installed release SDKs or relabel this candidate as an
official Microkit release. Use the existing `SEL4_SDK` Make override for the
candidate; default pins remain unchanged until qualification succeeds.

Acceptance requires the existing Intel native context/lifecycle checks,
single-CPU Linux regression, and `make gate-x86_64-smp` with two online CPUs
and the ordinary CPU-affined workload in both managed generations. Use
disposable disk copies with recorded baseline hashes. Then run the complete
Spark `make gate` against the same candidate, retaining commands and receipts.
A successful SDK build, or the upstream merge itself, is not acceptance.
If faults persist, retain the failing results and reassess the architectural
context hypothesis without claiming that CR2 is the cause.

## Proposed change boundary

The constitution says: "seL4 is the only kernel-mode code. Never modify it."
The candidate uses unmodified upstream source and an isolated SDK directory.
This dependency qualification is covered by the user's v0.4 implementation,
test and release request; it requires no exception to the prohibition on
local kernel patches. The earlier agent-imposed blanket SDK build hold was
unnecessary and has been corrected in the ledger. Installed release SDKs and
default pins remain unchanged during qualification.

The user explicitly approved both this isolated upstream qualification and
the scoped upstream-compatible CR2 fix with full requalification on
2026-09-19. This authorizes a narrow exception to the local-kernel-change
rule if that fix remains necessary. The unmodified upstream candidate also
failed Linux SMP; the approved [CR2 candidate](x86-cr2-candidate.md) is now
under qualification in a separate source tree and SDK directory. Upstream
publication is not part of this approval. Adoption still requires reproducible
build evidence and renewed native, single-CPU, two-CPU, lifecycle and full
Spark qualification. No release may infer Linux SMP success from the short
native tests, a successful build or the upstream source audit.

## Initial candidate qualification

The isolated GNU build and a clean rebuild in a second directory produced
byte-identical seL4 kernels and SDK headers for the release configurations
of `qemu_virt_aarch64`, `x86_64_generic` and `x86_64_generic_vtx`. This is not
a claim that the entire SDK archive reproduces: auxiliary Microkit runtime
artifacts differ, including the stripped Rust initialiser. agentOS uses its
own loader/root task and does not link those runtime artifacts.

The Intel userspace/teardown gate passed with this candidate at `979ec65`.
The first managed Debian attempt reached login but used a known-hosts pin
from a different seeded image; it was aborted and is not accepted. The
baseline disk's public host key was then read through a read-only loop device
and matched the observed key. Both managed one-CPU generations then passed
at `4143e14`. The strict SMP gate at that revision failed before the workload:
`systemd-remount` faulted in the dynamic loader on CPU 0. The unmodified VMX
fix is therefore insufficient for this workload. The subsequent CR2 candidate
passed both SMP generations; the failure alone does not establish its cause.

The first Spark gate stopped at kernel entry: the new ARM kernel's ELF entry
is `0xffc0000000`, while the loader used a fixed `0x8060000000` branch target
and mapping. The loader now maps ELF load segments with 2 MiB blocks and
branches to `e_entry`. A host page-table walk checks both old and new layouts,
including segment tails and overlapping blocks with matching translations.
The full candidate `make gate` and the installed 2.3.0 ARM boot regression
then passed. The [initial receipt](evidence/2026-09-19-spark/upstream-sdk-initial.json)
records hashes and limitations; final integrated revision qualification is
still required before adoption or release.
