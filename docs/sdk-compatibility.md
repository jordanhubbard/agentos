# Microkit SDK compatibility

The v0.4 default is the approved `2.3.1-agentos-e60776ac-cr2` target bundle,
selected by `tools/sdk/default-version`. Its [source pins, scoped kernel patch,
packaging and qualification](x86-cr2-candidate.md) are separate from the earlier
userspace API compatibility work below. CI builds and verifies the same bundle.

Microkit 2.1.0 and 2.3.0 remain selectable with `SEL4_SDK_VERSION` for historical
compatibility checks. Their separate cache directories are preserved. The
original API compatibility changes below did not alter seL4 source.

Three userspace API adjustments permit both versions:

- ISR_EL1 is read-only and no longer appears in the newer ARM VCPU register
  API. Do not reset it or include it in the saved-VCPU diagnostic dump.
- The x86 VM-entry interruption-info message register has a corrected name.
  Select the new enum when the deprecated old-name macro exists, retaining
  the old enum for SDK 2.1 without expanding a compiler-specific pragma.
- New EPT mapping signatures accept the EPT attribute enum. Preserve the EPT
  default wire value through a `seL4_Word` instead of converting between
  ordinary VM and EPT attribute enums.

The [qualification receipt](evidence/2026-09-17-spark/sdk-compatibility.json)
records revisions, commands, archive checksums, retained logs and images.
Both versions passed `make gate` on Spark: host tests, AArch64/reduced-x86
root boot, and AArch64 guest network, block and console proofs. After the
VTX-only compatibility adjustments, SDK 2.1 cross-built the VTX image and
SDK 2.3 passed `make gate-x86_64-vtx` on the accessible Intel host madmax.
That proof checks one EPT-backed long-mode HLT exit.

## Firmware prerequisite

The official [Microkit 2.3.0 manifest](https://github.com/seL4/microkit-manifest/blob/2.3.0/main.xml)
pins seL4 revision `6e7c3b733d296cfd88d5fbf635c96e447a882374`, which includes
the upstream [guest-mode control change](https://github.com/seL4/seL4/commit/0d2c70c7f0121181243c2c5dc2fa54213bb223ff).
This gives the firmware work a supported upstream dependency to evaluate.
Long-mode HLT success does not qualify real-mode reset, protected-mode
transitions, UEFI, Linux, installed ACPI tables or guest device emulation.
Those require their own implementation and target evidence before changing
the default SDK or making a v0.4 release claim.

The separate `make gate-x86_64-firmware-modes SEL4_SDK_VERSION=2.3.0`
qualification enters real-address, unpaged protected and long mode in order
on an Intel KVM host. Each entry resets the VCPU state, preserves unrelated
control bits, enables unrestricted guest execution and checks relevant VMCS
readback before requiring the exact EPT-backed HLT exit. The harness requires
a fresh build and a distinct success marker; SDK 2.1 cannot compile this
variant. Switching back to `make gate-x86_64-vtx SEL4_SDK_VERSION=2.1.0`
rebuilds the VMM and root for the original long-mode-only proof.

These are VMM-selected entry states at GPA `0x1000`, not execution from the
architectural reset vector or guest-driven mode transitions. Firmware payload
loading, reset-vector mappings, exit emulation, virtual interrupts and actual
UEFI/Linux boot remain required. No new device ownership is introduced.

The [entry-mode receipt](evidence/2026-09-17-spark/firmware-entry-modes.json)
records the Intel mode tests, cross-SDK rebuild regression and Spark full gate.
