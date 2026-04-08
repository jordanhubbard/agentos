use anyhow::{anyhow, bail, Context, Result};
use clap::Parser;
use sha2::{Digest, Sha512};
use std::path::PathBuf;

// ── LEB128 helpers ────────────────────────────────────────────────────── //

/// Read an unsigned LEB128 value from `data` at `pos`.
/// Returns `(value, new_pos)`.
fn read_leb128(data: &[u8], pos: usize) -> (u32, usize) {
    let mut result: u32 = 0;
    let mut shift: u32 = 0;
    let mut pos = pos;
    while pos < data.len() && shift < 35 {
        let byte = data[pos];
        pos += 1;
        result |= ((byte & 0x7F) as u32) << shift;
        shift += 7;
        if byte & 0x80 == 0 {
            break;
        }
    }
    (result, pos)
}

/// Encode a `u32` as unsigned LEB128.
fn write_leb128(mut value: u32) -> Vec<u8> {
    let mut out = Vec::new();
    loop {
        let mut byte = (value & 0x7F) as u8;
        value >>= 7;
        if value != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if value == 0 {
            break;
        }
    }
    out
}

// ── WASM section scanner ──────────────────────────────────────────────── //

/// Walk the WASM binary from offset 8 (past magic + version) and locate
/// the first custom section whose name matches `name`.
///
/// Returns `(payload_offset, payload_len)` where the payload is the bytes
/// after the section name string (i.e. the raw capability / signature data).
fn find_custom_section<'a>(wasm: &'a [u8], name: &str) -> Option<(usize, usize)> {
    if wasm.len() < 8 {
        return None;
    }
    let mut pos = 8; // skip magic + version
    while pos < wasm.len() {
        if pos >= wasm.len() {
            break;
        }
        let section_id = wasm[pos];
        pos += 1;
        let (sec_size, new_pos) = read_leb128(wasm, pos);
        pos = new_pos;
        let sec_start = pos;
        let sec_end = sec_start + sec_size as usize;
        if sec_end > wasm.len() {
            break;
        }
        if section_id == 0 {
            // Custom section: read name
            let (name_len, npos) = read_leb128(wasm, sec_start);
            let name_end = npos + name_len as usize;
            if name_end <= sec_end {
                let name_bytes = &wasm[npos..name_end];
                if name_bytes == name.as_bytes() {
                    let payload_start = name_end;
                    let payload_len = sec_end - payload_start;
                    return Some((payload_start, payload_len));
                }
            }
        }
        pos = sec_end;
    }
    None
}

// ── Custom section builder ────────────────────────────────────────────── //

/// Build a complete WASM custom section (section_id=0, LEB128 size, name, payload).
fn make_custom_section(name: &str, payload: &[u8]) -> Vec<u8> {
    let name_bytes = name.as_bytes();
    let name_leb = write_leb128(name_bytes.len() as u32);
    // content = name_leb + name_bytes + payload
    let content_len = name_leb.len() + name_bytes.len() + payload.len();
    let size_leb = write_leb128(content_len as u32);

    let mut out = Vec::with_capacity(1 + size_leb.len() + content_len);
    out.push(0x00); // section id = custom
    out.extend_from_slice(&size_leb);
    out.extend_from_slice(&name_leb);
    out.extend_from_slice(name_bytes);
    out.extend_from_slice(payload);
    out
}

// ── Core sign / verify logic ──────────────────────────────────────────── //

/// Sign a WASM binary: locate `agentos.capabilities`, compute SHA-512,
/// and append an `agentos.signature` custom section.
fn sign_wasm(input: &[u8], key_id: &[u8; 8]) -> Result<Vec<u8>> {
    // Verify WASM magic
    if input.len() < 4 || &input[..4] != b"\0asm" {
        bail!("not a valid WASM file (bad magic)");
    }

    // Find capabilities section
    let (caps_off, caps_len) = find_custom_section(input, "agentos.capabilities")
        .ok_or_else(|| anyhow!("no agentos.capabilities section found"))?;
    let caps_data = &input[caps_off..caps_off + caps_len];

    // Compute SHA-512
    let mut hasher = Sha512::new();
    hasher.update(caps_data);
    let hash = hasher.finalize();

    // Build 64-byte signature payload: key_id[8] + hash[0..32] + zeros[24]
    let mut sig_payload = [0u8; 64];
    sig_payload[..8].copy_from_slice(key_id);
    sig_payload[8..40].copy_from_slice(&hash[..32]);
    // bytes 40..64 remain zero

    // Append signature section
    let sig_section = make_custom_section("agentos.signature", &sig_payload);
    let mut signed = input.to_vec();
    signed.extend_from_slice(&sig_section);

    println!("Signed successfully");
    println!("  Key ID:    {}", hex::encode(key_id));
    println!("  Hash:      {}", hex::encode(&hash[..32]));
    println!("  Caps size: {} bytes", caps_len);

    Ok(signed)
}

/// Verify the `agentos.signature` section in a WASM binary.
/// Returns `true` if the signature is valid.
fn verify_wasm(input: &[u8]) -> Result<bool> {
    let caps = find_custom_section(input, "agentos.capabilities");
    let sig = find_custom_section(input, "agentos.signature");

    let (caps_off, caps_len) = caps
        .ok_or_else(|| anyhow!("no agentos.capabilities section found"))?;
    let (sig_off, sig_len) = sig
        .ok_or_else(|| anyhow!("no agentos.signature section found (unsigned module)"))?;

    if sig_len != 64 {
        bail!("bad signature size: {} (expected 64)", sig_len);
    }

    let caps_data = &input[caps_off..caps_off + caps_len];
    let sig_data = &input[sig_off..sig_off + sig_len];

    let key_id = &sig_data[..8];
    let stored_hash = &sig_data[8..40];
    let reserved = &sig_data[40..64];

    let mut hasher = Sha512::new();
    hasher.update(caps_data);
    let computed = hasher.finalize();
    let computed_trunc = &computed[..32];

    println!("Key ID:        {}", hex::encode(key_id));
    println!("Stored hash:   {}", hex::encode(stored_hash));
    println!("Computed hash: {}", hex::encode(computed_trunc));
    println!("Reserved zero: {}", reserved == [0u8; 24].as_ref());

    let valid = stored_hash == computed_trunc && reserved == [0u8; 24].as_ref();
    if valid {
        println!("Signature VALID");
    } else {
        println!("Signature INVALID");
    }
    Ok(valid)
}

// ── CLI ───────────────────────────────────────────────────────────────── //

#[derive(Parser, Debug)]
#[command(name = "sign-wasm", about = "Sign agentOS WASM modules")]
struct Cli {
    /// Input .wasm file
    input: PathBuf,

    /// Output signed .wasm file
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// 8-byte key ID in hex (16 hex chars)
    #[arg(long, default_value = "0000000000000001")]
    key_id: String,

    /// Verify existing signature instead of signing
    #[arg(long)]
    verify: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    let input = std::fs::read(&cli.input)
        .with_context(|| format!("reading {}", cli.input.display()))?;

    if cli.verify {
        let ok = verify_wasm(&input)?;
        std::process::exit(if ok { 0 } else { 1 });
    }

    // Parse key_id
    let key_id_bytes = hex::decode(&cli.key_id)
        .context("key_id must be valid hex")?;
    if key_id_bytes.len() != 8 {
        bail!("key_id must be 8 bytes (16 hex chars), got {}", key_id_bytes.len());
    }
    let mut key_id = [0u8; 8];
    key_id.copy_from_slice(&key_id_bytes);

    let signed = sign_wasm(&input, &key_id)?;

    let output = cli.output.unwrap_or_else(|| {
        let s = cli.input.to_string_lossy();
        let base = s.strip_suffix(".wasm").unwrap_or(&s);
        PathBuf::from(format!("{}.signed.wasm", base))
    });

    std::fs::write(&output, &signed)
        .with_context(|| format!("writing {}", output.display()))?;
    println!("Output: {}", output.display());

    Ok(())
}

// ── Tests ─────────────────────────────────────────────────────────────── //

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_leb128_roundtrip() {
        for &v in &[0u32, 127, 128, 300, 16384] {
            let encoded = write_leb128(v);
            let (decoded, _) = read_leb128(&encoded, 0);
            assert_eq!(decoded, v, "roundtrip failed for {}", v);
        }
    }

    #[test]
    fn test_make_custom_section() {
        let sec = make_custom_section("test.section", b"hello");
        // First byte must be section id 0 (custom)
        assert_eq!(sec[0], 0x00, "section id must be 0x00");
        // Decode the size field and ensure total length is consistent
        let (sec_size, payload_start) = read_leb128(&sec, 1);
        assert_eq!(
            payload_start + sec_size as usize,
            sec.len(),
            "section length mismatch"
        );
    }

    /// Build a minimal WASM binary that has a `agentos.capabilities` section.
    fn make_test_wasm(caps_payload: &[u8]) -> Vec<u8> {
        let mut wasm = Vec::new();
        // WASM magic + version
        wasm.extend_from_slice(b"\0asm");
        wasm.extend_from_slice(&[1u8, 0, 0, 0]);
        // Append a custom section named "agentos.capabilities"
        let sec = make_custom_section("agentos.capabilities", caps_payload);
        wasm.extend_from_slice(&sec);
        wasm
    }

    #[test]
    fn test_sign_and_verify() {
        let caps_data = b"read:net.http write:fs.tmp";
        let wasm = make_test_wasm(caps_data);

        let key_id = [0u8, 0, 0, 0, 0, 0, 0, 1];
        let signed = sign_wasm(&wasm, &key_id).expect("sign_wasm failed");

        let result = verify_wasm(&signed).expect("verify_wasm failed");
        assert!(result, "signature should be valid after sign+verify");
    }
}
