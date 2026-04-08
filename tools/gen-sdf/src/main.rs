use anyhow::{Context, Result};
use clap::Parser;
use serde::Deserialize;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

// ── Constants ──────────────────────────────────────────────────────────── //

const MICROKIT_MAX_CHANNEL_ID: u32 = 62;
const MICROKIT_MAX_PDS: usize = 63;
const MICROKIT_MAX_MRS: usize = 64;
const MICROKIT_MAX_CHANNELS: usize = 128;
const VALID_PERMS: &[&str] = &["r", "w", "x", "rw", "rx", "rwx", "rws"];

// ── Serde structs ─────────────────────────────────────────────────────── //

#[derive(Deserialize, Clone)]
struct Topology {
    memory_regions: Option<Vec<MemoryRegion>>,
    pds: Option<Vec<Pd>>,
    channels: Option<Vec<Channel>>,
}

#[derive(Deserialize, Clone)]
struct MemoryRegion {
    name: String,
    // serde_yaml handles both "0x1000" strings and integers as Value
    size: serde_yaml::Value,
    perms: Option<String>,
    page_size: Option<serde_yaml::Value>,
}

#[derive(Deserialize, Clone)]
struct Pd {
    name: String,
    priority: Option<serde_yaml::Value>,
    passive: Option<bool>,
    program_image: Option<String>,
    maps: Option<Vec<Map>>,
}

#[derive(Deserialize, Clone)]
struct Map {
    mr: String,
    vaddr: serde_yaml::Value,
    perms: Option<String>,
    cached: Option<bool>,
    setvar_vaddr: Option<String>,
}

#[derive(Deserialize, Clone)]
struct Channel {
    name: Option<String>,
    pd_a: Option<String>,
    id_a: Option<serde_yaml::Value>, // int or "auto"
    pp_a: Option<bool>,
    pd_b: Option<String>,
    id_b: Option<serde_yaml::Value>,
    pp_b: Option<bool>,
}

// ── Helpers ───────────────────────────────────────────────────────────── //

fn to_hex(v: u64) -> String {
    format!("0x{:x}", v)
}

/// Parse a serde_yaml::Value as a u64 (handles both integer and hex-string forms).
fn value_to_u64(v: &serde_yaml::Value) -> Option<u64> {
    match v {
        serde_yaml::Value::Number(n) => n.as_u64(),
        serde_yaml::Value::String(s) => {
            let s = s.trim();
            if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
                u64::from_str_radix(hex, 16).ok()
            } else {
                s.parse::<u64>().ok()
            }
        }
        _ => None,
    }
}

/// Return the hex string for a size Value ("0x1000" or 4096 both → "0x1000").
fn size_hex(v: &serde_yaml::Value) -> String {
    if let Some(n) = value_to_u64(v) {
        to_hex(n)
    } else {
        // fallback: emit as-is
        match v {
            serde_yaml::Value::String(s) => s.clone(),
            _ => format!("{:?}", v),
        }
    }
}

/// Return the channel id as u32 if the value is an integer (not "auto").
fn channel_id_as_int(v: &serde_yaml::Value) -> Option<u32> {
    match v {
        serde_yaml::Value::Number(n) => n.as_u64().map(|x| x as u32),
        serde_yaml::Value::String(s) if s == "auto" => None,
        _ => None,
    }
}

// ── Validate ─────────────────────────────────────────────────────────── //

fn validate(topo: &Topology) -> Vec<String> {
    let mut errors: Vec<String> = Vec::new();

    let pds = topo.pds.as_deref().unwrap_or(&[]);
    let mrs = topo.memory_regions.as_deref().unwrap_or(&[]);
    let channels = topo.channels.as_deref().unwrap_or(&[]);

    // ── PD names unique ───────────────────────────────────────────────── //
    if pds.len() > MICROKIT_MAX_PDS {
        errors.push(format!(
            "Too many PDs: {} (Microkit limit {})",
            pds.len(),
            MICROKIT_MAX_PDS
        ));
    }
    let mut pd_names: HashSet<String> = HashSet::new();
    for pd in pds {
        if pd.name.is_empty() {
            errors.push("PD missing 'name'".to_string());
            continue;
        }
        if pd_names.contains(&pd.name) {
            errors.push(format!("Duplicate PD name: '{}'", pd.name));
        }
        pd_names.insert(pd.name.clone());

        match &pd.priority {
            None => errors.push(format!("PD '{}' missing 'priority'", pd.name)),
            Some(pv) => {
                let valid = match pv {
                    serde_yaml::Value::Number(n) => n
                        .as_i64()
                        .map(|i| i >= 0 && i <= 254)
                        .unwrap_or(false),
                    _ => false,
                };
                if !valid {
                    errors.push(format!(
                        "PD '{}' priority {:?} out of range [0,254]",
                        pd.name, pv
                    ));
                }
            }
        }
    }

    // ── MR names unique ───────────────────────────────────────────────── //
    if mrs.len() > MICROKIT_MAX_MRS {
        errors.push(format!(
            "Too many memory_regions: {} (Microkit limit {})",
            mrs.len(),
            MICROKIT_MAX_MRS
        ));
    }
    let mut mr_names: HashSet<String> = HashSet::new();
    for mr in mrs {
        if mr.name.is_empty() {
            errors.push("memory_region missing 'name'".to_string());
            continue;
        }
        if mr_names.contains(&mr.name) {
            errors.push(format!("Duplicate memory_region name: '{}'", mr.name));
        }
        mr_names.insert(mr.name.clone());

        if matches!(mr.size, serde_yaml::Value::Null) {
            errors.push(format!("MR '{}' missing 'size'", mr.name));
        }
    }

    // ── Map references ────────────────────────────────────────────────── //
    for pd in pds {
        let pnm = &pd.name;
        for mp in pd.maps.as_deref().unwrap_or(&[]) {
            if !mr_names.contains(&mp.mr) {
                errors.push(format!("PD '{}' maps unknown MR '{}'", pnm, mp.mr));
            }
            let p = mp.perms.as_deref().unwrap_or("r");
            if !VALID_PERMS.contains(&p) {
                errors.push(format!("PD '{}' map has invalid perms '{}'", pnm, p));
            }
        }
    }

    // ── Channel validation ────────────────────────────────────────────── //
    if channels.len() > MICROKIT_MAX_CHANNELS {
        errors.push(format!(
            "Too many channels: {} (Microkit limit {})",
            channels.len(),
            MICROKIT_MAX_CHANNELS
        ));
    }

    // Track (pd_name, channel_id) -> channel_name
    let mut pd_ch_map: HashMap<(String, u32), String> = HashMap::new();

    for ch in channels {
        let cname = ch.name.as_deref().unwrap_or("<unnamed>").to_string();
        for side in ["a", "b"] {
            let pd_key = format!("pd_{}", side);
            let id_key = format!("id_{}", side);
            let pnm = match side {
                "a" => ch.pd_a.as_deref(),
                "b" => ch.pd_b.as_deref(),
                _ => unreachable!(),
            };
            let cid_val = match side {
                "a" => ch.id_a.as_ref(),
                "b" => ch.id_b.as_ref(),
                _ => unreachable!(),
            };

            match pnm {
                None | Some("") => {
                    errors.push(format!("Channel '{}' missing '{}'", cname, pd_key));
                    continue;
                }
                Some(p) => {
                    if !pd_names.contains(p) {
                        errors.push(format!(
                            "Channel '{}' references unknown PD '{}'",
                            cname, p
                        ));
                    }
                }
            }

            match cid_val {
                None => {
                    errors.push(format!("Channel '{}' missing '{}'", cname, id_key));
                    continue;
                }
                Some(v) => {
                    // Skip "auto" — not assigned yet, no range check
                    if let serde_yaml::Value::String(s) = v {
                        if s == "auto" {
                            continue;
                        }
                    }
                    let cid = channel_id_as_int(v);
                    match cid {
                        None => {
                            errors.push(format!(
                                "Channel '{}' PD '{}' id {:?} out of range [0,{}]",
                                cname,
                                pnm.unwrap_or("?"),
                                v,
                                MICROKIT_MAX_CHANNEL_ID
                            ));
                        }
                        Some(id) => {
                            if id > MICROKIT_MAX_CHANNEL_ID {
                                errors.push(format!(
                                    "Channel '{}' PD '{}' id {} out of range [0,{}]",
                                    cname,
                                    pnm.unwrap_or("?"),
                                    id,
                                    MICROKIT_MAX_CHANNEL_ID
                                ));
                            }
                            let key = (pnm.unwrap_or("").to_string(), id);
                            if let Some(existing) = pd_ch_map.get(&key) {
                                errors.push(format!(
                                    "Channel ID collision: PD '{}' id={} used by both '{}' and '{}'",
                                    pnm.unwrap_or("?"),
                                    id,
                                    cname,
                                    existing
                                ));
                            } else {
                                pd_ch_map.insert(key, cname.clone());
                            }
                        }
                    }
                }
            }
        }
    }

    errors
}

// ── emit_sdf ─────────────────────────────────────────────────────────── //

fn emit_sdf(topo: &Topology) -> String {
    let mut lines: Vec<String> = Vec::new();
    lines.push(r#"<?xml version="1.0" encoding="UTF-8"?>"#.to_string());

    let chans = topo.channels.as_deref().unwrap_or(&[]);
    let pds = topo.pds.as_deref().unwrap_or(&[]);
    let mrs = topo.memory_regions.as_deref().unwrap_or(&[]);

    // Header comment with channel ID table
    lines.push("<!--".to_string());
    lines.push("  Generated by gen_sdf.py - agentOS Microkit SDF".to_string());
    lines.push(format!(
        "  PDs: {}, Channels: {}, MRs: {}",
        pds.len(),
        chans.len(),
        mrs.len()
    ));
    lines.push(String::new());
    lines.push("  Channel ID allocation table:".to_string());
    lines.push(format!(
        "  {:<40} {:<18} {:>4}  {:<18} {:>4}",
        "Channel name", "PD A", "id_A", "PD B", "id_B"
    ));
    lines.push(format!("  {}", "=".repeat(90)));

    // Sort channels by (id_a, name) — matching Python sort
    let mut sorted_chans: Vec<&Channel> = chans.iter().collect();
    sorted_chans.sort_by(|a, b| {
        let id_a_a = a
            .id_a
            .as_ref()
            .and_then(channel_id_as_int)
            .unwrap_or(999);
        let id_a_b = b
            .id_a
            .as_ref()
            .and_then(channel_id_as_int)
            .unwrap_or(999);
        let name_a = a.name.as_deref().unwrap_or("");
        let name_b = b.name.as_deref().unwrap_or("");
        id_a_a.cmp(&id_a_b).then(name_a.cmp(name_b))
    });

    for ch in &sorted_chans {
        let cname = ch.name.as_deref().unwrap_or("");
        let pd_a = ch.pd_a.as_deref().unwrap_or("");
        let pd_b = ch.pd_b.as_deref().unwrap_or("");
        let id_a_str = ch
            .id_a
            .as_ref()
            .map(|v| match v {
                serde_yaml::Value::Number(n) => n.as_u64().unwrap_or(0).to_string(),
                serde_yaml::Value::String(s) => s.clone(),
                _ => String::new(),
            })
            .unwrap_or_default();
        let id_b_str = ch
            .id_b
            .as_ref()
            .map(|v| match v {
                serde_yaml::Value::Number(n) => n.as_u64().unwrap_or(0).to_string(),
                serde_yaml::Value::String(s) => s.clone(),
                _ => String::new(),
            })
            .unwrap_or_default();
        lines.push(format!(
            "  {:<40} {:<18} {:>4}  {:<18} {:>4}",
            cname, pd_a, id_a_str, pd_b, id_b_str
        ));
    }
    lines.push("-->".to_string());
    lines.push("<system>".to_string());

    // Memory Regions
    if !mrs.is_empty() {
        lines.push(String::new());
        lines.push("    <!-- Memory Regions -->".to_string());
    }
    for mr in mrs {
        let mut attrs = vec![
            format!(r#"name="{}""#, mr.name),
            format!(r#"size="{}""#, size_hex(&mr.size)),
        ];
        if mr.perms.is_some() {
            let ps = mr
                .page_size
                .as_ref()
                .map(|v| size_hex(v))
                .unwrap_or_else(|| to_hex(0x1000));
            attrs.push(format!(r#"page_size="{}""#, ps));
        }
        lines.push(format!("    <memory_region {} />", attrs.join(" ")));
    }

    // Protection Domains
    lines.push(String::new());
    lines.push("    <!-- Protection Domains -->".to_string());
    for pd in pds {
        let passive = if pd.passive.unwrap_or(false) {
            r#" passive="true""#
        } else {
            ""
        };
        let prio_str = pd
            .priority
            .as_ref()
            .map(|v| match v {
                serde_yaml::Value::Number(n) => n.as_i64().unwrap_or(0).to_string(),
                serde_yaml::Value::String(s) => s.clone(),
                _ => String::new(),
            })
            .unwrap_or_default();
        lines.push(format!(
            r#"    <protection_domain name="{}" priority="{}"{}>"#,
            pd.name, prio_str, passive
        ));
        if let Some(img) = &pd.program_image {
            lines.push(format!(r#"        <program_image path="{}" />"#, img));
        }
        for mp in pd.maps.as_deref().unwrap_or(&[]) {
            let vaddr_str = size_hex(&mp.vaddr);
            let mut mp_attrs = vec![
                format!(r#"mr="{}""#, mp.mr),
                format!(r#"vaddr="{}""#, vaddr_str),
                format!(r#"perms="{}""#, mp.perms.as_deref().unwrap_or("r")),
            ];
            if let Some(c) = mp.cached {
                mp_attrs.push(format!(r#"cached="{}""#, c));
            }
            if let Some(sv) = &mp.setvar_vaddr {
                mp_attrs.push(format!(r#"setvar_vaddr="{}""#, sv));
            }
            lines.push(format!("        <map {} />", mp_attrs.join(" ")));
        }
        lines.push("    </protection_domain>".to_string());
        lines.push(String::new());
    }

    // Channels
    lines.push("    <!-- Channels -->".to_string());
    for ch in chans {
        if let Some(cname) = &ch.name {
            if !cname.is_empty() {
                lines.push(format!("    <!-- {} -->", cname));
            }
        }
        lines.push("    <channel>".to_string());
        for side in ["a", "b"] {
            let pnm = match side {
                "a" => ch.pd_a.as_deref().unwrap_or(""),
                "b" => ch.pd_b.as_deref().unwrap_or(""),
                _ => unreachable!(),
            };
            let cid_val = match side {
                "a" => ch.id_a.as_ref(),
                "b" => ch.id_b.as_ref(),
                _ => unreachable!(),
            };
            let pp = match side {
                "a" => ch.pp_a.unwrap_or(false),
                "b" => ch.pp_b.unwrap_or(false),
                _ => unreachable!(),
            };
            let cid_str = cid_val
                .map(|v| match v {
                    serde_yaml::Value::Number(n) => n.as_u64().unwrap_or(0).to_string(),
                    serde_yaml::Value::String(s) => s.clone(),
                    _ => "0".to_string(),
                })
                .unwrap_or_else(|| "0".to_string());
            let pp_attr = if pp { r#" pp="true""# } else { "" };
            lines.push(format!(
                r#"        <end pd="{}" id="{}"{} />"#,
                pnm, cid_str, pp_attr
            ));
        }
        lines.push("    </channel>".to_string());
        lines.push(String::new());
    }

    lines.push("</system>".to_string());
    lines.join("\n")
}

// ── alloc_channel_ids ────────────────────────────────────────────────── //

fn alloc_channel_ids(topo: &mut Topology) {
    let mut used: HashSet<(String, u32)> = HashSet::new();

    // First pass: collect manually assigned IDs
    if let Some(channels) = &topo.channels {
        for ch in channels {
            for (pd_opt, id_opt) in [
                (ch.pd_a.as_deref(), ch.id_a.as_ref()),
                (ch.pd_b.as_deref(), ch.id_b.as_ref()),
            ] {
                if let (Some(pnm), Some(cid)) = (pd_opt, id_opt) {
                    if let Some(id) = channel_id_as_int(cid) {
                        used.insert((pnm.to_string(), id));
                    }
                }
            }
        }
    }

    // Second pass: assign "auto" IDs
    if let Some(channels) = &mut topo.channels {
        for ch in channels.iter_mut() {
            // Process side a
            {
                let is_auto = matches!(&ch.id_a, Some(serde_yaml::Value::String(s)) if s == "auto");
                if is_auto {
                    let pnm = ch.pd_a.clone().unwrap_or_default();
                    for candidate in 0..=MICROKIT_MAX_CHANNEL_ID {
                        if !used.contains(&(pnm.clone(), candidate)) {
                            used.insert((pnm.clone(), candidate));
                            ch.id_a = Some(serde_yaml::Value::Number(candidate.into()));
                            break;
                        }
                    }
                }
            }
            // Process side b
            {
                let is_auto = matches!(&ch.id_b, Some(serde_yaml::Value::String(s)) if s == "auto");
                if is_auto {
                    let pnm = ch.pd_b.clone().unwrap_or_default();
                    for candidate in 0..=MICROKIT_MAX_CHANNEL_ID {
                        if !used.contains(&(pnm.clone(), candidate)) {
                            used.insert((pnm.clone(), candidate));
                            ch.id_b = Some(serde_yaml::Value::Number(candidate.into()));
                            break;
                        }
                    }
                }
            }
        }
    }
}

// ── CLI ───────────────────────────────────────────────────────────────── //

#[derive(Parser)]
#[command(name = "gen-sdf", about = "agentOS Microkit SDF generator")]
struct Cli {
    /// YAML topology spec
    topology: PathBuf,

    /// Output .system file (default: stdout)
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Only validate, do not emit SDF
    #[arg(long)]
    validate_only: bool,

    /// Auto-assign channel IDs marked as 'auto'
    #[arg(long)]
    auto_ids: bool,
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    let content = std::fs::read_to_string(&cli.topology)
        .with_context(|| format!("error: {}: not found", cli.topology.display()))?;

    let mut topo: Topology = serde_yaml::from_str(&content)
        .with_context(|| "error: YAML parse error")?;

    if cli.auto_ids {
        alloc_channel_ids(&mut topo);
    }

    let errors = validate(&topo);
    if !errors.is_empty() {
        eprintln!("Validation FAILED ({} error(s)):", errors.len());
        for e in &errors {
            eprintln!("  ✗ {}", e);
        }
        std::process::exit(1);
    } else {
        eprintln!(
            "✓ Validation passed: {} PDs, {} channels, {} MRs",
            topo.pds.as_deref().unwrap_or(&[]).len(),
            topo.channels.as_deref().unwrap_or(&[]).len(),
            topo.memory_regions.as_deref().unwrap_or(&[]).len(),
        );
    }

    if cli.validate_only {
        return Ok(());
    }

    let sdf = emit_sdf(&topo);

    if let Some(out) = &cli.output {
        std::fs::write(out, &sdf)
            .with_context(|| format!("error: failed to write {}", out.display()))?;
        eprintln!("✓ Wrote {}", out.display());
    } else {
        println!("{}", sdf);
    }

    Ok(())
}

// ── Tests ─────────────────────────────────────────────────────────────── //

#[cfg(test)]
mod tests {
    use super::*;

    fn make_pd(name: &str, priority: i64) -> Pd {
        Pd {
            name: name.to_string(),
            priority: Some(serde_yaml::Value::Number(priority.into())),
            passive: None,
            program_image: None,
            maps: None,
        }
    }

    fn make_channel(
        name: &str,
        pd_a: &str,
        id_a: u32,
        pd_b: &str,
        id_b: u32,
    ) -> Channel {
        Channel {
            name: Some(name.to_string()),
            pd_a: Some(pd_a.to_string()),
            id_a: Some(serde_yaml::Value::Number(id_a.into())),
            pp_a: None,
            pd_b: Some(pd_b.to_string()),
            id_b: Some(serde_yaml::Value::Number(id_b.into())),
            pp_b: None,
        }
    }

    #[test]
    fn test_validate_empty() {
        let topo = Topology {
            memory_regions: None,
            pds: None,
            channels: None,
        };
        let errors = validate(&topo);
        assert!(errors.is_empty(), "empty topology should be valid, got: {:?}", errors);
    }

    #[test]
    fn test_validate_dup_pd() {
        let topo = Topology {
            memory_regions: None,
            pds: Some(vec![make_pd("worker", 10), make_pd("worker", 20)]),
            channels: None,
        };
        let errors = validate(&topo);
        assert!(
            errors.iter().any(|e| e.contains("Duplicate PD name")),
            "expected duplicate PD error, got: {:?}",
            errors
        );
    }

    #[test]
    fn test_emit_minimal() {
        let topo = Topology {
            memory_regions: None,
            pds: Some(vec![make_pd("myapp", 100)]),
            channels: None,
        };
        let sdf = emit_sdf(&topo);
        assert!(sdf.contains("<system>"), "output should contain <system>");
        assert!(sdf.contains("</system>"), "output should contain </system>");
        assert!(sdf.contains("myapp"), "output should contain PD name");
    }

    #[test]
    fn test_channel_id_collision() {
        let topo = Topology {
            memory_regions: None,
            pds: Some(vec![make_pd("pd1", 10), make_pd("pd2", 20)]),
            channels: Some(vec![
                make_channel("ch_first", "pd1", 0, "pd2", 0),
                make_channel("ch_second", "pd1", 0, "pd2", 1),
            ]),
        };
        let errors = validate(&topo);
        assert!(
            errors.iter().any(|e| e.contains("collision")),
            "expected channel ID collision error, got: {:?}",
            errors
        );
    }

    #[test]
    fn test_auto_ids() {
        let mut topo = Topology {
            memory_regions: None,
            pds: Some(vec![make_pd("pd1", 10), make_pd("pd2", 20)]),
            channels: Some(vec![Channel {
                name: Some("ch_auto".to_string()),
                pd_a: Some("pd1".to_string()),
                id_a: Some(serde_yaml::Value::String("auto".to_string())),
                pp_a: None,
                pd_b: Some("pd2".to_string()),
                id_b: Some(serde_yaml::Value::String("auto".to_string())),
                pp_b: None,
            }]),
        };
        alloc_channel_ids(&mut topo);
        let ch = &topo.channels.as_ref().unwrap()[0];
        let id_a = channel_id_as_int(ch.id_a.as_ref().unwrap());
        let id_b = channel_id_as_int(ch.id_b.as_ref().unwrap());
        assert!(id_a.is_some(), "id_a should be assigned an integer");
        assert!(id_b.is_some(), "id_b should be assigned an integer");
        // ids are for different PDs so they can both be 0
        assert!(
            id_a.unwrap() <= MICROKIT_MAX_CHANNEL_ID,
            "id_a should be in valid range"
        );
        assert!(
            id_b.unwrap() <= MICROKIT_MAX_CHANNEL_ID,
            "id_b should be in valid range"
        );
    }
}
