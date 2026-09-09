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
- guest ID, vCPU count, lifecycle flags, and canonical devices;
- guest GPA, VMM HVA, RAM size, and bounded artifact placements;
- profile and artifact SHA-256 identities;
- a bounded command line.

The build hashes the staged artifacts before embedding the manifest. The VMM
then validates the fixed wire representation and checks embedded artifact sizes
before the guest-neutral boot executor copies anything into guest RAM.

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
```

Adding a profile does not add a VMM personality. Buildroot and Ubuntu extend
the AArch64 Linux Image profile; Debian is the planned stable integration
profile; Omarchy extends the planned Arch profile and selects the future x86-64
UEFI machine. FreeBSD selects the raw AArch64 direct-boot format. Those are data
choices over the same bounded boot and canonical VirtIO contracts.

