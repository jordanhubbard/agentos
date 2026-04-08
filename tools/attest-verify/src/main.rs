use anyhow::{bail, Context, Result};
use chrono::{DateTime, TimeZone, Utc};
use clap::Parser;
use ed25519_dalek::{Signature, VerifyingKey};
use std::collections::HashMap;
use std::path::PathBuf;

// ── Capability kind names ─────────────────────────────────────────────── //

fn cap_kind_name(kind: u8) -> &'static str {
    match kind {
        0 => "untyped",
        1 => "endpoint",
        2 => "notification",
        3 => "reply",
        4 => "page",
        5 => "tcb",
        6 => "sched_context",
        7 => "irq_handler",
        8 => "io_port",
        9 => "frame",
        10 => "vspace",
        _ => "unknown",
    }
}

/// Decode capability rights bitmask into a human-readable string.
pub fn decode_rights(rights: u32) -> String {
    let mut parts = Vec::new();
    if rights & 0x01 != 0 { parts.push("read"); }
    if rights & 0x02 != 0 { parts.push("write"); }
    if rights & 0x04 != 0 { parts.push("grant"); }
    if rights & 0x08 != 0 { parts.push("grant_reply"); }
    if parts.is_empty() {
        "none".to_string()
    } else {
        parts.join(",")
    }
}

// ── Data structures ───────────────────────────────────────────────────── //

#[derive(Debug, Clone, PartialEq)]
pub struct CapEntry {
    pub handle: u64,
    pub owner_pd: u64,
    pub granted_to: u64,
    pub cptr: String,
    pub rights: u32,
    pub kind: u8,
    pub badge: String,
    pub revokable: bool,
    pub grant_time: u64,
}

#[derive(Debug, Clone)]
pub struct AttestationRecord {
    pub seq: u64,
    pub timestamp_us: u64,
    pub caps: Vec<CapEntry>,
}

// ── Parser ─────────────────────────────────────────────────────────────── //

/// Parse a tab-delimited attestation body into an `AttestationRecord`.
pub fn parse_attestation(text: &str) -> AttestationRecord {
    let mut seq = 0u64;
    let mut timestamp_us = 0u64;
    let mut caps = Vec::new();

    for line in text.lines() {
        let parts: Vec<&str> = line.split('\t').collect();
        if parts.is_empty() {
            continue;
        }
        match parts[0] {
            "ATTEST" if parts.len() >= 3 => {
                seq = parts[1].parse().unwrap_or(0);
                timestamp_us = parts[2].parse().unwrap_or(0);
            }
            "CAP" if parts.len() >= 10 => {
                // rights and kind may be hex strings (e.g. "0x0f") or decimal
                let parse_u32_hex = |s: &str| -> u32 {
                    let s = s.trim();
                    if let Some(rest) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
                        u32::from_str_radix(rest, 16).unwrap_or(0)
                    } else {
                        s.parse().unwrap_or(0)
                    }
                };
                let parse_u8_hex = |s: &str| -> u8 {
                    let s = s.trim();
                    if let Some(rest) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
                        u8::from_str_radix(rest, 16).unwrap_or(0)
                    } else {
                        s.parse().unwrap_or(0)
                    }
                };
                caps.push(CapEntry {
                    handle:     parts[1].parse().unwrap_or(0),
                    owner_pd:   parts[2].parse().unwrap_or(0),
                    granted_to: parts[3].parse().unwrap_or(0),
                    cptr:       parts[4].to_string(),
                    rights:     parse_u32_hex(parts[5]),
                    kind:       parse_u8_hex(parts[6]),
                    badge:      parts[7].to_string(),
                    revokable:  parts[8] == "1",
                    grant_time: parts[9].parse().unwrap_or(0),
                });
            }
            "END" => {}
            _ => {}
        }
    }

    AttestationRecord { seq, timestamp_us, caps }
}

// ── Signed attestation file format ───────────────────────────────────── //
// Layout: [4-byte big-endian body_len][body_bytes][64-byte Ed25519 sig]

/// Parse a signed attestation binary: returns `(body_bytes, signature)`.
pub fn load_signed_attestation(data: &[u8]) -> Result<(Vec<u8>, [u8; 64])> {
    if data.len() < 68 {
        bail!("too short for signed attestation ({} bytes)", data.len());
    }
    let body_len = u32::from_be_bytes([data[0], data[1], data[2], data[3]]) as usize;
    if 4 + body_len + 64 > data.len() {
        bail!("truncated: claimed body_len={} but file is {} bytes", body_len, data.len());
    }
    let body = data[4..4 + body_len].to_vec();
    let mut sig = [0u8; 64];
    sig.copy_from_slice(&data[4 + body_len..4 + body_len + 64]);
    Ok((body, sig))
}

// ── Ed25519 verification ──────────────────────────────────────────────── //

/// Verify an Ed25519 signature.  Returns `true` if valid.
pub fn verify_ed25519(pubkey_bytes: &[u8; 32], message: &[u8], signature: &[u8; 64]) -> bool {
    let vk = match VerifyingKey::from_bytes(pubkey_bytes) {
        Ok(k) => k,
        Err(_) => return false,
    };
    let sig = Signature::from_bytes(signature);
    vk.verify_strict(message, &sig).is_ok()
}

// ── Report printer ────────────────────────────────────────────────────── //

/// Print a formatted capability attestation report.
pub fn print_report(rec: &AttestationRecord, verified: Option<bool>) {
    let w = 80;
    let sep: String = "=".repeat(w);
    println!("{}", sep);
    println!("  agentOS Capability Attestation Report");
    println!("  Sequence:    {}", rec.seq);

    // Format timestamp
    let ts: DateTime<Utc> = Utc
        .timestamp_opt(
            (rec.timestamp_us / 1_000_000) as i64,
            ((rec.timestamp_us % 1_000_000) * 1_000) as u32,
        )
        .single()
        .unwrap_or(DateTime::UNIX_EPOCH);
    println!("  Timestamp:   {}", ts.to_rfc3339());
    println!("  Active caps: {}", rec.caps.len());
    match verified {
        Some(true)  => println!("  Signature:   VERIFIED"),
        Some(false) => println!("  Signature:   INVALID -- report may be tampered!"),
        None        => println!("  Signature:   ? (public key not provided)"),
    }
    println!("{}", sep);
    println!();

    println!(
        "{:>6}  {:>8}  {:>8}  {:<16}  {:<24}  {:<10}  {}",
        "Handle", "Owner PD", "Granted->", "Kind", "Rights", "cptr", "Rev"
    );
    println!("{}", "-".repeat(w));

    let mut sorted_caps = rec.caps.clone();
    sorted_caps.sort_by_key(|c| c.handle);
    for c in &sorted_caps {
        let granted = if c.granted_to == 0 {
            "--".to_string()
        } else {
            c.granted_to.to_string()
        };
        let kn = cap_kind_name(c.kind);
        let rr = decode_rights(c.rights);
        let rev = if c.revokable { "Y" } else { "-" };
        println!(
            "{:>6}  {:>8}  {:>8}  {:<16}  {:<24}  {:<10}  {}",
            c.handle, c.owner_pd, granted, kn, rr, c.cptr, rev
        );
    }
    println!();
}

// ── Diff ──────────────────────────────────────────────────────────────── //

/// Print added/removed/changed capabilities between two attestation records.
pub fn diff_attestations(prev: &AttestationRecord, curr: &AttestationRecord) {
    let prev_map: HashMap<u64, &CapEntry> = prev.caps.iter().map(|c| (c.handle, c)).collect();
    let curr_map: HashMap<u64, &CapEntry> = curr.caps.iter().map(|c| (c.handle, c)).collect();

    let added: Vec<&CapEntry>   = curr_map.values().filter(|c| !prev_map.contains_key(&c.handle)).copied().collect();
    let removed: Vec<&CapEntry> = prev_map.values().filter(|c| !curr_map.contains_key(&c.handle)).copied().collect();
    let changed: Vec<&CapEntry> = curr_map.values()
        .filter(|c| prev_map.get(&c.handle).map_or(false, |p| *p != **c))
        .copied()
        .collect();

    println!("Diff (seq {} -> {}):", prev.seq, curr.seq);
    if added.is_empty() && removed.is_empty() && changed.is_empty() {
        println!("  No changes.");
        return;
    }
    for c in &added {
        println!("  + handle {}: {} owner={} granted={}", c.handle, cap_kind_name(c.kind), c.owner_pd, c.granted_to);
    }
    for c in &removed {
        println!("  - handle {}: {} owner={}", c.handle, cap_kind_name(c.kind), c.owner_pd);
    }
    for c in &changed {
        println!("  ~ handle {}: modified", c.handle);
    }
    println!();
}

// ── AgentFS fetch ─────────────────────────────────────────────────────── //

fn fetch_latest(agentfs_url: &str, token: Option<&str>) -> Result<Vec<u8>> {
    let client = reqwest::blocking::Client::new();
    let list_url = format!("{}/ls?prefix=agentos/attestation/", agentfs_url);

    let mut req = client.get(&list_url);
    if let Some(tok) = token {
        req = req.header("Authorization", format!("Bearer {}", tok));
    }
    let resp = req.send().with_context(|| format!("GET {}", list_url))?;
    let listing: serde_json::Value = resp.json().context("parsing attestation listing")?;

    let files: Vec<String> = match &listing {
        serde_json::Value::Array(arr) => arr.iter().map(|v| v.to_string()).collect(),
        serde_json::Value::Object(obj) => {
            if let Some(serde_json::Value::Array(arr)) = obj.get("files") {
                arr.iter().map(|v| v.to_string()).collect()
            } else {
                bail!("unexpected attestation listing format: missing 'files' key");
            }
        }
        _ => bail!("unexpected attestation listing format: expected array or object"),
    };

    if files.is_empty() {
        bail!("no attestation files found");
    }
    let latest = files.iter().max()
        .expect("non-empty vec always has a max — checked above");
    let fetch_url = format!("{}/get?hash={}", agentfs_url, latest);

    let mut req2 = client.get(&fetch_url);
    if let Some(tok) = token {
        req2 = req2.header("Authorization", format!("Bearer {}", tok));
    }
    let data = req2.send()
        .with_context(|| format!("GET {}", fetch_url))?
        .bytes()
        .context("reading response body")?;
    Ok(data.to_vec())
}

// ── CLI ───────────────────────────────────────────────────────────────── //

#[derive(Parser, Debug)]
#[command(name = "attest-verify", about = "agentOS capability attestation verifier")]
struct Cli {
    /// Fetch the latest attestation from AgentFS
    #[arg(long)]
    latest: bool,

    /// Load from a local file
    #[arg(long, value_name = "FILE")]
    local: Option<PathBuf>,

    /// AgentFS base URL
    #[arg(long, default_value = "http://127.0.0.1:8791")]
    agentfs: String,

    /// Bearer token for AgentFS
    #[arg(long)]
    token: Option<String>,

    /// Path to raw 32-byte Ed25519 public key
    #[arg(long, default_value = "~/.nanoc/system.pub")]
    pubkey: String,

    /// Diff against a previous attestation file
    #[arg(long, value_name = "PREV_FILE")]
    diff: Option<PathBuf>,

    /// Save fetched attestation to a file
    #[arg(long, value_name = "FILE")]
    save: Option<PathBuf>,

    /// Parse unsigned (plain text) attestation
    #[arg(long)]
    unsigned: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    // Load raw attestation data
    let raw_data: Vec<u8> = if let Some(ref path) = cli.local {
        std::fs::read(path).with_context(|| format!("reading {}", path.display()))?
    } else if cli.latest {
        eprintln!("Fetching latest attestation from {}...", cli.agentfs);
        fetch_latest(&cli.agentfs, cli.token.as_deref())?
    } else {
        anyhow::bail!("provide --latest or --local <file>");
    };

    if let Some(ref save_path) = cli.save {
        std::fs::write(save_path, &raw_data)
            .with_context(|| format!("saving to {}", save_path.display()))?;
        eprintln!("Saved to {}", save_path.display());
    }

    // Parse body
    let (body, sig_opt): (Vec<u8>, Option<[u8; 64]>) = if cli.unsigned {
        (raw_data.clone(), None)
    } else {
        match load_signed_attestation(&raw_data) {
            Ok((b, s)) => (b, Some(s)),
            Err(_) => (raw_data.clone(), None),
        }
    };

    let rec = parse_attestation(&String::from_utf8_lossy(&body));

    // Verify signature
    let verified: Option<bool> = if let Some(sig) = sig_opt {
        // Expand ~ in pubkey path
        let pubkey_path = if cli.pubkey.starts_with('~') {
            let home = std::env::var("HOME").unwrap_or_default();
            cli.pubkey.replacen('~', &home, 1)
        } else {
            cli.pubkey.clone()
        };
        match std::fs::read(&pubkey_path) {
            Ok(pk) if pk.len() == 32 => {
                let mut pk32 = [0u8; 32];
                pk32.copy_from_slice(&pk);
                Some(verify_ed25519(&pk32, &body, &sig))
            }
            Ok(pk) => {
                eprintln!("Warning: pubkey {} is {} bytes (expected 32)", pubkey_path, pk.len());
                None
            }
            Err(_) => None, // key not found — skip verification
        }
    } else {
        None
    };

    print_report(&rec, verified);

    // Diff
    if let Some(ref diff_path) = cli.diff {
        let diff_raw = std::fs::read(diff_path)
            .with_context(|| format!("reading {}", diff_path.display()))?;
        let diff_body = match load_signed_attestation(&diff_raw) {
            Ok((b, _)) => b,
            Err(_) => diff_raw,
        };
        let prev_rec = parse_attestation(&String::from_utf8_lossy(&diff_body));
        diff_attestations(&prev_rec, &rec);
    }

    // Exit code: 1 if signature is explicitly invalid
    if verified == Some(false) {
        eprintln!("ATTESTATION FAILED: signature invalid");
        std::process::exit(1);
    }

    Ok(())
}

// ── Tests ─────────────────────────────────────────────────────────────── //

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "\
ATTEST\t42\t1700000000000000\n\
CAP\t1\t100\t200\t0x1ff\t0x0f\t1\t0x0\t1\t1699990000000000\n\
CAP\t2\t100\t0\t0x200\t0x03\t4\t0x0\t0\t1699991000000000\n\
END\t2\n";

    #[test]
    fn test_parse_attestation() {
        let rec = parse_attestation(SAMPLE);
        assert_eq!(rec.seq, 42);
        assert_eq!(rec.timestamp_us, 1700000000000000);
        assert_eq!(rec.caps.len(), 2);

        let c0 = &rec.caps[0];
        assert_eq!(c0.handle, 1);
        assert_eq!(c0.owner_pd, 100);
        assert_eq!(c0.granted_to, 200);
        assert_eq!(c0.kind, 1);  // endpoint
        assert!(c0.revokable);

        let c1 = &rec.caps[1];
        assert_eq!(c1.handle, 2);
        assert!(!c1.revokable);
    }

    #[test]
    fn test_load_signed() {
        let body = b"hello attestation body";
        let body_len = (body.len() as u32).to_be_bytes();
        let sig = [0u8; 64];
        let mut data = Vec::new();
        data.extend_from_slice(&body_len);
        data.extend_from_slice(body);
        data.extend_from_slice(&sig);

        let (parsed_body, parsed_sig) = load_signed_attestation(&data)
            .expect("well-formed attestation payload should parse");
        assert_eq!(parsed_body, body);
        assert_eq!(parsed_sig, sig);
    }

    #[test]
    fn test_decode_rights() {
        assert_eq!(decode_rights(0x07), "read,write,grant");
        assert_eq!(decode_rights(0x00), "none");
        assert_eq!(decode_rights(0x01), "read");
        assert_eq!(decode_rights(0x0f), "read,write,grant,grant_reply");
    }
}
