# x86 EFI boot payload delivery

The opt-in OVMF reset composition can expose a complete EFI application image
through fw_cfg. EFI-stub Linux kernels use this path. Firmware receives the
whole image as `KernelData`, with `KernelSetupSize` zero, so this interface
does not supply a legacy Linux setup/entry address or bypass UEFI loading.
The VMM does not parse or execute a PE image itself; OVMF owns image validation
and loading. A malformed or unsupported guest image is not a host executable.

## Inputs and authority

`aos_x86_config_boot` binds immutable VMM-owned kernel, optional initrd and
optional command-line buffers once, before the first fw_cfg read. Kernel and
initrd are individually bounded at 64 MiB. The command line is at most 4096
bytes including one final NUL; preceding bytes must be printable ASCII.
Invalid descriptors leave state unchanged. Storage remains alive for the
guest lifetime, and no descriptor comes from a guest address.

The Make interface copies each supplied file into the build directory, checks
its explicit SHA-256 and size, then embeds it in a read-only VMM ELF section.
Root provisions 256 MiB of private RAM for this opt-in variant; firmware-only
builds retain 32 MiB. These are fixed bring-up compositions, not runtime
desktop resource profiles. A compile-time bound keeps RAM below the VMM's
ROM mapping and requires whole 2 MiB pages. Allocation failure aborts boot. Neither variant
qualifies teardown reclamation.

No payload source has a direct guest EPT mapping. fw_cfg returns the standard
little-endian sizes and byte streams; selecting an item resets its offset,
reads after its end return zero, and offsets/counters saturate without wrapping.
REP INSB validates and commits at most 1024 bytes at a time, including all
destination pages and accessed/dirty updates. Rejected destinations consume
no bytes. This increases transfer work per exit from the former 256-byte
chunk while preserving the 65,536 processed-exit limit and its failure result.
The VMM has a 64 KiB stack in the qualification topology.

The source ABI is OVMF's
[QemuKernelLoaderFsDxe](https://github.com/tianocore/edk2/blob/edk2-stable202402/OvmfPkg/QemuKernelLoaderFsDxe/QemuKernelLoaderFsDxe.c)
and [QemuFwCfg item definitions](https://github.com/tianocore/edk2/blob/edk2-stable202402/OvmfPkg/Include/IndustryStandard/QemuFwCfg.h).
Blob delivery is boot input, not a replacement for canonical virtio block,
network or console services, nor persistent guest storage.

## Reproduce on Intel

With the pinned OVMF image from [firmware bring-up](x86-firmware.md), supply
an EFI-stub Linux image and its independently recorded digest:

```sh
make gate-x86_64-firmware-reset SEL4_SDK_VERSION=2.3.0 \
  X86_FIRMWARE_IMAGE=/tmp/agentos-ovmf-2024.02-2ubuntu0.9.fd \
  X86_FIRMWARE_SHA256=101afad5f520753224b60b9555df60add435cf50c7a82b781d7d7d8c60db7eb5 \
  X86_BOOT_KERNEL=/tmp/agentos-linux-7.0.0-30.efi \
  X86_BOOT_KERNEL_SHA256=0ad39b13e289e1a5cf806d14541ac8f221eefe849017f5915e0846917ed67785
```

Optional inputs are `X86_BOOT_INITRD` with `X86_BOOT_INITRD_SHA256`, and
`X86_BOOT_CMDLINE_FILE` with `X86_BOOT_CMDLINE_SHA256`. Both require a kernel.
The command-line file must contain the terminating NUL; a text file ending
in a newline is rejected by the VMM. Omit optional inputs instead of passing
empty files. Each input file is externally supplied; binaries are not committed.

## Evidence and remaining work

The [Intel receipt](evidence/2026-09-17-spark/ovmf-boot-payload.json) records
runtime `8895952` consuming all 17,295,752 kernel bytes from Ubuntu package
`linux-image-7.0.0-30-generic` version `7.0.0-30.30`. Its PE header identifies
AMD64, EFI application subsystem, and entry RVA `0x106ff48`.
No initrd or command line was supplied in this run.

The gate still fails at its exit bound. Last RIP `0x070ff31d` is an IN EAX,DX
instruction matching OVMF's Metronome driver; exit qualification `0xb008000b`
identifies a 32-bit PM-timer read. Transfer counters prove bytes consumed, not
their execution. The reason for this subsequent firmware delay remains to be
resolved. EFI payload entry, Linux userspace, generated ACPI and the canonical
x86 guest-device paths remain unqualified.

The [subsequent caller receipt](evidence/2026-09-17-spark/ovmf-boot-caller.json)
records runtime `9a144ae` progressing to 401 HLT exits within the same exit
bound. Its retained chain reaches return `0x578b2bb`, matching the pinned
OVMF Shell's `WaitForEvent` return at RVA `0x82bb`, followed by return
`0x57a191d`, matching the same module at RVA `0x1e91d`. Both imply load
base `0x5783000`. The first call waits on `ConIn->WaitForKey`.
This identifies the observed endpoint as the firmware shell; it does not
establish the earlier Linux loader result or whether the kernel briefly
entered and returned. Diagnose that earlier result before changing boot
inputs or treating a longer exit budget as progress. Spark's full gate also
passed at this runtime revision.

The [memory and Linux-entry receipt](evidence/2026-09-17-spark/ovmf-linux-entry.json)
supersedes that endpoint. Stock QEMU with the same firmware and kernel reports
EFI decompression failure at 128 MiB and reaches Linux initialization at
256 MiB (then panics because no root filesystem was supplied). These are
controls, not agentOS qualification. Increasing this composition to 256 MiB
also moved the seL4 run into the decompressed Linux kernel: runtime `7570527`
stopped on Linux's EFER write enabling SCE. Runtime `0d5c3ae` accepts that
architectural state and reaches the kernel's legacy PIT calibration read at
port `0x61`. Its exact instruction matches the extracted, pinned kernel ELF.
The Intel gate still fails because that port is unimplemented; userspace,
generated ACPI and canonical guest I/O remain outstanding.

EFER writes now validate SCE/LME/NXE while preserving derived LMA and rejecting
LME transitions during paging. CPUID admits and advertises SYSCALL support.
STAR/LSTAR/CSTAR/FMASK access uses seL4's existing per-VCPU MSR API, with
canonical 48-bit entry-address and FMASK validation. This adds no host MSR
passthrough. Host tests cover valid transitions, unchanged rejected state and
noncanonical addresses; the target result proves progress past EFER.SCE,
not a userspace syscall or per-guest syscall-state isolation proof.

The temporary first-post-transfer wait snapshot at `d835ca4` identified TLS
driver initialization, since QemuKernelLoaderFsDxe fetches blobs before BDS
loads Linux. That diagnostic selection was removed; normal bounded wait
snapshots remain. No exit limit or test success condition was relaxed.

The [clock and LAPIC receipt](evidence/2026-09-17-spark/ovmf-linux-clock.json)
records further progress at `7f61656`: architectural clock discovery passes
the initial PIT calibration, absent-PIC probes select no writable legacy
controller, and Linux proceeds through local APIC setup. The next rejected
operation is still a PIT request, now programming channel 0 in periodic mode
(`0x34` to port `0x43`), not calibration or shutdown. Do not discard it as an
unused-device write. Linux's `apic_needs_pit` also requires configured
interrupt topology and the always-running APIC-timer declaration; generated
ACPI and the complete guest timer contract are the next integration step.
The 32-vCPU-capable ACPI serializers in PR #178 remain separate and host-only.

`make test-x86-config-host test-x86-string-host` checks exact blob sizes and
bytes, data beyond the old 80-byte stream boundary, reselect/EOF behavior,
per-guest isolation, invalid descriptor rollback, bounded cross-page transfers,
and unchanged source/destination state on a rejected transfer. Sanitized host
runs passed for the initial implementation. A wrong kernel hash was also
rejected before object generation. Full Spark `make gate` passed at `8895952`;
that covers ordinary architecture boots and guest I/O, not the Intel EFI path.
