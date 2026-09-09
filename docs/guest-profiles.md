# Guest profiles

Guest identity is host data, not a target-code branch. Source profiles live in
`guest-profiles/` and compile to a fixed 576-byte manifest consumed by the VMM
PD. Target code recognizes architecture, boot protocol, image format, bounded
memory windows, and canonical device classes. It does not interpret TOML or run
profile recipes.

The source format has three states:

- `abstract` supplies inherited defaults and cannot enter a target image;
- `planned` describes a roadmap guest but cannot enter a target image;
- `runtime` requires pinned kernel, DTB, and optional initrd SHA-256 identities.

`extends` is a root-relative profile path. The Rust compiler rejects absolute
paths, `..`, cycles, inheritance deeper than eight files, unknown fields,
unbounded text and recipes, invalid enum values, address wrap, artifacts outside
guest RAM, and overlapping maximum artifact windows. Arrays replace inherited
arrays; tables merge recursively.

## Host and target boundary

The TOML source may contain finite host recipes for acquisition, provisioning,
and tests. Recipe actions come from a closed Rust whitelist and never appear in
the binary manifest. There is no target scripting language or interpreter.

The binary manifest contains only:

- schema and profile identity;
- architecture, direct-boot protocol, and kernel format;
- guest ID, vCPU count, lifecycle flags, control type, and canonical devices;
- guest GPA, VMM HVA, RAM size, and bounded artifact placements;
- profile and artifact SHA-256 identities;
- a bounded command line.

The `fetch-guest` host tool resolves the selected profile and executes only its
bounded `host.acquire` recipe. Its implementation contains no distribution
enum, URL, filename, or guest-name dispatch. Runtime acquisition supports a
closed set of semantic operations (HTTPS staging, archive/ISO extraction,
arm64 image normalization, and deterministic probe-initramfs construction).
Unknown actions and arguments fail closed. The build then hashes staged
artifacts before embedding the manifest. The VMM validates the fixed wire
representation and checks embedded artifact sizes before the guest-neutral boot
executor copies anything into guest RAM.

Lifecycle RPC, device selection, boot preparation, and the seL4 receive loop
are shared target components. The `control_type` value is data used only to
match a create request to a profile; it does not select target code.

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
```

Adding a profile does not add a VMM personality. Buildroot and Ubuntu extend
the AArch64 Linux Image profile; Debian is the planned stable integration
profile; Omarchy extends the planned Arch profile and selects the future x86-64
UEFI machine. FreeBSD selects the raw AArch64 direct-boot format. Those are data
choices over the same bounded boot and canonical VirtIO contracts.
