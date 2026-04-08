/// WASM validator for agentOS service compatibility.
/// Port of services/vibe-swap/src/wasm-validator.mjs
///
/// Copyright (c) 2026 The agentOS Project
/// SPDX-License-Identifier: BSD-2-Clause

extern crate alloc;
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use alloc::format;

const WASM_MAGIC: &[u8; 4] = b"\x00asm";
const WASM_VERSION: &[u8; 4] = &[0x01, 0x00, 0x00, 0x00];
const MAX_WASM_SIZE: usize = 1 * 1024 * 1024; // 1 MB

const REQUIRED_EXPORTS: &[&str] = &["init", "handle_ppc", "health_check", "memory"];

// WASM section IDs
const SECTION_IMPORT: u8 = 2;
const SECTION_EXPORT: u8 = 7;

// WASM external kind names
const KIND_NAMES: &[&str] = &["function", "table", "memory", "global"];

// ── Public types ──────────────────────────────────────────────────────────────

pub struct ValidationResult {
    pub valid: bool,
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
    pub exports: Vec<String>, // "name:kind"
    pub imports: Vec<String>, // "module.name:kind"
}

// ── LEB128 helper ─────────────────────────────────────────────────────────────

/// Read an unsigned LEB128-encoded u32 from `data` starting at `pos`.
/// Returns `Some((value, bytes_consumed))` on success, `None` on error/overflow.
fn read_leb128(data: &[u8], pos: usize) -> Option<(u32, usize)> {
    let mut result: u32 = 0;
    let mut shift: u32 = 0;
    let mut offset = pos;

    loop {
        if offset >= data.len() {
            return None;
        }
        let byte = data[offset];
        offset += 1;

        // Guard against overflow: LEB128 u32 can be at most 5 bytes
        if shift >= 35 {
            return None;
        }

        result |= ((byte & 0x7F) as u32) << shift;
        shift += 7;

        if byte & 0x80 == 0 {
            return Some((result, offset - pos));
        }
    }
}

// ── Section scanner ───────────────────────────────────────────────────────────

/// Find a WASM section by id, skipping the 8-byte header (magic + version).
/// Returns `Some((payload_start, payload_len))` if found, `None` otherwise.
fn find_section(wasm: &[u8], target_id: u8) -> Option<(usize, usize)> {
    if wasm.len() < 8 {
        return None;
    }
    let mut pos = 8; // skip magic (4) + version (4)

    while pos < wasm.len() {
        if pos >= wasm.len() {
            break;
        }
        let section_id = wasm[pos];
        pos += 1;

        // Read section size (LEB128)
        let (section_size, size_bytes) = read_leb128(wasm, pos)?;
        pos += size_bytes;

        let payload_start = pos;
        let payload_len = section_size as usize;

        if section_id == target_id {
            return Some((payload_start, payload_len));
        }

        // Skip over this section's payload
        pos = pos.checked_add(payload_len)?;
    }

    None
}

// ── Export section parser ─────────────────────────────────────────────────────

/// Parse the WASM export section and return a Vec of "name:kind" strings.
/// Export entry layout: [name_len LEB128][name bytes][kind byte][index LEB128]
fn parse_exports(wasm: &[u8]) -> Vec<String> {
    let mut result = Vec::new();

    let Some((start, len)) = find_section(wasm, SECTION_EXPORT) else {
        return result;
    };

    let end = start.saturating_add(len);
    if end > wasm.len() {
        return result;
    }

    let payload = &wasm[start..end];
    let mut pos = 0;

    // Read count of exports
    let Some((count, consumed)) = read_leb128(payload, pos) else {
        return result;
    };
    pos += consumed;

    for _ in 0..count {
        // Read name length
        let Some((name_len, consumed)) = read_leb128(payload, pos) else {
            break;
        };
        pos += consumed;

        let name_end = pos + name_len as usize;
        if name_end > payload.len() {
            break;
        }
        let name = match core::str::from_utf8(&payload[pos..name_end]) {
            Ok(s) => s.to_string(),
            Err(_) => break,
        };
        pos = name_end;

        // Read kind byte
        if pos >= payload.len() {
            break;
        }
        let kind_byte = payload[pos];
        pos += 1;

        // Skip index (LEB128)
        let Some((_, consumed)) = read_leb128(payload, pos) else {
            break;
        };
        pos += consumed;

        let kind_name = KIND_NAMES.get(kind_byte as usize).copied().unwrap_or("unknown");
        result.push(format!("{}:{}", name, kind_name));
    }

    result
}

// ── Import section parser ─────────────────────────────────────────────────────

/// Parse the WASM import section and return a Vec of "module.name:kind" strings.
/// Import entry layout: [mod_len LEB128][mod bytes][name_len LEB128][name bytes][kind byte][index/type LEB128]
fn parse_imports(wasm: &[u8]) -> Vec<String> {
    let mut result = Vec::new();

    let Some((start, len)) = find_section(wasm, SECTION_IMPORT) else {
        return result;
    };

    let end = start.saturating_add(len);
    if end > wasm.len() {
        return result;
    }

    let payload = &wasm[start..end];
    let mut pos = 0;

    // Read count of imports
    let Some((count, consumed)) = read_leb128(payload, pos) else {
        return result;
    };
    pos += consumed;

    for _ in 0..count {
        // Read module name length
        let Some((mod_len, consumed)) = read_leb128(payload, pos) else {
            break;
        };
        pos += consumed;

        let mod_end = pos + mod_len as usize;
        if mod_end > payload.len() {
            break;
        }
        let module = match core::str::from_utf8(&payload[pos..mod_end]) {
            Ok(s) => s.to_string(),
            Err(_) => break,
        };
        pos = mod_end;

        // Read import name length
        let Some((name_len, consumed)) = read_leb128(payload, pos) else {
            break;
        };
        pos += consumed;

        let name_end = pos + name_len as usize;
        if name_end > payload.len() {
            break;
        }
        let name = match core::str::from_utf8(&payload[pos..name_end]) {
            Ok(s) => s.to_string(),
            Err(_) => break,
        };
        pos = name_end;

        // Read kind byte
        if pos >= payload.len() {
            break;
        }
        let kind_byte = payload[pos];
        pos += 1;

        // Skip type index or similar (LEB128)
        let Some((_, consumed)) = read_leb128(payload, pos) else {
            break;
        };
        pos += consumed;

        let kind_name = KIND_NAMES.get(kind_byte as usize).copied().unwrap_or("unknown");
        result.push(format!("{}.{}:{}", module, name, kind_name));
    }

    result
}

// ── Public API ────────────────────────────────────────────────────────────────

/// Check whether `bytes` starts with the WASM magic bytes.
pub fn is_wasm(bytes: &[u8]) -> bool {
    bytes.len() >= 4 && &bytes[..4] == WASM_MAGIC
}

/// Validate a WASM binary for agentOS service compatibility.
pub fn validate_wasm(bytes: &[u8]) -> ValidationResult {
    let mut errors: Vec<String> = Vec::new();
    let mut warnings: Vec<String> = Vec::new();

    // 1. Size checks
    if bytes.len() < 8 {
        errors.push(format!(
            "Binary too small ({} bytes); minimum is 8 bytes",
            bytes.len()
        ));
        return ValidationResult {
            valid: false,
            errors,
            warnings,
            exports: Vec::new(),
            imports: Vec::new(),
        };
    }
    if bytes.len() > MAX_WASM_SIZE {
        errors.push(format!(
            "Binary too large ({} bytes); maximum is {} bytes",
            bytes.len(),
            MAX_WASM_SIZE
        ));
    }

    // 2. Magic bytes check
    if &bytes[..4] != WASM_MAGIC {
        errors.push("Invalid WASM magic bytes".to_string());
        return ValidationResult {
            valid: false,
            errors,
            warnings,
            exports: Vec::new(),
            imports: Vec::new(),
        };
    }

    // 3. Version check
    if &bytes[4..8] != WASM_VERSION {
        errors.push(format!(
            "Unsupported WASM version: {:?}; expected {:?}",
            &bytes[4..8],
            WASM_VERSION
        ));
    }

    // 4. Parse exports
    let exports = parse_exports(bytes);

    // 5. Parse imports
    let imports = parse_imports(bytes);

    // 6. Check required exports
    let export_names: Vec<&str> = exports
        .iter()
        .map(|e| e.split(':').next().unwrap_or(""))
        .collect();

    for required in REQUIRED_EXPORTS {
        if !export_names.contains(required) {
            if *required == "memory" {
                warnings.push(format!(
                    "Missing recommended export: \"{}\"; memory may be inaccessible",
                    required
                ));
            } else {
                errors.push(format!("Missing required export: \"{}\"", required));
            }
        }
    }

    // 7. Check for "aos" module imports
    let has_aos_import = imports.iter().any(|i| i.starts_with("aos."));
    if !has_aos_import {
        warnings.push("No imports from \"aos\" module; service may not integrate with agentOS".to_string());
    }

    // 8. Warn on unknown import modules (not "aos" or "env")
    for import in &imports {
        let module = import.split('.').next().unwrap_or("");
        if module != "aos" && module != "env" {
            warnings.push(format!("Unknown import module: \"{}\"", module));
        }
    }

    let valid = errors.is_empty();
    ValidationResult {
        valid,
        errors,
        warnings,
        exports,
        imports,
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_is_wasm_valid() {
        let bytes = b"\x00asm\x01\x00\x00\x00";
        assert!(is_wasm(bytes));
    }

    #[test]
    fn test_is_wasm_invalid() {
        let bytes = b"\xFF\xFE\x00\x00";
        assert!(!is_wasm(bytes));
    }

    #[test]
    fn test_too_small() {
        let bytes = b"\x00asm";
        let result = validate_wasm(bytes);
        assert!(!result.valid);
        assert!(result.errors.iter().any(|e| e.to_lowercase().contains("too small")));
    }

    #[test]
    fn test_valid_magic_missing_exports() {
        // Minimal valid WASM: magic + version, empty export section
        // Section 7 (export), size=1, count=0
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D, // magic: \0asm
            0x01, 0x00, 0x00, 0x00, // version: 1
            0x07,                   // section id: export
            0x01,                   // section size: 1 byte
            0x00,                   // export count: 0
        ];
        let result = validate_wasm(wasm);
        assert!(!result.valid);
        assert!(
            result.errors.iter().any(|e| e.contains("Missing required export: \"init\"")),
            "Expected error about missing 'init' export, got: {:?}",
            result.errors
        );
    }
}
