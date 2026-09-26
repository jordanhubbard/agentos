# CR2 dependency candidate

The user approved this scoped kernel exception and full requalification on
2026-09-19. The v0.4 Make and Rust defaults select this bundle through
`tools/sdk/default-version`; CI consumes its verified build artifact.
This is an experimental dependency candidate, not a released seL4 fix or a
claim of upstream acceptance.

The [patch](../tools/sdk/patches/sel4-e60776ac-cr2.patch) applies to upstream
seL4 `e60776acc31097ca063806c257f07a3ec05eacf8`. Its SHA-256 is
`e65c2f6c966a223e5785cc0298f228d3fcdb86ffccf9c66290e1617beec69027`.
It modifies GPL-2.0-only seL4 sources under their existing license.

The patch adds private per-VCPU CR2 storage, initializes it to zero, saves it
in the existing VM-exit context-save path before interrupt handling or
scheduling, and restores it on every VM entry. Restoration is unconditional
even if the VMCS remains current: CR2 is not a VMCS field. The internal field
does not change the public VCPU register API or the assembly register arrays.
Both x86 word sizes have a matching write helper; runtime evidence here is
limited to the x86-64 configuration.

## Build configuration

Use an isolated clone of Microkit
`ec86afdcd662b5976d11d4994acf1b11a2979882` and the patched seL4 tree under
`_build/sdk-candidate`. The retained build used GNU GCC 13.3.0, binutils 2.42,
Python 3.12.3 and `sel4-deps==0.9.0`. The dependency freeze and complete build
logs are retained with the qualification evidence.

Use the opt-in Make target with local upstream clones and an external Python
environment containing the pinned dependencies. The example uses absolute
paths; the output directory must be fresh. The default is `_build/sdk-candidate`;
an explicit external output directory is also supported.
GNU build tools and the `aarch64-linux-gnu` and `x86_64-linux-gnu` GCC 13.3
cross toolchains must be on `PATH`.
`qemu-system-aarch64` and `dtc` must also be available: upstream extracts the
ARM platform device tree during kernel configuration, even for this target-only
SDK build.
On Ubuntu with recommended packages disabled, install `ipxe-qemu` explicitly
as well; the default QEMU network device used by upstream's DTB probe needs
its `efi-virtio.rom`.

```sh
make sdk-candidate \
  SDK_CANDIDATE_MICROKIT_SOURCE=/path/to/upstream/microkit \
  SDK_CANDIDATE_SEL4_SOURCE=/path/to/upstream/sel4 \
  SDK_CANDIDATE_PYTHON=/path/to/sel4-venv/bin/python
```

The target creates private clones at the pinned revisions, applies only the
recorded patch, and builds all three release boards. It verifies their kernel
hashes against `tools/sdk/cr2-kernels.sha256` and prints the candidate
`SEL4_SDK` path. It neither replaces an installed SDK nor changes default
pins. A differing kernel hash is a failed reproduction, not a new accepted
candidate. Existing output directories are preserved and rejected.

An installed candidate can be checked independently with
`make sdk-candidate-check SEL4_SDK=/path/to/candidate`.
`make sdk-check` and the top-level `make build` perform this verification when
`SEL4_SDK_VERSION=2.3.1-agentos-e60776ac-cr2` is selected. It rejects a
different version, any changed or missing pinned kernel, and missing or empty
required public/configuration headers. Header presence is not header-content
authentication. These checks do not replace the runtime gates or adopt the
candidate as the default SDK.

## Local distribution artifacts

`make sdk-candidate-package` accepts `SEL4_SDK`, both upstream source-clone
variables above, and an optional fresh `SDK_CANDIDATE_PACKAGE_DIR` (default
`build/sdk-candidate-package`). It verifies the candidate, then produces:

- `agentos-sdk-targets.tar.gz`, containing the three-board target bundle;
- `microkit-source.tar.gz` and `sel4-source.tar.gz` at the pinned commits,
  retaining their source licenses;
- the CR2 patch, kernel hash manifest, Make recipe, C header normalizer, pinned Python build
  dependencies and this build description;
- `SHA256SUMS` covering all those artifacts.

The source archives contain the upstream bases; apply the included patch to
the extracted seL4 tree before rebuilding. The recipe's normal clone-based
build remains the qualified build path. GNU tar and gzip normalize package
metadata; this does not claim whole-SDK reproducibility across compilers.
The target archive retains the usual `microkit-sdk-<version>` top directory.
It contains each board's kernel, complete include tree and `microkit.ld`, both x86
`sel4_32.elf` Multiboot wrappers, plus VERSION and
licenses. agentOS supplies its own loader and does not link libmicrokit, so
unused Microkit loader/monitor/library binaries and examples are excluded.
The C packaging helper replaces only the absolute source path in the first
comment of the six generated bitfield headers with its repository-relative
path. All declarations and the original installed SDK remain unchanged.
Archive metadata and modes are normalized. This produced identical target
archives from the independent Spark and Intel builds; the pinned SHA-256 is
`fb4290f10c2e59a0baa4d85d477726c3713dec5c497e0d232968bcb6675d566b`.
The hash manifest verifies all three kernels, both Multiboot wrappers and
the three linker scripts. The VMM still uses the SDK's linker script even
though it does not link libmicrokit. Earlier normalized archives omitted
the wrappers or linker scripts and failed the OS gate; neither is accepted
as a release bundle.
It must not be advertised as a complete upstream SDK.
Packaging preserves existing output. CI consumes this artifact through the
reusable SDK build job. Local defaults select the qualified bundle; hosted
acceptance and public release distribution remain pending.

The normal installer can consume the qualified target archive explicitly:

```sh
make sdk SEL4_SDK_VERSION=2.3.1-agentos-e60776ac-cr2 \
  SEL4_SDK=/path/to/new/sdk-directory \
  MICROKIT_SDK_URL=file:///absolute/path/to/agentos-sdk-targets.tar.gz
```

For this candidate version, `make sdk` verifies the pinned archive SHA-256
before extraction, stages extraction separately, and then runs `sdk-check`.
An existing incomplete destination is preserved and rejected. This explicit
URL is needed for prerelease local installation. The default installer URL is
the planned v0.4.0 `agentos-sdk-targets.tar.gz` release asset; it becomes usable
when the evidence-bound release workflow publishes that asset. The bundle
version is not an upstream Microkit release tag.

The `Pinned SDK candidate` workflow rebuilds the three-board bundle from the
exact upstream commits and scoped patch on Ubuntu 24.04 with GCC 13 and the
retained Python dependency pins. It rejects kernel hash differences, packages
sources and licenses with the target bundle, and exercises the checksum-checked
installer before uploading distribution artifacts. Build logs and toolchain
versions are retained even on failure. Main CI and nightly guest qualification
call it once, and all their SDK-consuming jobs depend on its success. A shared
installation action downloads the artifact from that same workflow run and
invokes the checksum-enforced Make installer. Required check names and their
guest-I/O assertions remain intact. The SDK workflow can also run through
manual dispatch. An uploaded workflow artifact is not an OS release, public
download URL, or runtime qualification; hosted execution still needs to pass.

The target bundle used by agentOS needs the kernel and generated headers.
`--skip-tool` and `--skip-initialiser` avoid building unused Microkit components.
Do not use `--skip-run-time`: upstream also skips the kernel under that option.

The upstream release configuration leaves verification/debug defaults
dependent on CMake cache history. A second configure can disable VMX.
The Make target seeds verification, debug and printing settings in a fresh
CMake cache before the first configure, so board selection resolves against
the intended release settings from the outset. The VTX board must produce:

```text
KernelVerificationBuild=OFF
KernelDebugBuild=OFF
KernelPrinting=OFF
KernelIRQReporting=OFF
KernelColourPrinting=OFF
KernelVTX=ON
KernelX86_64VTX64BitGuests=ON
```

Earlier manual builds needed separate configure passes to let upstream's
dependent-option cache settle; the fresh-cache Make path avoids that history.
Inspect `gen_config.h` after the final build: VTX and 64-bit guest support must be enabled, while verification,
debug and printing must be disabled. Reject an unexpected configuration;
successful compilation alone is insufficient.

The unpatched control built with these explicit options has kernel SHA-256
`ded10a09b9820bdd5256085b3a38f89d308d3a0b19316104828efa536e9a4108`,
identical to the unmodified candidate that failed SMP. The patched kernel is
`0f5e9cf9ed7672fb745371b5f2aa0b20df1a49f24e707b88161c9ee4aefc2f9d`.
A clean second-directory build reproduces that kernel and its headers exactly.
Control and patched API headers differ only in generated source-path comments.
No complete Microkit SDK or unused initialiser qualification is claimed.

## Qualification so far

At agentOS `4143e1408b924540a1243612c84af8013a88771d`:

- Unmodified upstream candidate: both managed one-CPU Debian generations pass;
  strict SMP fails on a CPU-0 userspace segfault before the workload runs.
- CR2 candidate: Intel native context/userspace/teardown gate passes, including
  canonical device I/O and reconstruction. Log SHA-256:
  `ddd49d3028cbf5183c70c719965d25c1b017deea6efa9f706d0822b649d2462f`.
- CR2 candidate: full Spark `make gate` passes with the final SDK configuration.
  Log SHA-256:
  `39d2aa228d615e696d140a83f2f685e1a5a0773cc8044a23abbccbaeebe01009`.

The first patched SMP generation passed the overlapping CPU-affined x87/SSE
workload. The replacement guest reached login, but a cloud-init record split
the hostname from ` login:` and the harness timed out. The raw console is
retained; this run is not a complete SMP pass. Prompt recognition now removes
complete numeric-timestamp printk records, while fault checks still inspect
the unchanged raw transcript. Regression tests cover the observed split,
incomplete and malformed records, and a fault interrupting the prompt.

The fresh-cache `make sdk-candidate` build reproduced all three pinned kernel
hashes and passed the full Spark `make gate`. The updated harness passed
`make test-host`, its targeted login tests, and the full Spark gate at
`de1458c244258202aaaa63cd890b1c526202b7e5`. The
[build receipt](evidence/2026-09-19-spark/cr2-sdk-build.json) records the source
pins, build scope and log hashes.

The clean repeat at `de1458c` passed both SMP generations with the updated
harness: pinned SSH, overlapping affined x87/SSE workers, console input,
destruction/recreation and stale-handle rejection. The
[Intel SMP receipt](evidence/2026-09-19-spark/cr2-intel-smp.json) records the
exact binaries and retained evidence. Journald replaced an unclean user
journal; this is not proof of orderly guest reboot or storage durability.

The [patched one-CPU regression](evidence/2026-09-19-spark/cr2-intel-single-cpu.json)
also passed both managed Debian generations at `de1458c`, including pinned
SSH, console input, teardown and stale-handle rejection.

Final integrated revision gates, required CI/review and the rest of v0.4
remain outstanding. Do not infer crash causation from the candidate passes
or broaden their acceptance beyond the recorded workloads and lifecycle.
