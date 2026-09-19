# x86 architectural context dependency

Linux SMP remains unqualified. The strict gate rejects recurring userspace
faults with both the reused disk and a journal-recovered baseline copy.
Disabling nested VMX VPID does not eliminate the failure.

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
causation has not been established by a corrected-kernel comparison.

## Upstream VMX fix found on 2026-09-19

Upstream [seL4 PR 1732](https://github.com/seL4/seL4/pull/1732) is merged at
`e60776acc31097ca063806c257f07a3ec05eacf8`. It fixes VM execution when
multiple VCPUs share a physical core, as well as host SMP VMX setup. This is
a more direct supported dependency candidate than a new CR2 implementation.
It has not yet been qualified with agentOS; the cause of our Linux failure
remains unproven.

[Microkit 2.3.1 release notes](https://docs.sel4.systems/releases/microkit/2.3.1)
explicitly exclude this fix and warn about multiple VMs and x86 VTX SMP.
Installing the 2.3.1 release archive alone therefore cannot qualify this
dependency. The release points to PR 1732 for the future correction.

The proposed qualification candidate uses these exact, unmodified upstream
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
No kernel source or SDK binary has been changed in this investigation.
An explicit project exception is required before pursuing a kernel change.

The earlier proposed exception was limited to upstream-compatible preservation of
CR2 as native VCPU architectural state, including initialization and separate
host state where required. It does not extend agentOS device authority or
replace seL4 with a project fork. An approved change would require a pinned,
reproducible SDK build and renewed native, single-CPU, two-CPU, lifecycle and
full Spark qualification. No release may infer Linux SMP success from the
short native tests or from this source audit. Upstream publication requires
separate authorization.

That custom-kernel proposal is deferred in favor of qualifying the already
merged upstream revision above. The ledger currently retains an explicit
pending-authorization boundary on SDK rebuilding; approval of this concrete
unmodified-upstream candidate is required before replacing that boundary.
No custom kernel patch is proposed by this plan.
