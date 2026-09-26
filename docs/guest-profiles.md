# Guest profiles

Guest identity is host data, not a target-code branch. Source profiles live in
`guest-profiles/` and compile to a fixed 640-byte manifest consumed by the VMM
PD. Target code recognizes architecture, boot protocol, image format, bounded
memory windows, and canonical device classes. It does not interpret TOML or run
profile recipes.

The source format has three states:

- `abstract` supplies inherited defaults and cannot enter a target image;
- `planned` describes a roadmap guest but cannot enter a target image;
- `runtime` requires pinned kernel, DTB, and optional initrd SHA-256 identities.
  UEFI profiles may omit the DTB; its manifest address, size, and hash are then
  zero. Placement DTB hashes and nonzero addresses are rejected when the
  artifact is absent. FDT boot continues to require a DTB. The existing build
  bundle adapter accepts only `fdt-direct`; a valid UEFI manifest does not yet
  imply that this adapter or the target VMM can execute the profile.

`extends` is a root-relative profile path. The Rust compiler rejects absolute
paths, `..`, cycles, inheritance deeper than eight files, unknown fields,
unbounded text and recipes, invalid enum values, address wrap, artifacts outside
guest RAM, and overlapping maximum artifact windows. Arrays replace inherited
arrays; tables merge recursively.

## Host and target boundary

The TOML source may contain finite host recipes for acquisition, build
preparation, provisioning, and tests, plus bounded QEMU metadata. Profile
aliases, machine options, RAM, host-media paths and bus numbers, SSH identity,
console markers, requested VirtIO devices, DTB templates, and proof scope
(`emulated` or `host-backed`) are data. Recipe actions and DTB adapters come
from closed Rust whitelists and never appear in the binary manifest. There is
no target scripting language or interpreter.

The binary manifest contains only:

- schema and profile identity;
- architecture, direct-boot protocol, and kernel format;
- guest ID, vCPU count, lifecycle flags, control type, and canonical devices;
- guest GPA, VMM HVA, RAM size, and bounded artifact placements;
- profile and artifact SHA-256 identities;
- a bounded command line and, when required, a normalized ISO initrd path.

The `fetch-guest` host tool resolves the selected profile and executes only its
bounded `host.acquire` recipe. The interactive QEMU launcher and QEMU test
runner resolve the same profile or scenario and consume `host.qemu`; QA also
consumes `host.test`. Neither path chooses single-guest machine, memory, media,
SSH, console, or VirtIO proof policy by distribution name. Runtime acquisition
supports a closed set of semantic operations (HTTPS staging, archive/ISO
extraction, SHA-512 verification, qcow2-to-raw conversion, bounded GPT
partition extraction, ext4 file extraction/configuration, arm64 image normalization,
deterministic probe-initramfs construction, and confined initramfs file
overlays). Unknown actions and arguments fail closed. The build executor
renders a bounded FDT template, hashes all staged artifacts, and emits a
canonical per-slot bundle containing `kernel.bin`, `guest.dtb`, `initrd.bin`,
and `profile.bin`. `vmm.mk` packages only that bundle and has no
distribution-specific artifact or DTB branches. The VMM validates the fixed
wire representation and checks embedded artifact sizes before the
guest-neutral boot executor copies anything into guest RAM.

For single-guest launchers, `host.qemu.restrict_network` optionally controls
QEMU user-network isolation.
The x86 firmware launcher defaults to restricted networking when it is omitted;
Arch and Debian explicitly set it to `false` for DNS, package acquisition and
functional network checks. SSH forwarding remains bound to host loopback.
This host-emulator setting does not grant device capabilities to a guest.

For `build-initramfs-file` and `append-initramfs-file`, specify exactly one of
inline UTF-8 `content` or `content_file`. A file payload is relative to the
profile's acquisition output directory and requires `content_sha256` (64 hex
digits). The executor reads at most 16 MiB, verifies the bytes before changing
the destination, and preserves arbitrary binary data, including native ELF
helpers. `path` remains the relative path inside the CPIO archive; `mode` is
octal and at most `0777`. Both payload forms support `compression = "none"`
or `"zstd"`. Existing text overlays do not need a file checksum.

`build-static-linux-elf` compiles a repository-relative C `source` to an
acquisition-directory-relative `output`, with `architecture = "x86_64"` or
`"aarch64"`. The executor uses fixed freestanding static Clang/LLD flags and
strips build metadata with llvm-objcopy; recipes cannot supply compiler flags
or shell commands. Subsequent binary overlay steps pin the resulting bytes.

`make fetch-guest GUEST_PROFILE=debian-amd64.toml` acquires the pinned Debian
13 amd64 cloud image, extracts its kernel and initrd, preserves its stock udev
hooks, and appends native hooks and virtio module configuration. Its
`uefi-artifacts` build adapter provides an acquisition directory without FDT
template fields. The FDT bundle executor still rejects UEFI profiles.
`make gate-x86_64-linux-login X86_BOOT_PROFILE=debian-amd64.toml` selects,
acquires and verifies this profile's kernel and initrd, emits its NUL-terminated
command line, and applies its RAM budget. Supply the independently pinned
`X86_FIRMWARE_IMAGE`/`X86_FIRMWARE_SHA256` and a disposable `X86_ROOT_DISK` as
usual; separate `X86_BOOT_KERNEL`, initrd, command-line and RAM overrides
conflict with profile selection. The selector accepts only the currently
supported single primary guest, one vCPU, fixed VMM RAM mapping, canonical
net/block/console devices and no requested CPU-feature policy. The emitted
`build/tmp/x86-boot-profile/profile.bin` is embedded read-only in the x86 VMM
after a build-time hash check. Before publishing boot blobs through fw_cfg,
the VMM validates the manifest, compares its guest/device/RAM policy with the
provisioned configuration, matches the complete command line, and recomputes
kernel and initrd SHA-256 digests. Unsupported CPU-feature requests and
artifact/resource mismatches stop the boot. UEFI still chooses image placement
and entry; manifest artifact windows are resource bounds, not instructions to
the EFI loader. This binding does not authenticate a release or replace secure
boot. SSH provisioning remains separate integration work. The generated `disk.raw` is
source media; use a disposable copy for writable boot tests.
`install-gpt-ext4-file` installs one root-owned 0644 configuration file
(1–65536 bytes) into an ext4 partition of a private GPT disk copy. Its arguments
are `source`, `output`, `index`, `path`, `content`, and optional `sector_size`
(512 or 4096). The Rust executor verifies the installed bytes before publishing
the output and leaves the base disk unchanged. A matching cached recipe keeps
the existing writable output, including guest changes. A different recipe or
base image requires a new output path; preparation refuses to replace an
existing configured disk. The host needs e2fsprogs `debugfs`.

The Debian profile uses this operation to install an SSH service drop-in. Since
cloud-init is disabled, the guest runs its native `ssh-keygen -A` before the
normal `sshd -t` check. Existing keys are retained; no private key is baked into
the acquired image, and authentication policy is unchanged. A second configuration
file gives the already-enabled networkd service a `virtio_net` driver match and
IPv4 DHCP. The initramfs leaves the image's enabled systemd-resolved service
available so its existing stub resolver link can use DHCP-provided DNS.
The `.networked.raw` disk is the writable boot medium;
`.configured.raw` is the SSH-configured intermediate, and the dated `.raw` disk
remains the base for artifact extraction. No interface name or static guest IP
is selected by the network configuration. Test-harness SSH account provisioning
remains a separate operation.

Legacy `--guest-os` and `GUEST_OS` spellings remain compatibility selectors.
For a single guest, the value is resolved through the profile's `aliases`
array. Multi-guest tests resolve a separate bounded document under
`guest-scenarios/`. A scenario names ordered profiles and supplies the board,
QEMU machine and memory, per-profile RAM, host SSH forwarding, and guest
addresses. It coordinates configured profile slots as one lifecycle test; it
does not select different VMM implementations.

Console automation is a bounded declarative state machine under
`host.console`, not a profile- or distribution-named code path. The only
adapter is `expect`. A profile declares success markers, markers that must
also be present, immediate rejection markers, and ordered interaction rules.
Each interaction requires all of its `when` markers (or a bounded
`after_secs` delay), sends one bounded byte string, and has an explicit retry
ceiling. An optional command/marker pair proves that the reached prompt
accepts input; its timeout is bounded too. The compiler limits marker sizes,
rule counts, delays, retries, and probe sizes, and rejects incomplete or
unknown fields. Provisioning commands remain `send-console` recipe steps and
permit only the bounded `{{ssh_public_key}}` substitution. Unknown adapters,
recipe actions, arguments, and template variables fail closed.

Optional desktop qualification is profile data under `host.desktop`. The
closed `rfb-over-ssh` adapter consumes the profile's SSH account and port plus
a bounded local RFB port, exactly one guest TCP port or absolute Unix-socket
path, provisioning and frame deadlines, I/O timeout, and a size-limited guest
provisioning script. The runner provides only the
generic authenticated SSH tunnel and raw-frame verifier. A future Wayland
profile can supply a different recipe without adding a distribution branch to
the runner.

Manifest version 2 can set `boot.media_initrd_path`. The shared block backend
then walks that normalized relative path through ISO9660 and stages the file at
the profile's initrd address. No distribution name or fixed ISO pathname is
compiled into the VMM. Profiles that append a packaged overlay to that media
initrd pair `host.build.initrd_total_bytes` with a confined
`host.build.media_initrd_cache` path. Bundle preparation verifies the cached
media initrd plus overlay byte count exactly, emits the checked total as build
metadata, and the guest-neutral VMM rejects a different runtime media size.

Lifecycle RPC, device selection, boot preparation, the seL4 receive loop, and
the AArch64 VMM itself are shared target components. Primary and secondary
instances compile the same `guest_vmm.c`; a separate FreeBSD implementation no
longer exists. The `control_type` value is data used only to match a create
request to a profile; it does not select target code.

## Commands

Use Make for the normal check:

```text
make guest-profile-check
```

The lower-level compiler interface is:

```text
cargo xtask guest-profile --check-all
cargo xtask guest-profile --profile buildroot.toml \
  --placement default --output build/tmp/buildroot.bin
cargo xtask fetch-guest --profile ubuntu-e2e.toml \
  --output-dir build/guest-images
cargo xtask guest-profile --profile ubuntu-e2e.toml \
  --placement default --prepare-dir build/tmp/ubuntu-bundle
```

The release dual-guest alias is data in
`guest-scenarios/dual-release.toml`; `--guest-os both` is retained as its
compatibility spelling.

Interactive launch uses the same interpreter and attachment path:

```text
make run GUEST_PROFILE=ubuntu-e2e.toml
make run GUEST_SCENARIO=both
make run-fast GUEST_SCENARIO=both
```

Launch and qualification builds derive `GUEST_GRAPHICS` and `GUEST_INPUT`
from the selected profiles' device lists, including every guest in a scenario.
For example, `make run GUEST_PROFILE=debian-input.toml` builds the input
virtualizer and guest backends automatically. Absent capabilities are passed
as empty values so inherited environment settings cannot add an unrequested backend.
Direct `make build` still requires the corresponding optional build flags.

The canonical Make selector is `GUEST_PROFILE` for one profile,
`GUEST_SCENARIO` for a data-defined composition, or
`GUEST_PRIMARY_PROFILE` plus `GUEST_SECONDARY_PROFILE` for explicit slot
composition. The profile's `target.control_type` selects its configured slot,
and its selected placement derives the slot RAM capacity. `GUEST_OS` is a
compatibility spelling resolved through the profile's data-defined aliases and
scenario aliases and is not passed into the root-task or VMM build.

Adding a profile does not add a VMM personality. Buildroot and Ubuntu extend
the AArch64 Linux Image profile; Debian is the pinned stable integration
profile under qualification; Omarchy extends the planned Arch profile and
selects the future x86-64 UEFI machine. FreeBSD selects the raw AArch64
direct-boot format. Those are data choices over the same bounded boot and
canonical VirtIO contracts.
