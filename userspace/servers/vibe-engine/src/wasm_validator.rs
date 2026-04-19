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

const REQUIRED_EXPORTS: &[&str] = &["init", "handle_ppc", "health_check", "notified", "memory"];

// WASM section IDs
const SECTION_IMPORT: u8 = 2;
const SECTION_EXPORT: u8 = 7;
const SECTION_MEMORY: u8 = 5;
const SECTION_CUSTOM: u8 = 0;

/// Name of the required agentOS capabilities custom section.
const AGENTOS_CAPS_SECTION: &str = "agentos.capabilities";

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

/// Focused validation report returned by `validate_wasm_report`.
/// Callers that only need pass/fail + specific gaps can use this
/// instead of the full `ValidationResult`.
pub struct ValidationReport {
    /// Whether the binary is considered valid for hot-swap.
    pub valid: bool,
    /// Names of required exports that are absent.
    pub missing_exports: Vec<String>,
    /// Non-fatal advisory messages.
    pub warnings: Vec<String>,
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

// ── Memory section check ──────────────────────────────────────────────────────

/// Return `true` if the binary contains a WASM linear memory section (id 5).
pub fn has_memory_section(wasm: &[u8]) -> bool {
    find_section(wasm, SECTION_MEMORY).is_some()
}

// ── Custom section scanner ────────────────────────────────────────────────────

/// Find a WASM custom section (id 0) with a given name.
///
/// Returns `Some(payload_bytes)` where the payload starts after the name
/// inside the section body.  Returns `None` if not found or malformed.
pub fn find_wasm_custom_section<'a>(wasm: &'a [u8], name: &str) -> Option<&'a [u8]> {
    if wasm.len() < 8 {
        return None;
    }
    let mut pos = 8usize; // skip magic + version

    while pos < wasm.len() {
        let section_id = *wasm.get(pos)?;
        pos += 1;

        let (section_size, size_bytes) = read_leb128(wasm, pos)?;
        pos += size_bytes;

        let section_end = pos.checked_add(section_size as usize)?;
        if section_end > wasm.len() {
            return None;
        }

        if section_id == SECTION_CUSTOM {
            // Parse the name prefix inside the section body
            let body = &wasm[pos..section_end];
            let (name_len, name_leb) = read_leb128(body, 0)?;
            let name_start = name_leb;
            let name_end_idx = name_start.checked_add(name_len as usize)?;
            if name_end_idx > body.len() {
                pos = section_end;
                continue;
            }
            let section_name = core::str::from_utf8(&body[name_start..name_end_idx]).ok()?;
            if section_name == name {
                return Some(&body[name_end_idx..]);
            }
        }

        pos = section_end;
    }

    None
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

    // 7. Check that a linear memory section is declared
    if !has_memory_section(bytes) {
        errors.push("No WASM memory section found; linear memory is required for agentOS services".to_string());
    }

    // 8. Check for the agentos.capabilities custom section
    if find_wasm_custom_section(bytes, AGENTOS_CAPS_SECTION).is_none() {
        warnings.push(format!(
            "Missing custom section \"{}\"; service will run without a declared capability manifest (legacy mode)",
            AGENTOS_CAPS_SECTION
        ));
    }

    // 9. Check for "aos" module imports
    let has_aos_import = imports.iter().any(|i| i.starts_with("aos."));
    if !has_aos_import {
        warnings.push("No imports from \"aos\" module; service may not integrate with agentOS".to_string());
    }

    // 10. Warn on unknown import modules (not "aos" or "env")
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

/// Validate a WASM binary and return a focused `ValidationReport`.
///
/// This is the preferred entry point for the hot-swap pipeline — it surfaces
/// `missing_exports` explicitly so callers can give agents actionable feedback.
pub fn validate_wasm_report(bytes: &[u8]) -> ValidationReport {
    let result = validate_wasm(bytes);

    // Extract missing export names from error strings for structured access.
    let missing_exports: Vec<String> = result
        .errors
        .iter()
        .filter_map(|e| {
            let prefix = "Missing required export: \"";
            if e.starts_with(prefix) {
                e[prefix.len()..].strip_suffix('"').map(|s| s.to_string())
            } else {
                None
            }
        })
        .collect();

    ValidationReport {
        valid: result.valid,
        missing_exports,
        warnings: result.warnings,
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

    // ── Additional coverage ───────────────────────────────────────────────────

    #[test]
    fn test_is_wasm_empty_slice() {
        assert!(!is_wasm(&[]));
        assert!(!is_wasm(&[0x00]));
        assert!(!is_wasm(&[0x00, 0x61, 0x73])); // only 3 bytes
    }

    #[test]
    fn test_invalid_magic_bytes() {
        // Wrong magic — should fail with bad-magic error
        let bad: &[u8] = &[
            0xFF, 0xFF, 0xFF, 0xFF, // not \0asm
            0x01, 0x00, 0x00, 0x00,
        ];
        let result = validate_wasm(bad);
        assert!(!result.valid);
        assert!(
            result.errors.iter().any(|e| e.to_lowercase().contains("magic")),
            "expected magic error, got: {:?}", result.errors
        );
    }

    #[test]
    fn test_invalid_version() {
        // Valid magic, wrong version (version 2)
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D, // magic: \0asm
            0x02, 0x00, 0x00, 0x00, // version: 2 (invalid)
        ];
        let result = validate_wasm(wasm);
        // Version error present
        assert!(
            result.errors.iter().any(|e| e.contains("version") || e.contains("Version")),
            "expected version error, got: {:?}", result.errors
        );
    }

    #[test]
    fn test_too_large_binary() {
        // Build a buffer just over MAX_WASM_SIZE
        let mut big = alloc::vec![0u8; MAX_WASM_SIZE + 1];
        // Write valid magic so we pass the magic check and hit the size check
        big[0] = 0x00; big[1] = 0x61; big[2] = 0x73; big[3] = 0x6D;
        big[4] = 0x01; big[5] = 0x00; big[6] = 0x00; big[7] = 0x00;
        let result = validate_wasm(&big);
        assert!(
            result.errors.iter().any(|e| e.to_lowercase().contains("large")),
            "expected too-large error, got: {:?}", result.errors
        );
    }

    #[test]
    fn test_has_memory_section_on_minimal_binary() {
        // 8-byte binary with no sections → no memory section
        let minimal: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D,
            0x01, 0x00, 0x00, 0x00,
        ];
        assert!(!has_memory_section(minimal));
    }

    #[test]
    fn test_has_memory_section_too_small_binary() {
        let short = b"\x00asm";
        assert!(!has_memory_section(short));
    }

    #[test]
    fn test_find_wasm_custom_section_not_found() {
        let minimal: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D,
            0x01, 0x00, 0x00, 0x00,
        ];
        assert!(find_wasm_custom_section(minimal, "agentos.capabilities").is_none());
    }

    #[test]
    fn test_find_wasm_custom_section_too_short() {
        assert!(find_wasm_custom_section(b"\x00asm", "anything").is_none());
        assert!(find_wasm_custom_section(&[], "anything").is_none());
    }

    #[test]
    fn test_validate_wasm_report_missing_exports_list() {
        // Minimal binary: magic + version, no exports
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D,
            0x01, 0x00, 0x00, 0x00,
        ];
        let report = validate_wasm_report(wasm);
        assert!(!report.valid);
        // All required function exports (except "memory") should be listed
        for export in &["init", "handle_ppc", "health_check", "notified"] {
            assert!(
                report.missing_exports.iter().any(|e| e == export),
                "expected '{}' in missing_exports: {:?}", export, report.missing_exports
            );
        }
    }

    #[test]
    fn test_validate_wasm_report_memory_goes_to_warnings_not_missing() {
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D,
            0x01, 0x00, 0x00, 0x00,
        ];
        let report = validate_wasm_report(wasm);
        // "memory" is a warning, not a missing export
        assert!(
            !report.missing_exports.iter().any(|e| e == "memory"),
            "\"memory\" should not be in missing_exports (it's a warning)"
        );
    }

    #[test]
    fn test_validate_wasm_warnings_for_minimal_binary() {
        // A valid-magic binary with no imports/exports/custom sections
        let wasm: &[u8] = &[
            0x00, 0x61, 0x73, 0x6D,
            0x01, 0x00, 0x00, 0x00,
        ];
        let result = validate_wasm(wasm);
        // Should warn about missing "agentos.capabilities" custom section
        assert!(
            result.warnings.iter().any(|w| w.contains("agentos.capabilities")),
            "expected capabilities section warning, got: {:?}", result.warnings
        );
        // Should warn about missing "aos" imports
        assert!(
            result.warnings.iter().any(|w| w.contains("aos")),
            "expected aos import warning, got: {:?}", result.warnings
        );
    }
}
