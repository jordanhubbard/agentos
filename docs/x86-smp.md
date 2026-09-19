# x86 two-CPU qualification

The firmware composition provisions two private native runners and VCPUs.
`debian-amd64-2cpu.toml` selects both, with 2 GiB of guest RAM. The default
Debian profiles still select one CPU. Shared device handling is serialized
between bounded VM entries; this is virtual SMP, not a claim of simultaneous
execution on two host cores.

Run the qualification on an Intel Linux host with working nested VMX:

```sh
make gate-x86_64-smp SEL4_SDK_VERSION=2.3.0 \
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

Native workload acceptance is pending;
the first two-CPU bring-up exposed the private CMOS warm-start marker, which
is now emulated.
