// boot_manifest.rs — build-side writer for the signed boot manifest.
//
// Mirrors, byte-for-byte, the C reader at
// kernel/agentos-root-task/include/boot_manifest.h /
// kernel/agentos-root-task/src/boot_manifest.c:
//
//   [0..32)            aos_boot_manifest_hdr_t   (magic, version, count, reserved)
//   [32..32+80*count)  aos_boot_manifest_entry_t[count]  (name[48] + sha256[32])
//   [..+64)            Ed25519 signature over everything preceding it
//
// The two implementations (this one and the C reader) MUST agree on this
// layout byte-for-byte, or verification fails confusingly or — worse —
// silently validates the wrong bytes.  Three things enforce that agreement:
//
//   1. `ManifestHeader` / `ManifestEntry` below are `#[repr(C)]` with the
//      same field order as the C structs, and a `const _: () = ...` size
//      assertion pins them to 32 / 80 bytes respectively.
//   2. `boot_manifest.h` carries the identical `_Static_assert`s on the C
//      side.
//   3. `tests::roundtrip_blob_validates_in_c_reader` (below) writes a
//      manifest with this module and validates it with the real C reader
//      via a throwaway `cc` build — see tests/boot_manifest_roundtrip_check.c.
//
// Serialization is done field-by-field (not via a raw memory transmute) so
// endianness is explicit regardless of host vs. target byte order; both are
// little-endian today, but "explicit" beats "happens to match."

use std::path::Path;

use anyhow::{bail, Context, Result};
use ed25519_dalek::{Signer, SigningKey, VerifyingKey};

// ─── Wire format constants (must match boot_manifest.h) ─────────────────────

pub const MAGIC: u64 = 0x314e_414d_4253_4f41; // "AOSBMAN1", matches AOS_BOOT_MANIFEST_MAGIC
pub const VERSION: u32 = 1;
pub const MAX_PDS: usize = 32;
pub const SIG_LEN: usize = 64;
pub const NAME_LEN: usize = 48;

pub const HEADER_SIZE: usize = 32;
pub const ENTRY_SIZE: usize = 80;

/// `#[repr(C)]` mirror of `aos_boot_manifest_hdr_t`. Field order matches the
/// C struct exactly: magic, version, count, reserved. All fields are
/// naturally aligned at these offsets (8/4/4/16), so this layout is
/// identical under `#[repr(C)]` whether or not the C side additionally
/// marks itself `packed` — there is no padding to disagree about.
#[repr(C)]
pub struct ManifestHeader {
    pub magic: u64,
    pub version: u32,
    pub count: u32,
    pub reserved: [u8; 16],
}

const _: () = assert!(std::mem::size_of::<ManifestHeader>() == HEADER_SIZE);

/// `#[repr(C)]` mirror of `aos_boot_manifest_entry_t`: name[48] + sha256[32].
#[repr(C)]
pub struct ManifestEntry {
    pub name: [u8; NAME_LEN],
    pub sha256: [u8; 32],
}

const _: () = assert!(std::mem::size_of::<ManifestEntry>() == ENTRY_SIZE);

impl ManifestEntry {
    pub fn new(name: &str, sha256: [u8; 32]) -> Result<Self> {
        let bytes = name.as_bytes();
        if bytes.len() > NAME_LEN {
            bail!(
                "PD name '{name}' is {} bytes, exceeds the {NAME_LEN}-byte manifest name field",
                bytes.len()
            );
        }
        let mut name_buf = [0u8; NAME_LEN];
        name_buf[..bytes.len()].copy_from_slice(bytes);
        Ok(Self {
            name: name_buf,
            sha256,
        })
    }

    fn to_bytes(&self) -> [u8; ENTRY_SIZE] {
        let mut out = [0u8; ENTRY_SIZE];
        out[0..NAME_LEN].copy_from_slice(&self.name);
        out[NAME_LEN..ENTRY_SIZE].copy_from_slice(&self.sha256);
        out
    }
}

impl ManifestHeader {
    fn to_bytes(&self) -> [u8; HEADER_SIZE] {
        let mut out = [0u8; HEADER_SIZE];
        out[0..8].copy_from_slice(&self.magic.to_le_bytes());
        out[8..12].copy_from_slice(&self.version.to_le_bytes());
        out[12..16].copy_from_slice(&self.count.to_le_bytes());
        out[16..32].copy_from_slice(&self.reserved);
        out
    }
}

/// Build and sign a boot manifest: header + entries + trailing 64-byte
/// Ed25519 signature over everything preceding it.
///
/// Returns an error if `entries` is empty or exceeds `MAX_PDS` — those are
/// exactly the conditions the C validator rejects (`AOS_BOOT_MANIFEST_ERR_
/// COUNT_ZERO` / `_COUNT_MAX`), so a build must not produce such a blob in
/// the first place.
pub fn build_signed_manifest(
    entries: &[ManifestEntry],
    signing_key: &SigningKey,
) -> Result<Vec<u8>> {
    if entries.is_empty() {
        bail!("boot manifest must have at least one PD entry (empty manifest is a rejection, not a skip)");
    }
    if entries.len() > MAX_PDS {
        bail!(
            "boot manifest has {} entries, exceeds AOS_BOOT_MANIFEST_MAX_PDS ({MAX_PDS})",
            entries.len()
        );
    }

    let hdr = ManifestHeader {
        magic: MAGIC,
        version: VERSION,
        count: entries.len() as u32,
        reserved: [0u8; 16],
    };

    let mut body = Vec::with_capacity(HEADER_SIZE + entries.len() * ENTRY_SIZE + SIG_LEN);
    body.extend_from_slice(&hdr.to_bytes());
    for e in entries {
        body.extend_from_slice(&e.to_bytes());
    }

    // Sign header + entries (everything written so far); the signature
    // itself is appended after and is, by construction, not covered by
    // itself.
    let sig = signing_key.sign(&body);
    body.extend_from_slice(&sig.to_bytes());

    Ok(body)
}

// ─── Signing key handling ────────────────────────────────────────────────────
//
// Mirrors tools/sign-wasm's convention of keeping key material out of the
// signer's control flow (sign-wasm takes a key ID on the command line and
// never generates or stores a private key itself). Here the private key is
// a 32-byte Ed25519 seed read from a file:
//
//   - AGENTOS_BUNDLE_SIGNING_KEY set: path to a 32-byte seed file. Used as-is.
//   - AGENTOS_BUNDLE_SIGNING_KEY unset: falls back to the well-known in-tree
//     DEVELOPMENT key at kernel/agentos-root-task/keys/dev_signing_key.seed,
//     and prints a clear warning. This key is committed in-tree, obviously
//     not secret, and must never be used to sign anything but a development
//     build.
//
// Whether a build is "development-signed" (is_dev_key / the on-system
// `[rt] WARNING: DEVELOPMENT-signed image` marker) is decided by comparing
// the loaded seed BYTES against the in-tree development seed's bytes — not
// by whether AGENTOS_BUNDLE_SIGNING_KEY happened to be set. A path string
// can lie: a relative path, an absolute path, a symlink, or a copy can all
// point at the same 32 bytes as the dev seed while looking like "a real
// key was configured." Pointing AGENTOS_BUNDLE_SIGNING_KEY directly at
// kernel/agentos-root-task/keys/dev_signing_key.seed — by accident, or
// because a CI script or Makefile defaults it there "helpfully" — must
// still mark the resulting image as development-signed, exactly as if the
// variable had been left unset.

pub const DEV_SIGNING_KEY_REL_PATH: &str = "kernel/agentos-root-task/keys/dev_signing_key.seed";

pub struct LoadedSigningKey {
    pub signing_key: SigningKey,
    pub is_dev_key: bool,
    pub source: std::path::PathBuf,
}

pub fn load_signing_key(repo_root: &Path) -> Result<LoadedSigningKey> {
    let path = match std::env::var_os("AGENTOS_BUNDLE_SIGNING_KEY") {
        Some(p) => std::path::PathBuf::from(p),
        None => repo_root.join(DEV_SIGNING_KEY_REL_PATH),
    };

    let seed = std::fs::read(&path)
        .with_context(|| format!("failed to read signing key seed: {}", path.display()))?;
    if seed.len() != 32 {
        bail!(
            "signing key seed at {} is {} bytes, expected exactly 32 (an Ed25519 seed, not a PEM/DER/other encoding)",
            path.display(),
            seed.len()
        );
    }
    let mut seed_arr = [0u8; 32];
    seed_arr.copy_from_slice(&seed);
    let signing_key = SigningKey::from_bytes(&seed_arr);

    // is_dev_key is a function of WHICH KEY actually signed this build —
    // i.e. whether the loaded seed bytes equal the in-tree development
    // seed's bytes — never of how AGENTOS_BUNDLE_SIGNING_KEY was supplied.
    // See the module-level comment above for why a path comparison alone
    // would be defeatable.
    let dev_seed_path = repo_root.join(DEV_SIGNING_KEY_REL_PATH);
    let dev_seed = std::fs::read(&dev_seed_path).with_context(|| {
        format!(
            "failed to read in-tree development signing key seed for dev-key comparison: {}",
            dev_seed_path.display()
        )
    })?;
    let is_dev_key = dev_seed.len() == 32 && dev_seed == seed;

    if is_dev_key {
        eprintln!(
            "[gen-pd-bundle] WARNING: this build's manifest is signed with the \
             well-known in-tree DEVELOPMENT key ({}) — whether because \
             AGENTOS_BUNDLE_SIGNING_KEY is unset or because it points at that \
             same key. This image is DEVELOPMENT-SIGNED and must not be \
             treated as coming from any trusted build pipeline.",
            DEV_SIGNING_KEY_REL_PATH
        );
    }

    Ok(LoadedSigningKey {
        signing_key,
        is_dev_key,
        source: path,
    })
}

pub fn verifying_key_bytes(signing_key: &SigningKey) -> [u8; 32] {
    let vk: VerifyingKey = signing_key.verifying_key();
    vk.to_bytes()
}

/// Render a 32-byte public key as a C header defining the byte array the
/// root task compiles in and verifies the manifest signature against.
/// Included by kernel/agentos-root-task/src/main.c via the build directory
/// include path (see kernel/agentos-root-task/Makefile).
///
/// `is_dev_signed` controls `AOS_BOOT_MANIFEST_DEV_SIGNED`: when true, the
/// root task prints an unmissable `[rt] WARNING: DEVELOPMENT-signed image`
/// line right next to its normal "signature verified" success message, so
/// a development build announces itself on a running system too, not just
/// in the build-time stderr warning (which is gone by the time anyone is
/// looking at a booted board).
pub fn render_pubkey_header(pubkey: &[u8; 32], is_dev_signed: bool) -> String {
    let mut bytes_literal = String::new();
    for (i, b) in pubkey.iter().enumerate() {
        if i > 0 {
            bytes_literal.push_str(", ");
        }
        bytes_literal.push_str(&format!("0x{:02x}", b));
    }

    let dev_signed_flag = if is_dev_signed { 1 } else { 0 };

    format!(
        "/* GENERATED by `cargo xtask gen-pd-bundle` — do not edit by hand.\n\
         *\n\
         * Ed25519 public key compiled into the root task to verify the\n\
         * boot manifest's signature. It matches whichever private key\n\
         * signed THIS build's manifest (the well-known in-tree development\n\
         * key unless AGENTOS_BUNDLE_SIGNING_KEY was set). It does not, by\n\
         * itself, establish resistance to an operator who can replace the\n\
         * image (and this key with it) on the boot medium — see\n\
         * docs/superpowers/plans/2026-10-03-t3-image-verification.md.\n\
         */\n\
         #pragma once\n\
         #include <stdint.h>\n\
         static const uint8_t AOS_BOOT_MANIFEST_PUBKEY[32] = {{ {bytes_literal} }};\n\
         /* 1 iff this build's manifest was signed with the well-known\n\
          * in-tree development key (no AGENTOS_BUNDLE_SIGNING_KEY set).\n\
          * See boot_verify_manifest() in main.c. */\n\
         #define AOS_BOOT_MANIFEST_DEV_SIGNED {dev_signed_flag}\n"
    )
}

// ─── Tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;
    use sha2::{Digest, Sha256};
    use std::process::Command;

    fn repo_root() -> std::path::PathBuf {
        let output = Command::new("git")
            .args(["rev-parse", "--show-toplevel"])
            .output()
            .expect("git rev-parse failed to run");
        assert!(output.status.success(), "not in a git repository");
        std::path::PathBuf::from(
            String::from_utf8(output.stdout)
                .expect("git output not UTF-8")
                .trim(),
        )
    }

    #[test]
    fn header_and_entry_sizes_match_c_abi() {
        assert_eq!(std::mem::size_of::<ManifestHeader>(), 32);
        assert_eq!(std::mem::size_of::<ManifestEntry>(), 80);
    }

    #[test]
    fn dev_signing_key_seed_is_32_bytes() {
        let root = repo_root();
        let seed = std::fs::read(root.join(DEV_SIGNING_KEY_REL_PATH))
            .expect("dev signing key seed must be committed in-tree and readable");
        assert_eq!(
            seed.len(),
            32,
            "dev signing key seed must be exactly 32 bytes"
        );
    }

    // Serialize access to AGENTOS_BUNDLE_SIGNING_KEY: std::env::set_var /
    // remove_var affect the whole process, and cargo test runs tests in
    // parallel threads by default.
    static ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    /// Fix 2 regression test: pointing AGENTOS_BUNDLE_SIGNING_KEY directly at
    /// the in-tree development seed — not leaving it unset — must still be
    /// detected as development-signed. Before this fix, is_dev_key was keyed
    /// on "was the env var set at all", so this exact case produced a
    /// silently unmarked development-signed image.
    #[test]
    fn dev_seed_via_env_var_is_still_detected_as_dev_signed() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let dev_seed_abs = root.join(DEV_SIGNING_KEY_REL_PATH);

        let previous = std::env::var_os("AGENTOS_BUNDLE_SIGNING_KEY");
        // SAFETY (env mutation): serialized across this module's tests by
        // ENV_LOCK; restored immediately below before the lock is released.
        unsafe {
            std::env::set_var("AGENTOS_BUNDLE_SIGNING_KEY", &dev_seed_abs);
        }

        let loaded = load_signing_key(&root);

        unsafe {
            match &previous {
                Some(value) => std::env::set_var("AGENTOS_BUNDLE_SIGNING_KEY", value),
                None => std::env::remove_var("AGENTOS_BUNDLE_SIGNING_KEY"),
            }
        }

        let loaded = loaded.expect("loading the dev seed via the env var must still succeed");
        assert!(
            loaded.is_dev_key,
            "AGENTOS_BUNDLE_SIGNING_KEY pointed at the in-tree dev seed must still be \
             detected as development-signed, not just the unset-env-var default path"
        );
    }

    /// Converse of the above: a key that is NOT the dev seed must not be
    /// marked development-signed, preserving today's production-key
    /// behavior (no warning, AOS_BOOT_MANIFEST_DEV_SIGNED 0).
    #[test]
    fn non_dev_seed_via_env_var_is_not_dev_signed() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();

        let tmp = tempfile::tempdir().expect("tempdir");
        let fake_key_path = tmp.path().join("not-the-dev-key.seed");
        std::fs::write(&fake_key_path, [9u8; 32]).unwrap();

        let previous = std::env::var_os("AGENTOS_BUNDLE_SIGNING_KEY");
        unsafe {
            std::env::set_var("AGENTOS_BUNDLE_SIGNING_KEY", &fake_key_path);
        }

        let loaded = load_signing_key(&root);

        unsafe {
            match &previous {
                Some(value) => std::env::set_var("AGENTOS_BUNDLE_SIGNING_KEY", value),
                None => std::env::remove_var("AGENTOS_BUNDLE_SIGNING_KEY"),
            }
        }

        let loaded = loaded.expect("loading a well-formed 32-byte seed must succeed");
        assert!(
            !loaded.is_dev_key,
            "a seed with different bytes than the in-tree dev seed must not be marked dev-signed"
        );
    }

    #[test]
    fn build_signed_manifest_rejects_empty_and_oversized() {
        let signing_key = SigningKey::from_bytes(&[7u8; 32]);
        assert!(build_signed_manifest(&[], &signing_key).is_err());

        let too_many: Vec<ManifestEntry> = (0..MAX_PDS + 1)
            .map(|i| ManifestEntry::new(&format!("pd{i}"), [0u8; 32]).unwrap())
            .collect();
        assert!(build_signed_manifest(&too_many, &signing_key).is_err());
    }

    #[test]
    fn manifest_name_longer_than_field_is_rejected() {
        let long_name = "x".repeat(NAME_LEN + 1);
        assert!(ManifestEntry::new(&long_name, [0u8; 32]).is_err());
    }

    /// The critical ABI-agreement test: write a manifest with THIS module,
    /// then validate it with the real C reader
    /// (kernel/agentos-root-task/src/boot_manifest.c) via a throwaway `cc`
    /// build of tests/boot_manifest_roundtrip_check.c. If the Rust writer
    /// and the C reader ever disagree on byte layout, this fails here
    /// instead of surfacing as a confusing boot refusal later.
    #[test]
    fn roundtrip_blob_validates_in_c_reader() {
        let root = repo_root();

        let elf_bytes = b"not a real ELF, just bytes to hash for the test";
        let digest: [u8; 32] = Sha256::digest(elf_bytes).into();

        let signing_key = SigningKey::from_bytes(&[42u8; 32]);
        let entry = ManifestEntry::new("controller", digest).unwrap();
        let blob = build_signed_manifest(&[entry], &signing_key).unwrap();

        // Sanity: total size is header + 1 entry + signature.
        assert_eq!(blob.len(), HEADER_SIZE + ENTRY_SIZE + SIG_LEN);

        let tmp = tempfile::tempdir().expect("tempdir");
        let blob_path = tmp.path().join("manifest.bin");
        std::fs::write(&blob_path, &blob).unwrap();

        let checker_src = root.join("tests/boot_manifest_roundtrip_check.c");
        let checker_bin = tmp.path().join("checker");
        let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());

        let compile = Command::new(&cc)
            .args(["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-iquote"])
            .arg(root.join("kernel/agentos-root-task/include"))
            .arg(&checker_src)
            .arg(root.join("kernel/agentos-root-task/src/boot_manifest.c"))
            .arg("-o")
            .arg(&checker_bin)
            .output()
            .expect("failed to run cc");
        assert!(
            compile.status.success(),
            "roundtrip checker failed to compile:\nstdout: {}\nstderr: {}",
            String::from_utf8_lossy(&compile.stdout),
            String::from_utf8_lossy(&compile.stderr)
        );

        let hex_digest = digest
            .iter()
            .map(|b| format!("{:02x}", b))
            .collect::<String>();
        let run = Command::new(&checker_bin)
            .arg(&blob_path)
            .arg("controller")
            .arg(&hex_digest)
            .output()
            .expect("failed to run roundtrip checker");
        assert!(
            run.status.success(),
            "C reader rejected a blob the Rust writer produced:\nstdout: {}\nstderr: {}",
            String::from_utf8_lossy(&run.stdout),
            String::from_utf8_lossy(&run.stderr)
        );
        assert_eq!(String::from_utf8_lossy(&run.stdout).trim(), "OK");
    }

    #[test]
    fn roundtrip_rejects_tampered_digest() {
        let root = repo_root();
        let signing_key = SigningKey::from_bytes(&[9u8; 32]);
        let entry = ManifestEntry::new("controller", [0x11u8; 32]).unwrap();
        let blob = build_signed_manifest(&[entry], &signing_key).unwrap();

        let tmp = tempfile::tempdir().expect("tempdir");
        let blob_path = tmp.path().join("manifest.bin");
        std::fs::write(&blob_path, &blob).unwrap();

        let checker_src = root.join("tests/boot_manifest_roundtrip_check.c");
        let checker_bin = tmp.path().join("checker");
        let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_string());
        let compile = Command::new(&cc)
            .args(["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-iquote"])
            .arg(root.join("kernel/agentos-root-task/include"))
            .arg(&checker_src)
            .arg(root.join("kernel/agentos-root-task/src/boot_manifest.c"))
            .arg("-o")
            .arg(&checker_bin)
            .output()
            .expect("failed to run cc");
        assert!(compile.status.success());

        // Wrong expected digest (all 0xff instead of the stored 0x11s) must
        // be rejected by the C reader's comparison.
        let wrong_hex = "ff".repeat(32);
        let run = Command::new(&checker_bin)
            .arg(&blob_path)
            .arg("controller")
            .arg(&wrong_hex)
            .output()
            .expect("failed to run roundtrip checker");
        assert!(
            !run.status.success(),
            "tampered digest should have been rejected"
        );
    }
}
