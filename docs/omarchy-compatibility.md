# Omarchy Compatibility Ledger

This ledger is the upstream compatibility input to the agentOS Omarchy
roadmap. It records facts about an official artifact separately from agentOS
implementation choices and unproven expectations. An entry here is not a
support claim. `docs/ROADMAP.md` defines the gates for such a claim.

Current release observation: **2026-09-16**, upstream **Omarchy v4.0.4**.
The detailed compatibility baseline below remains the explicitly dated v4.0.3
study; this update records newly verified facts without implying that the new
ISO has been installed or qualified on agentOS.

The upstream release listing was rechecked on **2026-09-19** and still names
v4.0.4 as latest. The ISO metadata observations below remain dated 2026-09-16;
this release-list check did not download or requalify the media.

## Current official release observation

| Field | Observed value | Evidence |
| --- | --- | --- |
| Release | `v4.0.4`, published 2026-09-15 | [upstream release](https://github.com/omacom/omarchy/releases/tag/v4.0.4) |
| Runtime tag commit | `c668141e9c42b13c80c9ca4ea108e11708c5e8a5` | [tagged commit](https://github.com/omacom/omarchy/commit/c668141e9c42b13c80c9ca4ea108e11708c5e8a5) |
| Installation media | `https://iso.omarchy.org/omarchy-4.0.4.iso` | upstream release |
| Published ISO SHA-256 | `ddeded2758c48318d201dfdac905ecb28f570441883f0c052ea3cd5d05acf92d` | release and [matching sidecar](https://iso.omarchy.org/omarchy-4.0.4.iso.sha256) |
| ISO size | `6,185,304,064` bytes | HTTP Content-Length, observed 2026-09-16 |
| ISO source observed | `7cfb7111a06873d61c45d37034577d4ba08d3f4f` on `quattro` | [observed source](https://github.com/omacom/omarchy-iso/commit/7cfb7111a06873d61c45d37034577d4ba08d3f4f) |
| ISO architecture and firmware | `x86_64`; BIOS and UEFI boot modes | [source profile](https://github.com/omacom/omarchy-iso/blob/7cfb7111a06873d61c45d37034577d4ba08d3f4f/configs/profiledef.sh) |

The release changes the default kernel to `linux-omarchy`. Future qualification
must retain the actual installed kernel identity and configuration; the prior
kernel's device support cannot be assumed. The [v4.0.4 installation manual](https://github.com/omacom/omarchy/blob/v4.0.4/manual/02-getting-started.md)
still describes ISO installation, encryption by default and unattended
configuration on a second drive.

The checksum above is the published digest, cross-checked against its sidecar;
this observation did not download or hash the complete ISO. The source commit
is an observation of the ISO repository, not a provenance binding to the
published binary. No new AArch64 support or agentOS guest qualification is
established by these facts. The v0.4 VMX, UEFI/ACPI, graphics and input work
remains required before the v0.6 official-Omarchy gate.

## Previous pinned artifact (2026-09-08)

| Field | Pinned value | Evidence |
| --- | --- | --- |
| Release | `v4.0.3`, published 2026-09-08 | [upstream release](https://github.com/omacom/omarchy/releases/tag/v4.0.3) |
| Runtime tag commit | `0534987009061cbe2dacdde4ad564092ab698d12` | [tagged commit](https://github.com/omacom/omarchy/commit/0534987009061cbe2dacdde4ad564092ab698d12) |
| Installation media | `https://iso.omarchy.org/omarchy-4.0.3.iso` | release body and [ISO project](https://github.com/omacom/omarchy-iso) |
| ISO SHA-256 | `03d60bc74306dca51f96e1a84b690871d8d606826b260edd0208962da8507d14` | release body and adjacent `.sha256` sidecar |
| ISO size | `6,260,654,080` bytes | HTTP object metadata |
| ISO source observed | `a23f8d464dcb0616a61bfaa8026e23d0533da209` on `quattro` | [observed ISO commit](https://github.com/omacom/omarchy-iso/commit/a23f8d464dcb0616a61bfaa8026e23d0533da209) |

The upstream release receipt does not bind the published ISO to an exact
`omarchy-iso` source commit. The observed ISO commit is therefore research
context, not a claimed build provenance link. agentOS must retain the ISO
bytes and verify the published digest; it must not rebuild an artifact and
call that the official image.

## Compatibility baseline (v4.0.3, observed 2026-09-08)

| Area | Official v4.0.3 fact | Consequence for agentOS |
| --- | --- | --- |
| Architecture | The ISO profile sets `arch="x86_64"`. The stable Arch and Omarchy repositories serve `x86_64`; their corresponding `aarch64` database URLs returned 404 at this snapshot. | Official qualification waits for the 0.4 x86 VMM. Community AArch64 ports do not satisfy it. |
| Firmware | The ISO declares BIOS and UEFI boot modes. Upstream's VM example uses Q35 plus OVMF and no pre-enrolled keys. | The agentOS qualification profile deliberately uses its canonical UEFI/ACPI path. BIOS support is not required for the claim. |
| Installation | The ISO is the only upstream-supported installation path. It contains an offline package mirror and supports a second `cidata` drive for unattended installation. | Model the ISO and `cidata` as read-only block devices and the destination as a distinct persistent writable device. Prefer unattended installation for repeatable gates. |
| Storage | Full-disk and free-space installs are supported. Encryption is the default. Current installs use LUKS, Btrfs subvolumes, a swap file, snapshots, and an EFI system partition. | Virtio block must correctly implement flush, discard policy, persistence, reboot, and multiple devices before the installer gate. No host filesystem shortcuts. |
| Network | NetworkManager with DHCP is enabled. An unattended config containing `authorized_keys` enables SSH and opens its firewall rule. | Canonical virtio-net can provide the install and post-boot path. The support gate remains key-only SSH even though upstream leaves password SSH at the distribution default. |
| Desktop | Omarchy installs Hyprland, Quickshell, SDDM, UWSM, `xdg-desktop-portal-hyprland`, PipeWire/WirePlumber tools, and GPU-sensitive applications. Upstream installs Vulkan drivers after detecting a supported PCI GPU vendor. | Wayland stays entirely inside the hostile guest. agentOS must provide standards-visible DRM/KMS graphics and evdev input through virtio-gpu and virtio-input; it does not implement the Wayland protocol. Quickshell and representative GL/Vulkan clients are part of the eventual proof. |
| Graphics | Upstream's VM example uses virtio VGA. Its acceptance harness validates the real SDDM and graphical session with QMP screenshots and virtual keystrokes. | Start with virtio-gpu 2D scanout, but do not assume that scanout alone qualifies the full desktop. Record the renderer and acceleration mode, exercise buffer sharing, and require a non-uniform captured frame. |
| Input | The installer, encryption prompt, SDDM, Hyprland, and desktop acceptance all require keyboard input; normal desktop qualification also requires a pointer. | Provide generic virtio-input keyboard and pointer devices. Console injection is not substitute evidence. |
| CPU and RAM | Upstream publishes a Proxmox example with host CPU, 4 vCPUs, 8 GiB RAM, and a 40 GiB disk, but does not label those values as universal minimums. | Use these as the initial test profile, not a support minimum. Fail closed if the requested x86 profile cannot be allocated, then measure and document a qualified floor. |
| Secure boot and TPM | The current getting-started instructions require Secure Boot and TPM to be disabled for installation. | Do not expose either as enabled in the first qualification profile. Future support is a distinct claim. |
| Updates | Stable installations use Omarchy packages, a stable Omarchy Arch mirror, and the stable channel. | Pin the repository state used by the install/update test, retain before/after package identity, inject a failed update, and prove snapshot recovery. |

Primary upstream references for this snapshot:

- [Omarchy v4.0.3 release](https://github.com/omacom/omarchy/releases/tag/v4.0.3)
- [Omarchy ISO README at the observed source revision](https://github.com/omacom/omarchy-iso/blob/a23f8d464dcb0616a61bfaa8026e23d0533da209/README.md)
- [x86_64 ISO profile at the observed source revision](https://github.com/omacom/omarchy-iso/blob/a23f8d464dcb0616a61bfaa8026e23d0533da209/configs/profiledef.sh)
- [Omarchy getting-started manual at v4.0.3](https://github.com/omacom/omarchy/blob/v4.0.3/manual/02-getting-started.md)
- [Omarchy package set at v4.0.3](https://github.com/omacom/omarchy/blob/v4.0.3/install/omarchy-base.packages)
- [Omarchy GPU detection at v4.0.3](https://github.com/omacom/omarchy/blob/v4.0.3/install/hardware/vulkan.sh)
- [Omarchy update-channel manual at v4.0.3](https://github.com/omacom/omarchy/blob/v4.0.3/manual/30-updates.md)

## agentOS delivery sequence

The smallest truthful path is:

1. Keep this ledger current before each Omarchy-related milestone. A changed
   architecture, repository, installer, desktop stack, or artifact digest is
   a gate input, not background churn.
2. Complete generic framebuffer, virtio-gpu, and virtio-input work on the
   existing AArch64 guest. This validates the device model without claiming
   Omarchy support.
3. Boot a minimal x86_64 Linux guest through the data-driven VMX, UEFI, ACPI,
   interrupt, GPA, and canonical-device paths.
4. Install pinned vanilla Arch to persistent storage and qualify SSH,
   DRM/KMS, input, and a Hyprland-class compositor.
5. Attach the official Omarchy ISO plus generated `cidata`, install to an
   isolated writable disk, reboot, and prove key-only SSH.
6. Prove the unmodified Omarchy SDDM, Hyprland, and Quickshell session with
   keyboard and pointer input, a captured non-uniform frame, and representative
   GL/Vulkan clients.
7. Pin and test one update, inject a failed update, and demonstrate recovery
   from retained Btrfs snapshot evidence.

## Boot-time hypothesis and measurement

Omarchy upstream reports faster and potentially sub-minute **installation**;
that is not evidence of faster boot. The expected guest-development advantage
is a hypothesis until agentOS measures it.

Every persistent Arch and Omarchy run must retain monotonic timestamps for:

- vCPU start to firmware entry;
- vCPU start to key-only SSH readiness;
- vCPU start to SDDM readiness; and
- vCPU start to the first non-uniform compositor frame.

Compare cold and warm Omarchy runs with the pinned Ubuntu desktop proof on the
same agentOS revision and resource profile. Do not set a release threshold
until the first measurements show a stable distribution; thereafter, regress
the qualified percentile and record artifact, vCPU, RAM, storage, renderer,
and acceleration mode with every result.

## agentOS implementation evidence, 2026-09-19

The earlier implementation-gap notes predated the following target proofs.
These receipts describe their exact tested revisions, including work still
awaiting canonical integration and required review. They do not qualify the
official Omarchy ISO or turn this branch into a released platform.

| Area | Retained evidence | Remaining acceptance |
| --- | --- | --- |
| x86 execution and firmware | [Two-vCPU Linux recreation](evidence/2026-09-19-spark/intel-current-smp.json) passed pinned SSH, CPU affinity and overlapping x87/SSE workers across two managed boots at `6df47ad`. This supersedes the earlier failed SMP attempt for that exact scope. At `030915a`, the [two-GiB profile](evidence/2026-09-19-spark/intel-current-memory-capacity.json) passed managed recreation with Linux MemTotal measured in both generations, and the [terminal teardown gate](evidence/2026-09-19-spark/intel-current-teardown.json) passed zeroed pool reuse. | Final integrated qualification, full-memory stress and resource-admission boundaries remain required. Reclamation is proven for the configured native test, not every possible profile. These profiles do not qualify Omarchy's illustrative 4-vCPU/8-GiB configuration. |
| Writable storage | At `030915a`, [concurrent same-LBA writes](evidence/2026-09-19-spark/intel-current-same-lba.json) preserved distinct guest patterns, and [independent reboots](evidence/2026-09-19-spark/intel-current-raw-reboots.json) retained both patterns. [Detached snapshot/COW checks](evidence/2026-09-19-spark/intel-current-snapshot.json) and one [interrupted-write recovery](evidence/2026-09-19-spark/intel-current-crash-recovery.json) also passed. | Exhaustive bounds/crash coverage, installer/LUKS/Btrfs use, final integrated qualification and physical power-loss durability remain unproven. Discard is not exposed by the current queue contract; the snapshot proof is offline disk state, not live memory/device state. |
| Guest graphics and input | [Combined ARM qualification](evidence/2026-09-16-spark/graphics-input.json) captured guest-written pixels and checked Linux input events. [Integrated paused-input qualification](evidence/2026-09-19-spark/input-integration-paused.json) retained releases through backpressure. | x86 desktop graphics, abrupt connection-loss recovery, and an interactive Omarchy session remain unqualified. Bulk frame capture is not a responsive remote-desktop claim. |
| Integration guests | [Two Debian cold boots](evidence/2026-09-19-spark/debian-current-cold-boots.json) retained the disk and SSH host identity at `2bb18a0`. [Debian recreation with a FreeBSD peer](evidence/2026-09-19-spark/debian-peer-recreation.json) passed at `cd9efb9`; [Ubuntu peer recreation](evidence/2026-09-19-spark/ubuntu-peer-recreation.json) is separately retained. The Intel storage receipts above exercised two concurrent Debian guests. | All Debian promotion gates and final release acceptance remain required. The [graphics startup timeout](evidence/2026-09-19-spark/graphics-startup-timeout.json) did not reach reconstruction; graphics recreation remains unqualified. Retained boot warnings still need assessment. |

## Remaining official-Omarchy gaps

- No official Omarchy installation or desktop-session acceptance is claimed.
- The minimum renderer needed for unmodified Hyprland plus Quickshell is not
  yet measured. A software-rendered bring-up may accelerate development but
  cannot silently become the final support profile.
- The official release page supplies an ISO digest but not an exact ISO source
  revision or package-manifest digest. Those provenance gaps must be retained
  explicitly in release evidence.
- Upstream contains AArch64 planning and community experiments, but the
  v4.0.3 official image and repositories pinned above are x86_64-only.
