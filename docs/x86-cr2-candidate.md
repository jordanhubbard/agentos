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

Run the upstream builder for `x86_64_generic_vtx`, configuration `release`,
with candidate version `2.3.1-agentos-e60776ac-cr2`. The target bundle used
by agentOS needs the kernel and generated headers. `--skip-tool` and
`--skip-initialiser` avoid building unused Microkit components. Do not use
`--skip-run-time`: upstream also skips the kernel under that option.

The upstream release configuration leaves verification/debug defaults
dependent on CMake cache history. A second configure can disable VMX. After
the first upstream build creates its board build directory, configure that
directory with `KernelVerificationBuild=OFF`. Then configure it again with
all of the following explicit values before rerunning the upstream builder:

```text
KernelVerificationBuild=OFF
KernelDebugBuild=OFF
KernelPrinting=OFF
KernelIRQReporting=OFF
KernelColourPrinting=OFF
KernelVTX=ON
KernelX86_64VTX64BitGuests=ON
```

The separate configure passes allow upstream's dependent-option cache to
settle before selecting release values. Inspect `gen_config.h` after the
final build: VTX and 64-bit guest support must be enabled, while verification,
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

The strict patched SMP workload is still running. Patched managed one-CPU
Debian qualification, both SMP generations, final integrated revision gates,
required CI/review and the rest of v0.4 remain outstanding. Do not infer crash
causation or SMP acceptance from this patch, its build or the shorter gates.
