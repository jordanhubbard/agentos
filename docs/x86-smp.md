# x86 two-CPU qualification

The firmware composition provisions two private native runners and VCPUs.
`debian-amd64-2cpu.toml` selects both, with 2 GiB of guest RAM. The default
Debian profiles still select one CPU. Shared device handling is serialized
between bounded VM entries; this is virtual SMP, not a claim of simultaneous
execution on two host cores.

Run the qualification on an Intel Linux host with working nested VMX:
build the approved [isolated CR2 candidate](x86-cr2-candidate.md) first.
The unmodified 2.3.0 SDK has not passed this SMP gate.

```sh
make gate-x86_64-smp \
  SEL4_SDK=/path/to/microkit-sdk-2.3.1-agentos-e60776ac-cr2 \
  SEL4_SDK_VERSION=2.3.1-agentos-e60776ac-cr2 \
  X86_ROOT_DISK=/path/to/disposable-seeded-debian.raw \
  X86_SSH_KEY=/path/to/guest-identity \
  X86_SSH_PORT=12224 \
  X86_SSH_KNOWN_HOSTS=/path/to/pinned-known-hosts \
  X86_FIRMWARE_IMAGE=/path/to/pinned-ovmf.fd \
  X86_FIRMWARE_SHA256=<verified-sha256> QEMU_TEST_TIMEOUT=600
```

The gate selects the two-CPU profile and compiles a static Linux/x86-64
payload from `tests/platform/x86_smp_probe.c` and its assembly entry point.
`make x86-smp-probe` can cross-build that payload on Spark; it does not execute
the qualification there. The payload uses Linux syscalls and needs no extra
guest packages.

After each managed guest reaches login and passes pinned SSH, the harness
streams the payload into a private temporary directory and runs it. It checks
that `/sys/devices/system/cpu/online` reports exactly CPUs 0 and 1. Forked
workers pin themselves to those CPUs, confirm their actual CPU identity,
synchronize through a shared page, and keep distinct x87 and SSE register
values live during a bounded spin. The parent requires overlapping monotonic
time intervals, correct values and successful child exit. The workload has a
60-second alarm; the harness bounds SSH execution separately and does not
retry an ambiguous workload invocation.

The harness retains `.smp.out` and `.smp.err` alongside each generation's
console and SSH artifacts. It then checks console input, destroys the guest,
rejects stale handles and repeats the entire boot/workload sequence after
reconstruction. Compilation, a two-CPU manifest, login alone or a single
generation does not satisfy this gate. The login reader also rejects guest
segfault, general-protection and kernel-oops reports even if a login prompt
follows. The initial two-generation bring-up at `1f5fbe1` passed the earlier
login/SSH checks but reported a `udev-worker` segfault on CPU 1 in the second
generation; that result is not accepted as healthy SMP qualification.
The strict gate at `4a7c8b0` also rejected a CPU-1 userspace segfault, in the
dynamic loader during first-generation boot. The workload was not reached.
The [failure receipt](evidence/2026-09-18-spark/x86-smp-failure.json) records
the executable image and console hashes. Loader, libc and systemd-generator
files match the earlier seeded disk; this does not establish runtime memory
correctness. Repeating the same revision with a journal-recovered copy of
that earlier disk also failed, with a userspace instruction-fetch fault on
CPU 0. The fault is not confined to CPU 1 and reproduces on the baseline
disk. The failure's cause remains unconfirmed.

The approved CR2 candidate passed the strict gate in both managed generations
at `de1458c` on 2026-09-19. The [Intel receipt](evidence/2026-09-19-spark/cr2-intel-smp.json)
binds the SDK, root task, workload, console and lifecycle evidence. This is
acceptance of that candidate run, not default SDK adoption or release
qualification. Earlier unmodified-kernel failures remain valid observations;
their cause is not established by this pass.

The first two-CPU bring-up exposed the private CMOS warm-start marker, which
is now emulated.

### Optional second host disk

The managed gate accepts `X86_SECONDARY_DISK=/absolute/path/to/secondary.raw`.
It rebuilds root with the second PCI block function at 00:08.0 and attaches
that raw disk read-only by default. `X86_SECONDARY_WRITABLE=1` enables writes
to it independently of the primary root disk. The two paths must identify
different regular files, including through symbolic or hard links, and their
sizes must be nonzero multiples of 512 bytes. Use disposable copies for gates
that enable writes.

For example, append these selectors to the SDK, firmware, SSH and root-disk
arguments of `make gate-x86_64-smp`:

```sh
X86_SECONDARY_DISK=/absolute/path/to/secondary.raw X86_SECONDARY_WRITABLE=1
```

The second-disk gate additionally requires the driver to report successful
initialization of both PCI media and their separate queues. This proves host
device initialization only. The current descriptor still starts one guest;
secondary guest I/O, concurrent disk isolation and persistence remain separate
v0.4 requirements. The receipt must not describe this mode as a dual-guest pass.

The first native two-disk run at `1b4094d` failed because this marker was
absent. It uses `log_drain_write`, whose fallback is disabled when the release
SDK lacks `CONFIG_PRINTING`; the x86 composition has no log-drain PD. This
failed run is retained and does not establish second-media acceptance.
Concurrent guest I/O is still required; a missing diagnostic must not be
treated as a passing device check.

## Preparing a secondary coordinator

`make prepare-x86-profile X86_BOOT_PROFILE=debian-amd64-secondary.toml
X86_VMM_SLOT=secondary` verifies the pinned kernel/initrd and emits build
arguments as a JSON array after acquisition diagnostics. These are argument
values, not shell source. Primary and secondary manifests and command lines
are written under separate `build/tmp/x86-boot-profile/<slot>/` directories.
Build each coordinator in a separate build directory.

The selected slot must agree with all four profile identities: guest ID,
control type, network client and block media. The default runtime path still
requires the primary slot. Preparing a secondary manifest does not spawn its
coordinator or runner pair, attach its disk, or qualify concurrent storage.

## Opt-in two-guest image

After preparing both profiles, the existing `make build` interface accepts
`X86_DUAL_GUEST=1 X86_CC_PCI=1 X86_SECONDARY_BLOCK=1 X86_LINUX_LOGIN=1`
with the primary profile's emitted arguments and the usual pinned firmware
and SDK arguments. Supply the secondary manifest using
`X86_SECONDARY_BOOT_PROFILE_BIN` and `X86_SECONDARY_BOOT_PROFILE_SHA256`.
The two profiles must agree on kernel, initrd, command line and RAM size;
the secondary build shares these immutable inputs and verifies its own
manifest against its compiled slot and resources at admission.

The image has two coordinators, each with its own runner pair. The secondary
coordinator builds under `<BUILD_DIR>/secondary`; the primary build's objects
are not reused for its guest adapters. Both guests have public lifecycle
routes through VM manager. The ordinary composition remains one guest.
Use two independently seeded disks, not the small blank disk used solely
for driver initialization qualification. This image build does not prove
concurrent boot, network routing, peer progress during teardown or persistent
storage isolation. Those runtime results remain required for v0.4.

The first two-guest image booted all 13 PDs, but secondary CREATE did not
complete. The native-fault diagnostic build identified secondary service 17
reading `0x2a001004` in the serial startup check. Root maps that guest's serial
page at `0x2a200000`; the coordinator still used the primary base. Startup
and reconstruction now select the serial page by compiled guest slot. The
observed fault is retained; the corrected guest path requires native retesting.
