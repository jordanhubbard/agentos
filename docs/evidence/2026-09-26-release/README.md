# v0.5.0 native qualification

These receipts record Intel Linux/KVM runs with SDK
`2.3.1-agentos-e60776ac-cr2`. Source revisions, working-tree patches where
applicable, artifact hashes, and archive locations appear in each receipt.
The work was initially prepared as v0.4.2 and shipped in v0.5.0. This index
and the additional graphics/input and desktop receipts were prepared for
0.5.1 as historical documentation; they are not tests of a 0.5.1 revision.

| Evidence | Result and scope |
| --- | --- |
| [NIC scheduling](nic-scheduling.json) | Passed two Debian two-vCPU generations, functional SSH, overlapping CPU-affined x87/SSE work, and managed recreation. No throughput or fairness claim. |
| [Arch installation](arch-install.json) | Passed installation onto a fresh 8 GiB disk and a fresh platform boot to functional SSH. Uses the pinned external kernel/initramfs. The initial harness's later stale-lock finding is preserved. |
| [Arch package persistence](arch-package-sync.json) | Passed two cold boots with the final-sync fix, including package install/execute/remove on the same disk without repair between runs. |
| [Arch graphics/input](arch-graphics-input.json) | Passed exact framebuffer pixels and five Linux evdev input modes through the public CC path. Separate from compositor/RFB acceptance. |
| [Arch desktop](arch-desktop.txt) | Passed the native desktop gate at `28da7ff6`, including functional SSH, framebuffer/input checks, Sway readiness and a non-empty 1024x768 WayVNC RFB capture. |

The desktop receipt is copied verbatim from the retained archive's
`receipt.txt`. The archive is on `bullwinkle` at
`/home/jkh/.local/share/agentos-evidence/2026-09-26-arch/desktop-pass-28da7ff6.tar.gz`,
with SHA-256 `1bf2287b690a4b92064f5654748adc3b51d2258621d5feb256085f5acd007155`.
It retains the profile, guest-session transcripts and runtime evidence.
The working disk was recovered offline after interrupted diagnostics;
the original base and failed images were preserved. The graphics/input
receipt separately retains the failure archives for the configuration-write
and input-notification fixes.

The desktop source was squash-integrated by PR #284. The published v0.5.0
tag is `1c3ae4aff713f971ea214132be2fc70e569d7795`; its separate
[release receipt](https://github.com/jordanhubbard/agentos/releases/download/v0.5.0/receipt.json)
records the release gates. None of these tests establishes physical
input/display hardware compatibility, installed-bootloader support, or guest
kernel-update support, and there is no Omarchy qualification claim.

Archives exclude private SSH identities and installed disks. Generated local
evidence under `_build` is disposable and is removed by `make clean`; the
receipts identify the separate retained archives.
