// cmd_gen_pd_bundle.rs — agentOS PD-bundle generator
//
// Produces a compact binary blob containing the PD entry table and all PD ELFs.
// This blob is injected into root_task.elf's `.pd_bundle` section the same way
// `.pd_bundle` already is (objcopy --input-target binary -> relocatable .o ->
// linked into root_task.elf; see kernel/agentos-root-task/Makefile).
//
// The root task reads PD ELFs from this embedded bundle at boot instead of
// walking the seL4 extra-BootInfo region (which seL4 uses only for DTB data,
// not our custom PD payloads).
//
// Bundle format — a strict subset of the agentos.img format:
//
//   [0..64)           agentos_img_hdr_t  (magic + num_pds + offsets)
//   [64..64+N*64)     agentos_pd_entry_t table  (N = num_pds)
//   [pd_elf_data..)   PD ELF blobs, concatenated
//
// kernel_off and root_off are both set to 0 (the bundle has no kernel or
// root task ELF; those are in the main agentos.img loaded by the loader).
//
// Alongside the bundle, this command ALWAYS emits a signed boot manifest
// (T3 image verification, task 2): one entry per PD with its bare stem name
// and the SHA-256 of exactly the bytes at [elf_off, elf_off+elf_len) in the
// bundle just written — i.e. exactly what the root task will hash, from the
// exact same `pd_elfs[i]` bytes placed at `pd_elf_offsets[i]` above. The
// manifest is signed with Ed25519 (see boot_manifest.rs for key handling)
// and a matching public-key C header is emitted for the root task to
// compile in and verify against. Neither output is optional or gated by a
// flag: this command cannot produce a bundle without also producing its
// signed manifest.

use std::fs;
use std::io::Write;
use std::path::PathBuf;

use anyhow::{Context, Result};

use crate::boot_manifest::{
    build_signed_manifest, render_pubkey_header, select_anchor, AnchorTier, ManifestEntry,
};

// Re-use the same SystemDesc / PdDesc types from cmd_gen_image
use crate::cmd_gen_image::{SystemDesc, HEADER_SIZE, IMAGE_MAGIC, IMAGE_VERSION, PD_ENTRY_SIZE};

// ─── CLI args ────────────────────────────────────────────────────────────────

#[derive(clap::Args)]
pub struct GenPdBundleArgs {
    /// Path to the system description TOML (same one used by gen-image)
    #[arg(long)]
    pub system: PathBuf,

    /// Directory containing <pd_name>.elf files for each PD in the system TOML
    #[arg(long, name = "pd-dir")]
    pub pd_dir: PathBuf,

    /// Output bundle path
    #[arg(long)]
    pub out: PathBuf,

    /// Output path for the signed boot manifest (header + entries + Ed25519
    /// signature). Defaults to `<out>.manifest` next to the bundle — always
    /// written; there is no flag to suppress it.
    #[arg(long = "manifest-out")]
    pub manifest_out: Option<PathBuf>,

    /// Output path for the generated C header declaring the Ed25519 public
    /// key the root task compiles in to verify the manifest signature.
    /// Defaults to `boot_manifest_pubkey.h` next to `--out`.
    #[arg(long = "pubkey-header-out")]
    pub pubkey_header_out: Option<PathBuf>,
}

// ─── run ─────────────────────────────────────────────────────────────────────

pub fn run(args: &GenPdBundleArgs) -> Result<()> {
    // 1. Parse system TOML
    let toml_text = fs::read_to_string(&args.system)
        .with_context(|| format!("failed to read system TOML: {}", args.system.display()))?;
    let desc: SystemDesc = toml::from_str(&toml_text)
        .with_context(|| format!("failed to parse system TOML: {}", args.system.display()))?;
    let pds = &desc.pd;

    // The bundle's name[48] field is NUL-terminated (bundle_name_match() in
    // main.c strlen()-compares it), so it can hold at most 47 name bytes
    // plus the forced trailing NUL — unlike the boot manifest's name[48]
    // field, which has no NUL requirement and can hold a full 48 bytes (see
    // aos_boot_manifest_find()). A PD name of exactly 48 bytes would
    // therefore silently truncate to 47 in the bundle while the manifest
    // entry kept the full 48, and the two would never match at boot. Reject
    // such a name at build time instead of letting it mismatch at boot.
    const BUNDLE_NAME_MAX: usize = 47;
    for pd in pds {
        if pd.name.as_bytes().len() > BUNDLE_NAME_MAX {
            anyhow::bail!(
                "PD name '{}' is {} bytes, exceeds the {BUNDLE_NAME_MAX}-byte limit \
                 the bundle's NUL-terminated name field allows (the boot manifest's \
                 name field is 48 bytes with no NUL requirement, one byte more, so a \
                 longer name would silently mismatch between bundle and manifest at boot)",
                pd.name,
                pd.name.as_bytes().len()
            );
        }
    }

    // 2. Read PD ELF bytes
    let mut pd_elfs: Vec<Vec<u8>> = Vec::with_capacity(pds.len());
    for pd in pds {
        let elf_path = args.pd_dir.join(format!("{}.elf", pd.name));
        let bytes = fs::read(&elf_path)
            .with_context(|| format!("failed to read PD ELF: {}", elf_path.display()))?;
        pd_elfs.push(bytes);
    }

    // 3. Compute offsets
    //
    //   [0..HEADER_SIZE)           agentos_img_hdr_t
    //   [HEADER_SIZE..pd_data_off) PD entry table  (N × PD_ENTRY_SIZE)
    //   [pd_data_off..)            PD ELF blobs

    let num_pds = pds.len() as u32;
    let pd_table_off = HEADER_SIZE as u32;
    let pd_table_size = num_pds * PD_ENTRY_SIZE as u32;
    let pd_data_start = pd_table_off + pd_table_size;

    // Compute each PD ELF's offset within the bundle
    let mut pd_elf_offsets: Vec<u32> = Vec::with_capacity(pds.len());
    let mut running_off = pd_data_start;
    for elf in &pd_elfs {
        pd_elf_offsets.push(running_off);
        running_off += elf.len() as u32;
    }

    let total_size = running_off as usize;

    // 4. Assemble bundle in memory
    let mut bundle: Vec<u8> = Vec::with_capacity(total_size);

    // ── Header (64 bytes) ────────────────────────────────────────────────────
    // kernel_off = 0, kernel_len = 0  (no kernel in bundle)
    // root_off   = 0, root_len   = 0  (no root_task in bundle)
    bundle.extend_from_slice(&IMAGE_MAGIC.to_le_bytes()); // [0..8]
    bundle.extend_from_slice(&IMAGE_VERSION.to_le_bytes()); // [8..12]
    bundle.extend_from_slice(&num_pds.to_le_bytes()); // [12..16]
    bundle.extend_from_slice(&0u32.to_le_bytes()); // [16..20] kernel_off = 0
    bundle.extend_from_slice(&0u32.to_le_bytes()); // [20..24] kernel_len = 0
    bundle.extend_from_slice(&0u32.to_le_bytes()); // [24..28] root_off = 0
    bundle.extend_from_slice(&0u32.to_le_bytes()); // [28..32] root_len = 0
    bundle.extend_from_slice(&pd_table_off.to_le_bytes()); // [32..36] pd_table_off
    bundle.extend_from_slice(&[0u8; 28]); // [36..64] _pad

    debug_assert_eq!(
        bundle.len(),
        HEADER_SIZE,
        "bundle header must be exactly 64 bytes"
    );

    // ── PD entry table (num_pds × 64 bytes) ─────────────────────────────────
    for (i, pd) in pds.iter().enumerate() {
        let mut name_buf = [0u8; 48];
        let name_bytes = pd.name.as_bytes();
        let copy_len = name_bytes.len().min(47);
        name_buf[..copy_len].copy_from_slice(&name_bytes[..copy_len]);

        bundle.extend_from_slice(&name_buf); // [0..48]  name
        bundle.extend_from_slice(&pd_elf_offsets[i].to_le_bytes()); // [48..52] elf_off
        bundle.extend_from_slice(&(pd_elfs[i].len() as u32).to_le_bytes()); // [52..56] elf_len
        bundle.push(pd.priority); // [56]     priority
        bundle.extend_from_slice(&[0u8; 7]); // [57..64] _pad
    }

    // ── PD ELF data ──────────────────────────────────────────────────────────
    for elf in &pd_elfs {
        bundle.extend_from_slice(elf);
    }

    debug_assert_eq!(bundle.len(), total_size, "bundle size mismatch");

    // 5. Write output file
    let mut out_file = fs::File::create(&args.out)
        .with_context(|| format!("failed to create bundle file: {}", args.out.display()))?;
    out_file
        .write_all(&bundle)
        .with_context(|| format!("failed to write bundle file: {}", args.out.display()))?;

    println!(
        "[gen-pd-bundle] wrote {}: {} bytes, {} PD(s)",
        args.out.display(),
        total_size,
        num_pds,
    );

    // 6. Build and sign the boot manifest.
    //
    // Hashing the wrong bytes is the way this whole feature silently fails
    // open: it must hash exactly the bytes the loader will later read, i.e.
    // bundle[pd_elf_offsets[i] .. pd_elf_offsets[i] + pd_elfs[i].len()).
    // `pd_elfs[i]` IS that exact byte range — it is what was written into
    // `bundle` at `pd_elf_offsets[i]` above (step 4 "PD ELF data") — so
    // hashing `pd_elfs[i]` directly here is hashing the identical bytes the
    // root task will hash out of the mapped `.pd_bundle` section at boot.
    let mut manifest_entries: Vec<ManifestEntry> = Vec::with_capacity(pds.len());
    for (i, pd) in pds.iter().enumerate() {
        use sha2::{Digest, Sha256};
        let digest: [u8; 32] = Sha256::digest(&pd_elfs[i]).into();
        manifest_entries.push(ManifestEntry::new(&pd.name, digest)?);
    }

    let repo_root = boot_manifest_repo_root(&args.system)?;
    let selection = select_anchor(&repo_root)?;
    let manifest_blob = build_signed_manifest(&manifest_entries, &selection.signing_key)
        .context("failed to build signed boot manifest")?;

    let manifest_out = args
        .manifest_out
        .clone()
        .unwrap_or_else(|| args.out.with_extension("manifest"));
    fs::write(&manifest_out, &manifest_blob)
        .with_context(|| format!("failed to write boot manifest: {}", manifest_out.display()))?;

    let pubkey_header_out = args.pubkey_header_out.clone().unwrap_or_else(|| {
        args.out
            .parent()
            .unwrap_or_else(|| std::path::Path::new("."))
            .join("boot_manifest_pubkey.h")
    });
    fs::write(&pubkey_header_out, render_pubkey_header(&selection)).with_context(|| {
        format!(
            "failed to write boot manifest pubkey header: {}",
            pubkey_header_out.display()
        )
    })?;

    let tier_name = match selection.tier {
        AnchorTier::None => "NONE (development, not gating)",
        AnchorTier::Vendor => "VENDOR",
        AnchorTier::Mok => "MOK",
    };
    println!(
        "[gen-pd-bundle] wrote {}: {} bytes, {} entries, trust anchor tier {} \
         (vendor key {}, mok key {}), signed with {} key ({})",
        manifest_out.display(),
        manifest_blob.len(),
        manifest_entries.len(),
        tier_name,
        if selection.vendor.present {
            "present"
        } else {
            "absent"
        },
        if selection.mok.present {
            "present"
        } else {
            "absent"
        },
        if selection.signing_key_is_dev {
            "DEVELOPMENT"
        } else {
            "custom"
        },
        selection.signing_key_source.display(),
    );
    println!("[gen-pd-bundle] wrote {}", pubkey_header_out.display());

    Ok(())
}

/// Find the repo root for signing-key resolution: walk upward from the
/// system TOML's directory looking for a `.git` entry, then try the same
/// walk from the process cwd (xtask is sometimes invoked with a cwd of the
/// `xtask/` crate itself, e.g. under `cargo test -p xtask`, not the repo
/// root). Errors out rather than silently resolving to the wrong directory
/// if neither search finds one — a wrong repo root here means reading (or
/// failing to read) the wrong signing key.
fn boot_manifest_repo_root(system_toml: &std::path::Path) -> Result<PathBuf> {
    fn walk_up_for_git(start: &std::path::Path) -> Option<PathBuf> {
        let mut dir = start.to_path_buf();
        loop {
            if dir.join(".git").exists() {
                return Some(dir);
            }
            if !dir.pop() {
                return None;
            }
        }
    }

    let from_system_toml = system_toml
        .canonicalize()
        .unwrap_or_else(|_| system_toml.to_path_buf());
    if let Some(root) = walk_up_for_git(&from_system_toml) {
        return Ok(root);
    }

    let cwd = std::env::current_dir().context("failed to resolve current directory")?;
    if let Some(root) = walk_up_for_git(&cwd) {
        return Ok(root);
    }

    anyhow::bail!(
        "could not locate repo root (no .git found above {} or {}) to resolve the boot manifest signing key",
        from_system_toml.display(),
        cwd.display()
    )
}

// ─── Unit tests ───────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use crate::boot_manifest::test_support::{with_anchor_env, ENV_LOCK};
    use crate::boot_manifest::{DEV_SIGNING_KEY_REL_PATH, VENDOR_SIGNING_KEY_ENV};
    use std::io::Read;
    use tempfile::NamedTempFile;

    fn fake_elf() -> Vec<u8> {
        let mut v = vec![0x7f, b'E', b'L', b'F'];
        v.extend_from_slice(&[0u8; 12]);
        v
    }

    /// `run()` calls `select_anchor()`, which reads the same process-global
    /// trust-anchor env vars boot_manifest.rs's own tests mutate -- hence
    /// ENV_LOCK/with_anchor_env are shared across both modules. Tests here
    /// just need SOME gating key configured so `run()` succeeds; the dev
    /// seed (committed in-tree, not a secret) mirrors what the real
    /// Makefile defaults AGENTOS_BUNDLE_SIGNING_KEY to.
    fn run_with_dev_vendor_key(args: &GenPdBundleArgs) -> anyhow::Result<()> {
        let _guard = ENV_LOCK.lock().unwrap();
        let repo_root = Command::new("git")
            .args(["rev-parse", "--show-toplevel"])
            .output()
            .expect("git rev-parse failed to run");
        let repo_root = std::path::PathBuf::from(
            String::from_utf8(repo_root.stdout)
                .expect("git output not UTF-8")
                .trim(),
        );
        let dev_seed_abs = repo_root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();
        with_anchor_env(&[(VENDOR_SIGNING_KEY_ENV, dev_seed_abs_str)], || run(args))
    }

    use std::process::Command;

    #[test]
    fn test_gen_pd_bundle_magic_and_layout() {
        let pd_dir = tempfile::tempdir().unwrap();
        std::fs::write(pd_dir.path().join("controller.elf"), fake_elf()).unwrap();
        std::fs::write(pd_dir.path().join("serial_pd.elf"), fake_elf()).unwrap();

        let toml_str = r#"
[[pd]]
name = "controller"
priority = 2

[[pd]]
name = "serial_pd"
priority = 1
"#;
        let toml_file = NamedTempFile::new().unwrap();
        std::fs::write(toml_file.path(), toml_str).unwrap();
        let out_file = NamedTempFile::new().unwrap();

        let manifest_out = tempfile::NamedTempFile::new().unwrap();
        let pubkey_header_out = tempfile::NamedTempFile::new().unwrap();
        let args = GenPdBundleArgs {
            system: toml_file.path().to_path_buf(),
            pd_dir: pd_dir.path().to_path_buf(),
            out: out_file.path().to_path_buf(),
            manifest_out: Some(manifest_out.path().to_path_buf()),
            pubkey_header_out: Some(pubkey_header_out.path().to_path_buf()),
        };

        run_with_dev_vendor_key(&args).expect("gen-pd-bundle failed");

        let mut bytes = Vec::new();
        std::fs::File::open(out_file.path())
            .unwrap()
            .read_to_end(&mut bytes)
            .unwrap();

        // Verify magic
        let magic = u64::from_le_bytes(bytes[0..8].try_into().unwrap());
        assert_eq!(magic, IMAGE_MAGIC, "magic mismatch");

        // Verify PD count
        let num_pds = u32::from_le_bytes(bytes[12..16].try_into().unwrap());
        assert_eq!(num_pds, 2);

        // kernel_off / kernel_len / root_off / root_len must all be 0
        let kernel_off = u32::from_le_bytes(bytes[16..20].try_into().unwrap());
        let kernel_len = u32::from_le_bytes(bytes[20..24].try_into().unwrap());
        let root_off = u32::from_le_bytes(bytes[24..28].try_into().unwrap());
        let root_len = u32::from_le_bytes(bytes[28..32].try_into().unwrap());
        assert_eq!(kernel_off, 0);
        assert_eq!(kernel_len, 0);
        assert_eq!(root_off, 0);
        assert_eq!(root_len, 0);

        // Minimum expected size: header + 2*pd_entry + 2*pd_elf
        let expected_min = HEADER_SIZE + 2 * PD_ENTRY_SIZE + 2 * fake_elf().len();
        assert!(bytes.len() >= expected_min);
    }

    /// A 48-byte PD name is rejected at build time rather than silently
    /// truncating to 47 bytes in the bundle (whose name[48] field is
    /// NUL-terminated) while the boot manifest's name[48] field (no NUL
    /// requirement, can hold the full 48 bytes) kept the untruncated name
    /// — which would never match at boot. See the BUNDLE_NAME_MAX check
    /// in `run()`.
    #[test]
    fn test_pd_name_exceeding_bundle_limit_is_rejected() {
        let pd_dir = tempfile::tempdir().unwrap();
        let long_name = "x".repeat(48);
        std::fs::write(pd_dir.path().join(format!("{long_name}.elf")), fake_elf()).unwrap();

        let toml_str = format!("[[pd]]\nname = \"{long_name}\"\npriority = 1\n");
        let toml_file = NamedTempFile::new().unwrap();
        std::fs::write(toml_file.path(), toml_str).unwrap();
        let out_file = NamedTempFile::new().unwrap();

        let args = GenPdBundleArgs {
            system: toml_file.path().to_path_buf(),
            pd_dir: pd_dir.path().to_path_buf(),
            out: out_file.path().to_path_buf(),
            manifest_out: None,
            pubkey_header_out: None,
        };

        let err = run_with_dev_vendor_key(&args).expect_err("48-byte PD name must be rejected");
        let msg = format!("{err:#}");
        assert!(
            msg.contains("47-byte limit"),
            "error should explain the 47-byte bundle name limit, got: {msg}"
        );

        // A 47-byte name is the boundary case and must still succeed.
        let ok_name = "y".repeat(47);
        std::fs::write(pd_dir.path().join(format!("{ok_name}.elf")), fake_elf()).unwrap();
        let toml_str_ok = format!("[[pd]]\nname = \"{ok_name}\"\npriority = 1\n");
        std::fs::write(toml_file.path(), toml_str_ok).unwrap();
        run_with_dev_vendor_key(&args).expect("47-byte PD name must be accepted");
    }
}
