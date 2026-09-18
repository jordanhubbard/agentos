# Microkit SDK compatibility

The default remains Microkit 2.1.0. Microkit 2.3.0 can be selected explicitly
with `SEL4_SDK_VERSION=2.3.0`; install it through `make sdk` before building.
Its separate cache directory preserves the default SDK. No seL4 source changes
are part of this compatibility work.

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
