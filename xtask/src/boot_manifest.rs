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

// ─── Signing key handling / trust anchor tier selection (T10) ───────────────
//
// Mirrors tools/sign-wasm's convention of keeping key material out of the
// signer's control flow (sign-wasm takes a key ID on the command line and
// never generates or stores a private key itself). Each of the two possible
// keys (vendor, machine-owner) is a 32-byte Ed25519 seed read from a file
// named by its own env var:
//
//   - AGENTOS_BUNDLE_SIGNING_KEY: the vendor key's seed path.
//   - AGENTOS_MOK_SIGNING_KEY: the machine-owner (MOK) key's seed path —
//     this is the MOK "enrolment store" for this platform today,
//     build-time-provisioned (see that const's doc comment below).
//
// UNLIKE T3's original single-key design, neither of these falls back to
// the in-tree development seed when unset. `select_anchor` below is the
// ONLY place tier selection happens, strictest-first, and it is the ONLY
// path that may choose the dev seed (and ONLY for AOS_ANCHOR_NONE, behind
// an explicit AGENTOS_TRUST_ANCHOR=none opt-in) — see its doc comment. A
// build with neither key configured and no opt-in is a build ERROR, not a
// silent fallback: that silent-fallback shape is exactly what let T3's
// original `load_signing_key` mark an unset env var as "vendor-equivalent"
// when it was really running on the public dev key, and it is the one
// outcome this whole feature exists to prevent from recurring.
//
// Whether the key that actually SIGNED this build's manifest is
// "development-signed" (is_dev_key / the on-system `[rt] WARNING:
// DEVELOPMENT-signed image` marker) is decided by comparing the loaded seed
// BYTES against the in-tree development seed's bytes — not by which path
// supplied it. A path string can lie: a relative path, an absolute path, a
// symlink, or a copy can all point at the same 32 bytes as the dev seed
// while looking like "a real key was configured." Pointing either signing
// env var directly at kernel/agentos-root-task/keys/dev_signing_key.seed —
// by accident, or because a CI script or Makefile defaults it there
// "helpfully" — must still mark the resulting image as development-signed.

pub const DEV_SIGNING_KEY_REL_PATH: &str = "kernel/agentos-root-task/keys/dev_signing_key.seed";

/// Env var carrying the vendor (built-in) signing key's 32-byte Ed25519
/// seed path. Set -> a vendor key is configured for this build.
pub const VENDOR_SIGNING_KEY_ENV: &str = "AGENTOS_BUNDLE_SIGNING_KEY";

/// Env var carrying the machine-owner (MOK) signing key's 32-byte Ed25519
/// seed path. This is the MOK "enrolment store" for this platform today:
/// build-time-provisioned, not a persistent runtime enrolment mechanism.
/// See kernel/agentos-root-task/keys/README.md for why, and T10's report
/// for what would need to exist (writable, attested storage reachable at
/// boot) before this could become a real runtime enrolment flow.
pub const MOK_SIGNING_KEY_ENV: &str = "AGENTOS_MOK_SIGNING_KEY";

/// Explicit opt-in required to select the non-gating development anchor
/// when neither signing key is configured. See `select_anchor` below —
/// this is the ONE thing that must never be reachable by accident.
pub const TRUST_ANCHOR_ENV: &str = "AGENTOS_TRUST_ANCHOR";
pub const TRUST_ANCHOR_OPT_IN_NONE: &str = "none";

/// Mirrors `aos_anchor_tier_t` in
/// kernel/agentos-root-task/include/contracts/trust_anchor.h. Numeric
/// values MUST stay identical to that enum: main.c builds an
/// `aos_anchor_state_t` directly from `AOS_BOOT_TRUST_ANCHOR_TIER`, the
/// macro `render_pubkey_header` below emits from `self as u32`.
/// `AOS_ANCHOR_HARDWARE` has no build-time selection path (there is no key
/// source for it yet) so it is not a variant here.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AnchorTier {
    None = 0,
    Vendor = 1,
    Mok = 2,
}

/// One key slot (vendor or mok) as it will be compiled into the root task.
#[derive(Clone)]
pub struct AnchorKeySlot {
    pub present: bool,
    pub pubkey: [u8; 32],
    pub is_dev_key: bool,
    pub source: Option<std::path::PathBuf>,
}

impl AnchorKeySlot {
    fn absent() -> Self {
        AnchorKeySlot {
            present: false,
            pubkey: [0u8; 32],
            is_dev_key: false,
            source: None,
        }
    }
}

/// Result of build-time trust anchor selection: the tier, both key slots
/// as they'll be compiled in, and the single key that actually signs THIS
/// build's manifest.
pub struct AnchorSelection {
    pub tier: AnchorTier,
    pub vendor: AnchorKeySlot,
    pub mok: AnchorKeySlot,
    pub signing_key: SigningKey,
    pub signing_key_is_dev: bool,
    pub signing_key_source: std::path::PathBuf,
}

/// Read a 32-byte Ed25519 seed from the file named by `env_var`, or
/// `Ok(None)` if that env var is unset. Does NOT fall back to the dev seed
/// on its own — callers decide what "no key for this slot" means. Also
/// returns whether the loaded seed bytes equal the in-tree development
/// seed's bytes (never inferred from how the env var was supplied — a
/// path can point at the dev seed by accident or by a "helpful" default,
/// and must still be detected; see the module comment above).
fn load_env_seed_key(
    repo_root: &Path,
    env_var: &str,
) -> Result<Option<(SigningKey, bool, std::path::PathBuf)>> {
    // An EMPTY value means "not set", not "a key at the empty path".
    //
    // This is not defensive padding: `export NAME` in a Makefile puts `NAME=`
    // into the recipe's environment even when NAME is undefined, and
    // kernel/agentos-root-task/Makefile now exports all three anchor variables
    // unconditionally so that make's view of the selection and this function's
    // view cannot diverge. Without this, every ordinary build -- which sets no
    // MOK -- would see `Some("")` here and die on `failed to read signing key
    // seed: ` with an empty path.
    //
    // Treating it as unset is also the fail-closed reading: it yields NO KEY,
    // so select_anchor() takes its key-less path, which is a hard error unless
    // the caller explicitly opted in to the development anchor. There is no
    // value of this variable that turns a gating build into a non-gating one
    // without that opt-in.
    let path = match std::env::var_os(env_var) {
        Some(p) if !p.is_empty() => std::path::PathBuf::from(p),
        _ => return Ok(None),
    };

    let seed = std::fs::read(&path).with_context(|| {
        format!(
            "failed to read signing key seed ({env_var}): {}",
            path.display()
        )
    })?;
    if seed.len() != 32 {
        bail!(
            "signing key seed at {} ({env_var}) is {} bytes, expected exactly 32 (an Ed25519 seed, not a PEM/DER/other encoding)",
            path.display(),
            seed.len()
        );
    }
    let mut seed_arr = [0u8; 32];
    seed_arr.copy_from_slice(&seed);
    let signing_key = SigningKey::from_bytes(&seed_arr);

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
            "[gen-pd-bundle] WARNING: {env_var} points at the well-known in-tree \
             DEVELOPMENT key ({}). This key is DEVELOPMENT-SIGNED and must not be \
             treated as coming from any trusted build pipeline, regardless of \
             which trust anchor tier this build selects.",
            DEV_SIGNING_KEY_REL_PATH
        );
    }

    Ok(Some((signing_key, is_dev_key, path)))
}

fn slot_from(loaded: &Option<(SigningKey, bool, std::path::PathBuf)>) -> AnchorKeySlot {
    match loaded {
        Some((sk, is_dev, path)) => AnchorKeySlot {
            present: true,
            pubkey: verifying_key_bytes(sk),
            is_dev_key: *is_dev,
            source: Some(path.clone()),
        },
        None => AnchorKeySlot::absent(),
    }
}

/// Select this build's trust anchor tier, strictest-first, and resolve the
/// key material it needs.
///
/// Selection (mirrors the gating policy in
/// libs/pd-support/trust_anchor.c::aos_anchor_validate):
///
///   - AGENTOS_MOK_SIGNING_KEY set (regardless of vendor) -> AOS_ANCHOR_MOK.
///     The MOK tier is defined by mok.present; the vendor key is optional
///     for it (present extends trust to vendor-signed images too, absent
///     means the owner stands alone — see trust_anchor.h).
///   - else AGENTOS_BUNDLE_SIGNING_KEY set alone -> AOS_ANCHOR_VENDOR.
///   - else, with NEITHER key set, AGENTOS_TRUST_ANCHOR=none explicitly ->
///     AOS_ANCHOR_NONE (development, non-gating).
///   - else: ERROR. A build with no key and no opt-in must fail rather
///     than silently producing a non-gating image — this is the one
///     outcome this whole feature must never produce.
///
/// The manifest is signed with the vendor key when one is configured
/// (keeps a vendor build's manifest vendor-signed even when a MOK is also
/// layered on for this image), otherwise with the MOK key (the owner
/// standing alone), otherwise — AOS_ANCHOR_NONE only — with the in-tree
/// dev seed, purely so the manifest is a parseable, checkable blob for the
/// digest machinery; it carries no trust claim at that tier (see
/// AOS_ANCHOR_NONE's gating policy — verification still runs, it just
/// does not gate).
pub fn select_anchor(repo_root: &Path) -> Result<AnchorSelection> {
    let vendor_loaded = load_env_seed_key(repo_root, VENDOR_SIGNING_KEY_ENV)?;
    let mok_loaded = load_env_seed_key(repo_root, MOK_SIGNING_KEY_ENV)?;

    // Validate AGENTOS_TRUST_ANCHOR up front, regardless of which branch
    // below actually consults it: only "none" is a meaningful value (it is
    // only ever an opt-IN, never a tier selector in its own right — a key
    // always wins strictest-first). An unset var is fine (empty string
    // compares unequal to "none" below and is simply not an opt-in). Any
    // OTHER non-empty value is almost certainly a typo for "none" (e.g.
    // "None", "vendor", "mok") and must fail loudly rather than be
    // silently treated as "not none" and ignored -- that would be exactly
    // the kind of input a caller believes does something but doesn't.
    let trust_anchor_var = std::env::var(TRUST_ANCHOR_ENV).unwrap_or_default();
    if !trust_anchor_var.is_empty() && trust_anchor_var != TRUST_ANCHOR_OPT_IN_NONE {
        bail!(
            "{TRUST_ANCHOR_ENV}={trust_anchor_var:?} is not a recognized value -- the only \
             meaningful value is {TRUST_ANCHOR_OPT_IN_NONE:?} (an explicit opt-in to the \
             non-gating development anchor, consulted only when neither {VENDOR_SIGNING_KEY_ENV} \
             nor {MOK_SIGNING_KEY_ENV} is set). Unset {TRUST_ANCHOR_ENV} entirely if you did not \
             mean to set it."
        );
    }
    if trust_anchor_var == TRUST_ANCHOR_OPT_IN_NONE
        && (vendor_loaded.is_some() || mok_loaded.is_some())
    {
        eprintln!(
            "[gen-pd-bundle] NOTE: {TRUST_ANCHOR_ENV}={TRUST_ANCHOR_OPT_IN_NONE} was set but a \
             signing key is also configured ({}{}); tier selection is strictest-first, so the \
             configured key wins and this build is GATING, not the AOS_ANCHOR_NONE opt-in you \
             asked for.",
            if vendor_loaded.is_some() {
                VENDOR_SIGNING_KEY_ENV
            } else {
                ""
            },
            if mok_loaded.is_some() {
                if vendor_loaded.is_some() {
                    format!(" and {MOK_SIGNING_KEY_ENV}")
                } else {
                    MOK_SIGNING_KEY_ENV.to_string()
                }
            } else {
                String::new()
            }
        );
    }

    if mok_loaded.is_some() {
        let (signing_key, signing_is_dev, signing_source) = match &vendor_loaded {
            Some((sk, dev, path)) => (sk.clone(), *dev, path.clone()),
            None => {
                let (sk, dev, path) = mok_loaded.as_ref().unwrap();
                (sk.clone(), *dev, path.clone())
            }
        };
        return Ok(AnchorSelection {
            tier: AnchorTier::Mok,
            vendor: slot_from(&vendor_loaded),
            mok: slot_from(&mok_loaded),
            signing_key,
            signing_key_is_dev: signing_is_dev,
            signing_key_source: signing_source,
        });
    }

    if let Some((sk, dev, path)) = &vendor_loaded {
        return Ok(AnchorSelection {
            tier: AnchorTier::Vendor,
            vendor: slot_from(&vendor_loaded),
            mok: AnchorKeySlot::absent(),
            signing_key: sk.clone(),
            signing_key_is_dev: *dev,
            signing_key_source: path.clone(),
        });
    }

    // Neither key configured. The ONLY way to reach AOS_ANCHOR_NONE is the
    // explicit opt-in below — there is no silent fallback here.
    if trust_anchor_var == TRUST_ANCHOR_OPT_IN_NONE {
        eprintln!(
            "[gen-pd-bundle] WARNING: {TRUST_ANCHOR_ENV}={TRUST_ANCHOR_OPT_IN_NONE} — this \
             build selects the AOS_ANCHOR_NONE (development) trust anchor. Verification still \
             runs (digests are computed and mismatches reported) but nothing gates boot. This \
             image establishes NOTHING about integrity and must never be presented as, or \
             mistaken for, a production image."
        );
        let dev_path = repo_root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed = std::fs::read(&dev_path).with_context(|| {
            format!(
                "failed to read in-tree development signing key seed: {}",
                dev_path.display()
            )
        })?;
        if dev_seed.len() != 32 {
            bail!(
                "in-tree development signing key seed at {} is {} bytes, expected 32",
                dev_path.display(),
                dev_seed.len()
            );
        }
        let mut seed_arr = [0u8; 32];
        seed_arr.copy_from_slice(&dev_seed);
        let signing_key = SigningKey::from_bytes(&seed_arr);
        return Ok(AnchorSelection {
            tier: AnchorTier::None,
            vendor: AnchorKeySlot::absent(),
            mok: AnchorKeySlot::absent(),
            signing_key,
            signing_key_is_dev: true,
            signing_key_source: dev_path,
        });
    }

    bail!(
        "no trust anchor key configured for this build: neither {VENDOR_SIGNING_KEY_ENV} \
         (vendor key) nor {MOK_SIGNING_KEY_ENV} (machine-owner key) is set, and \
         {TRUST_ANCHOR_ENV}={TRUST_ANCHOR_OPT_IN_NONE} was not given to explicitly opt into \
         the non-gating development anchor. A build with no key and no opt-in MUST fail here \
         rather than silently producing an image that does not gate boot — see \
         kernel/agentos-root-task/keys/README.md and docs/TCB.md's trust anchor tier table."
    )
}

pub fn verifying_key_bytes(signing_key: &SigningKey) -> [u8; 32] {
    let vk: VerifyingKey = signing_key.verifying_key();
    vk.to_bytes()
}

/// Render the generated C header declaring this build's trust anchor:
/// the tier, both key slots (vendor/mok, each with a presence flag and
/// pubkey bytes), and the dev-signed indicator. Included by
/// kernel/agentos-root-task/src/main.c via the build directory include
/// path (see kernel/agentos-root-task/Makefile).
///
/// The tier and the dev-signed indicator are DISTINCT and both emitted:
/// "signed with the in-tree development key" and "no key at all" are
/// different states (see T10's brief) — a vendor-tier build signed with
/// the dev key is tier=VENDOR (gates boot) AND dev-signed (announced as
/// such), not conflated into one boolean.
pub fn render_pubkey_header(selection: &AnchorSelection) -> String {
    fn bytes_literal(pubkey: &[u8; 32]) -> String {
        let mut s = String::new();
        for (i, b) in pubkey.iter().enumerate() {
            if i > 0 {
                s.push_str(", ");
            }
            s.push_str(&format!("0x{:02x}", b));
        }
        s
    }

    let tier_value = selection.tier as u32;
    let vendor_present = if selection.vendor.present { 1 } else { 0 };
    let mok_present = if selection.mok.present { 1 } else { 0 };
    let dev_signed_flag = if selection.signing_key_is_dev { 1 } else { 0 };
    let vendor_bytes = bytes_literal(&selection.vendor.pubkey);
    let mok_bytes = bytes_literal(&selection.mok.pubkey);

    format!(
        "/* GENERATED by `cargo xtask gen-pd-bundle` — do not edit by hand.\n\
         *\n\
         * This build's trust anchor (see kernel/agentos-root-task/include/\n\
         * contracts/trust_anchor.h and libs/pd-support/trust_anchor.c for the\n\
         * tier model and gating policy; docs/TCB.md for what each tier\n\
         * establishes). Vendor and/or MOK public keys here are whichever keys\n\
         * were configured at build time (AGENTOS_BUNDLE_SIGNING_KEY /\n\
         * AGENTOS_MOK_SIGNING_KEY); neither, by itself, establishes resistance\n\
         * to an operator who can replace the image (and these keys with it) on\n\
         * the boot medium — see\n\
         * docs/superpowers/plans/2026-10-03-t3-image-verification.md.\n\
         */\n\
         #pragma once\n\
         #include <stdint.h>\n\
         \n\
         /* One of AOS_ANCHOR_NONE/VENDOR/MOK from contracts/trust_anchor.h.\n\
          * AOS_ANCHOR_HARDWARE has no build-time selection path (yet). */\n\
         #define AOS_BOOT_TRUST_ANCHOR_TIER {tier_value}u\n\
         \n\
         static const uint8_t AOS_BOOT_MANIFEST_VENDOR_PUBKEY[32] = {{ {vendor_bytes} }};\n\
         #define AOS_BOOT_MANIFEST_VENDOR_PRESENT {vendor_present}\n\
         \n\
         static const uint8_t AOS_BOOT_MANIFEST_MOK_PUBKEY[32] = {{ {mok_bytes} }};\n\
         #define AOS_BOOT_MANIFEST_MOK_PRESENT {mok_present}\n\
         \n\
         /* 1 iff the key that actually SIGNED this build's manifest is the\n\
          * well-known in-tree development key. Distinct from the tier above:\n\
          * a VENDOR- or MOK-tier build can still be dev-signed (e.g. a local\n\
          * build pointed its vendor var at the dev seed on purpose or by\n\
          * accident) and must announce that too. See boot_verify_manifest()\n\
          * in main.c. */\n\
         #define AOS_BOOT_MANIFEST_DEV_SIGNED {dev_signed_flag}\n"
    )
}

// ─── Test support (shared with cmd_gen_pd_bundle's tests) ───────────────────
//
// `select_anchor`/`load_env_seed_key` read process-global env vars, and
// `cargo test` runs tests in parallel threads within one binary. Every test
// anywhere in this crate that touches VENDOR_SIGNING_KEY_ENV /
// MOK_SIGNING_KEY_ENV / TRUST_ANCHOR_ENV -- including cmd_gen_pd_bundle.rs's
// `run()` tests, which call `select_anchor` transitively -- MUST serialize
// on the same lock and go through the same env pair, or they race each
// other's env mutations. Hence this lives outside `mod tests` as a
// `pub(crate)` module rather than being private to it.
#[cfg(test)]
pub(crate) mod test_support {
    use super::{MOK_SIGNING_KEY_ENV, TRUST_ANCHOR_ENV, VENDOR_SIGNING_KEY_ENV};

    pub(crate) static ENV_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    const ANCHOR_ENV_VARS: [&str; 3] = [
        VENDOR_SIGNING_KEY_ENV,
        MOK_SIGNING_KEY_ENV,
        TRUST_ANCHOR_ENV,
    ];

    /// Run `f` with exactly the given (var, value) pairs set and every
    /// other trust-anchor env var cleared, then restore all three to
    /// whatever they were before this call. Caller MUST hold `ENV_LOCK`.
    pub(crate) fn with_anchor_env<T>(vars: &[(&str, &str)], f: impl FnOnce() -> T) -> T {
        let previous: Vec<(&str, Option<std::ffi::OsString>)> = ANCHOR_ENV_VARS
            .iter()
            .map(|v| (*v, std::env::var_os(v)))
            .collect();

        // SAFETY (env mutation): serialized across every caller in this
        // crate by ENV_LOCK; restored below before returning.
        unsafe {
            for v in ANCHOR_ENV_VARS {
                std::env::remove_var(v);
            }
            for (k, val) in vars {
                std::env::set_var(k, val);
            }
        }

        let result = f();

        unsafe {
            for (k, val) in previous {
                match val {
                    Some(v) => std::env::set_var(k, v),
                    None => std::env::remove_var(k),
                }
            }
        }

        result
    }
}

#[cfg(test)]
mod tests {
    use super::test_support::{with_anchor_env, ENV_LOCK};
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
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();

        let loaded = with_anchor_env(&[(VENDOR_SIGNING_KEY_ENV, dev_seed_abs_str)], || {
            load_env_seed_key(&root, VENDOR_SIGNING_KEY_ENV)
        });

        let loaded = loaded
            .expect("loading the dev seed via the env var must not error")
            .expect("the env var was set, so a key must be returned");
        assert!(
            loaded.1,
            "AGENTOS_BUNDLE_SIGNING_KEY pointed at the in-tree dev seed must still be \
             detected as development-signed"
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
        let fake_key_path_str = fake_key_path.to_str().unwrap();

        let loaded = with_anchor_env(&[(VENDOR_SIGNING_KEY_ENV, fake_key_path_str)], || {
            load_env_seed_key(&root, VENDOR_SIGNING_KEY_ENV)
        });

        let loaded = loaded
            .expect("loading a well-formed 32-byte seed must not error")
            .expect("the env var was set, so a key must be returned");
        assert!(
            !loaded.1,
            "a seed with different bytes than the in-tree dev seed must not be marked dev-signed"
        );
    }

    /// The one unacceptable outcome this whole task exists to prevent: a
    /// build with NEITHER key configured and NO explicit opt-in must fail,
    /// never silently select AOS_ANCHOR_NONE or any other tier.
    #[test]
    fn select_anchor_with_no_key_and_no_opt_in_errors() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let result = with_anchor_env(&[], || select_anchor(&root));
        assert!(
            result.is_err(),
            "a build with no signing key and no AGENTOS_TRUST_ANCHOR=none opt-in must fail, \
             not silently produce an image"
        );
    }

    /// An unrecognized AGENTOS_TRUST_ANCHOR value (a typo for "none", or
    /// any other garbage) must fail loudly rather than silently be treated
    /// as "not none" and ignored -- see review Minor #6.
    #[test]
    fn select_anchor_with_unrecognized_trust_anchor_value_errors() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        for bogus in ["None", "vendor", "mok", "nonee", "NONE"] {
            let result = with_anchor_env(&[(TRUST_ANCHOR_ENV, bogus)], || select_anchor(&root));
            assert!(
                result.is_err(),
                "AGENTOS_TRUST_ANCHOR={bogus:?} must be rejected, not silently ignored"
            );
        }
    }

    /// AGENTOS_TRUST_ANCHOR=none set together with a signing key does not
    /// error (the configured key safely wins, strictest-first) and does
    /// not silently discard the opt-in without comment -- see review Minor
    /// #6. This only exercises the non-error path; the NOTE is printed to
    /// stderr and not asserted here, but the selection itself must still
    /// be the gating tier the key implies, not NONE.
    #[test]
    fn select_anchor_with_none_opt_in_and_a_key_prefers_the_key() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let dev_seed_abs = root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();
        let selection = with_anchor_env(
            &[
                (TRUST_ANCHOR_ENV, TRUST_ANCHOR_OPT_IN_NONE),
                (VENDOR_SIGNING_KEY_ENV, dev_seed_abs_str),
            ],
            || select_anchor(&root),
        )
        .expect("a configured key alongside the none opt-in must still succeed");
        assert_eq!(
            selection.tier,
            AnchorTier::Vendor,
            "a configured vendor key must win over a none opt-in, strictest-first"
        );
    }

    /// The explicit opt-in is the only path to AOS_ANCHOR_NONE.
    #[test]
    fn select_anchor_with_explicit_opt_in_selects_none_tier() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let selection = with_anchor_env(&[(TRUST_ANCHOR_ENV, TRUST_ANCHOR_OPT_IN_NONE)], || {
            select_anchor(&root)
        })
        .expect("explicit none opt-in must succeed");
        assert_eq!(selection.tier, AnchorTier::None);
        assert!(!selection.vendor.present);
        assert!(!selection.mok.present);
        assert!(
            selection.signing_key_is_dev,
            "AOS_ANCHOR_NONE signs with the dev seed so the manifest is still a parseable, \
             checkable blob for the digest machinery -- it is not a trust claim at this tier"
        );
    }

    /// An exported-but-EMPTY key variable means "no key", not "a key at the
    /// empty path".
    ///
    /// kernel/agentos-root-task/Makefile exports all three anchor variables
    /// unconditionally so make's recorded selection and gen-pd-bundle's
    /// environment cannot diverge; `export` on an undefined make variable puts
    /// `NAME=` into the recipe environment, so this is the ordinary case for
    /// AGENTOS_MOK_SIGNING_KEY on every vendor build, not a corner case.
    #[test]
    fn empty_key_env_vars_are_treated_as_unset() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let dev_seed_abs = root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();

        // Vendor key set, MOK exported empty: still a plain VENDOR build.
        let selection = with_anchor_env(
            &[
                (VENDOR_SIGNING_KEY_ENV, dev_seed_abs_str),
                (MOK_SIGNING_KEY_ENV, ""),
            ],
            || select_anchor(&root),
        )
        .expect("an empty MOK variable must read as unset, not as a key at the empty path");
        assert_eq!(selection.tier, AnchorTier::Vendor);
        assert!(!selection.mok.present);

        // Both key variables exported empty and no opt-in: fail-closed. The
        // empty value must not be mistaken for a configured key, and must not
        // quietly become AOS_ANCHOR_NONE either.
        let result = with_anchor_env(
            &[(VENDOR_SIGNING_KEY_ENV, ""), (MOK_SIGNING_KEY_ENV, "")],
            || select_anchor(&root),
        );
        assert!(
            result.is_err(),
            "empty key variables with no explicit opt-in must be a build error, not a \
             silently non-gating image"
        );
    }

    /// A build with only the vendor key configured selects AOS_ANCHOR_VENDOR.
    #[test]
    fn select_anchor_with_only_vendor_key_selects_vendor_tier() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let dev_seed_abs = root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();
        let selection = with_anchor_env(&[(VENDOR_SIGNING_KEY_ENV, dev_seed_abs_str)], || {
            select_anchor(&root)
        })
        .expect("vendor-only selection must succeed");
        assert_eq!(selection.tier, AnchorTier::Vendor);
        assert!(selection.vendor.present);
        assert!(!selection.mok.present);
    }

    /// A build with only the MOK key configured selects AOS_ANCHOR_MOK with
    /// the vendor slot absent -- the owner standing alone, per the ruling
    /// that supersedes the original additive-only MOK design.
    #[test]
    fn select_anchor_with_only_mok_key_selects_mok_tier_standalone() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let dev_seed_abs = root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();
        let selection = with_anchor_env(&[(MOK_SIGNING_KEY_ENV, dev_seed_abs_str)], || {
            select_anchor(&root)
        })
        .expect("mok-only selection must succeed");
        assert_eq!(selection.tier, AnchorTier::Mok);
        assert!(!selection.vendor.present);
        assert!(selection.mok.present);
    }

    /// Both keys configured: AOS_ANCHOR_MOK with vendor ALSO present, so
    /// either key verifies an image on this machine.
    #[test]
    fn select_anchor_with_both_keys_selects_mok_tier_with_vendor_present() {
        let _guard = ENV_LOCK.lock().unwrap();
        let root = repo_root();
        let dev_seed_abs = root.join(DEV_SIGNING_KEY_REL_PATH);
        let dev_seed_abs_str = dev_seed_abs.to_str().unwrap();

        let tmp = tempfile::tempdir().expect("tempdir");
        let mok_key_path = tmp.path().join("mok.seed");
        std::fs::write(&mok_key_path, [3u8; 32]).unwrap();
        let mok_key_path_str = mok_key_path.to_str().unwrap();

        let selection = with_anchor_env(
            &[
                (VENDOR_SIGNING_KEY_ENV, dev_seed_abs_str),
                (MOK_SIGNING_KEY_ENV, mok_key_path_str),
            ],
            || select_anchor(&root),
        )
        .expect("vendor+mok selection must succeed");
        assert_eq!(selection.tier, AnchorTier::Mok);
        assert!(selection.vendor.present);
        assert!(selection.mok.present);
        // This build's manifest signs with the vendor key when one is
        // configured, even under the MOK tier.
        assert!(selection.signing_key_is_dev);
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
