# CR2 dependency candidate

The user approved this scoped kernel exception and full requalification on
2026-09-19. Default SDK pins and installed release SDKs remain unchanged.
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
`ec86afdcd662b5976d11d4994acf1b11a2979882` and the patched seL4 tree outside
the agentOS repository. The retained build used GNU GCC 13.3.0, binutils 2.42,
Python 3.12.3 and `sel4-deps==0.9.0`. The dependency freeze and complete build
logs are retained with the qualification evidence.

Use the opt-in Make target with local upstream clones and an external Python
environment containing the pinned dependencies. The example uses absolute
paths; the output directory must be fresh and outside the agentOS checkout.
GNU build tools and the `aarch64-linux-gnu` and `x86_64-linux-gnu` GCC 13.3
cross toolchains must be on `PATH`.

```sh
make sdk-candidate \
  SDK_CANDIDATE_MICROKIT_SOURCE=/path/to/upstream/microkit \
  SDK_CANDIDATE_SEL4_SOURCE=/path/to/upstream/sel4 \
  SDK_CANDIDATE_PYTHON=/path/to/sel4-venv/bin/python \
  SDK_CANDIDATE_DIR=/path/to/fresh-external-build
```

The target creates private clones at the pinned revisions, applies only the
recorded patch, and builds all three release boards. It verifies their kernel
hashes against `tools/sdk/cr2-kernels.sha256` and prints the candidate
`SEL4_SDK` path. It neither replaces an installed SDK nor changes default
pins. A differing kernel hash is a failed reproduction, not a new accepted
candidate. Existing output directories are preserved and rejected.

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

Patched managed one-CPU Debian qualification, both SMP generations with the
updated harness, final integrated revision gates,
required CI/review and the rest of v0.4 remain outstanding. Do not infer crash
causation or SMP acceptance from this patch, its build or the shorter gates.
