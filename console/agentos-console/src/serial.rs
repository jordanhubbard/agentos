//! Serial log file parser + QEMU socket connection.
//!
//! Parses /tmp/agentos-serial.log (QEMU -serial file:...) to reconstruct
//! per-PD output streams.  console_mux tags each line in broadcast mode:
//!   \033[36m[pd_name]\033[0m text\n   (ANSI coloured)
//! Some PDs also use microkit_dbg_puts directly:
//!   [pd_name] text\n
//!
//! We strip ANSI, extract the tag, look up the slot id, and store lines.

use std::collections::HashSet;
use std::io::{Read, Seek, SeekFrom};

use pd_slots::{MAX_SLOTS, name_to_slot};
use tracing::{debug, warn};

/// A log line routed to a specific slot.
#[derive(Debug, Clone)]
pub struct RoutedLine {
    pub slot:     usize,
    pub line:     String,
    pub seen_pd:  bool,
}

/// Per-slot line cache populated from the serial log.
pub struct SerialCache {
    /// slot → Vec of complete lines
    pub lines:    Vec<Vec<String>>,
    /// slot → Set of lines for O(1) duplicate suppression
    pub seen:     Vec<HashSet<String>>,
    /// last byte-offset we read in the log file
    pub offset:   u64,
    /// whether we have ever successfully read the log
    pub ever_read: bool,
}

impl SerialCache {
    pub fn new() -> Self {
        SerialCache {
            lines:     (0..MAX_SLOTS).map(|_| Vec::new()).collect(),
            seen:      (0..MAX_SLOTS).map(|_| HashSet::new()).collect(),
            offset:    0,
            ever_read: false,
        }
    }

    pub fn reset(&mut self) {
        for i in 0..MAX_SLOTS {
            self.lines[i].clear();
            self.seen[i].clear();
        }
        self.offset = 0;
    }

    /// Add a line to the cache, returning true if it was new (not a duplicate).
    pub fn add_line(&mut self, slot: usize, line: String) -> bool {
        if slot >= MAX_SLOTS {
            return false;
        }
        if self.seen[slot].contains(&line) {
            return false;
        }
        self.seen[slot].insert(line.clone());
        self.lines[slot].push(line);
        true
    }
}

impl Default for SerialCache {
    fn default() -> Self {
        Self::new()
    }
}

/// Strip all ANSI escape sequences from a string (char-by-char, handles multi-byte UTF-8).
pub fn strip_ansi_bytes(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\x1b' {
            if chars.peek() == Some(&'[') {
                chars.next(); // consume '['
                // consume until ASCII letter
                for c2 in chars.by_ref() {
                    if c2.is_ascii_alphabetic() {
                        break;
                    }
                }
            }
            // else just drop the bare ESC
        } else {
            out.push(c);
        }
    }
    out
}

/// Route a single raw serial line to a slot.
///
/// Returns `Some(RoutedLine)` if routable, `None` otherwise.
/// Pass `seen_pd = true` if PD-tagged lines have already been observed in
/// this stream (enables linux_vmm fallback for untagged lines).
pub fn route_log_line(raw_line: &str, seen_pd: bool) -> Option<RoutedLine> {
    let clean = strip_ansi_bytes(raw_line).trim().to_string();
    if clean.is_empty() {
        return None;
    }

    // BRACKET_RE: `(?:\x1b\[[0-9;]*m)?\[([a-zA-Z0-9_]+)\](?:\x1b\[[0-9;]*m)?\s*(.*)`
    // We parse this manually for performance.
    let trimmed_raw = raw_line.trim();
    if let Some(bracket_match) = parse_bracket_line(trimmed_raw) {
        let pd = bracket_match.0.to_lowercase();
        let text = strip_ansi_bytes(bracket_match.1).trim_end().to_string();
        if let Some(slot) = name_to_slot(&pd) {
            let normalised_pd: String = pd.chars()
                .map(|c| if c == ' ' || c == '-' { '_' } else { c })
                .collect();
            return Some(RoutedLine {
                slot,
                line: format!("[{}] {}", normalised_pd, text),
                seen_pd: true,
            });
        }
    }

    // PIPE_RE: `^([a-zA-Z0-9_]+)\|(?:INFO|WARN|ERROR|DEBUG):\s*(.*)`
    if let Some(pipe_match) = parse_pipe_line(&clean) {
        let pd = pipe_match.0.to_lowercase();
        let text = pipe_match.1.trim_end().to_string();
        if let Some(slot) = name_to_slot(&pd) {
            return Some(RoutedLine {
                slot,
                line: format!("[{}] {}", pd, text),
                seen_pd: true,
            });
        }
    }

    // Untagged line after PD startup = linux_vmm guest console output
    if seen_pd && !is_sys_prefix(&clean) {
        if let Some(slot) = name_to_slot("linux_vmm") {
            return Some(RoutedLine { slot, line: clean, seen_pd });
        }
    }

    None
}

/// Parse a bracket-tagged line: optional ANSI, `[name]`, optional ANSI, text.
/// Returns (name, rest_of_line) or None.
fn parse_bracket_line(s: &str) -> Option<(&str, &str)> {
    // Skip leading ANSI if present
    let s = skip_ansi_prefix(s);
    if !s.starts_with('[') {
        return None;
    }
    let rest = &s[1..];
    let end = rest.find(']')?;
    let name = &rest[..end];
    // Name must be alphanumeric + underscore only
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        return None;
    }
    let after = &rest[end + 1..];
    // Skip trailing ANSI + whitespace
    let after = skip_ansi_prefix(after.trim_start());
    Some((name, after))
}

/// Skip a single leading ANSI CSI sequence if present.
fn skip_ansi_prefix(s: &str) -> &str {
    let bytes = s.as_bytes();
    if bytes.first() == Some(&0x1b) && bytes.get(1) == Some(&b'[') {
        let mut i = 2;
        while i < bytes.len() && !bytes[i].is_ascii_alphabetic() {
            i += 1;
        }
        if i < bytes.len() {
            return &s[i + 1..];
        }
    }
    s
}

/// Parse a pipe-tagged line: `name|LEVEL: text`.
/// Returns (name, text) or None.
fn parse_pipe_line(s: &str) -> Option<(&str, &str)> {
    let pipe_pos = s.find('|')?;
    let name = &s[..pipe_pos];
    // name must be alphanumeric + underscore
    if !name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_') {
        return None;
    }
    let after = &s[pipe_pos + 1..];
    for level in &["INFO:", "WARN:", "ERROR:", "DEBUG:"] {
        if let Some(rest) = after.strip_prefix(level) {
            return Some((name, rest.trim_start()));
        }
    }
    None
}

/// SYSPREFIX_RE: `^(LDR\||MON\||MICROKIT\||Bootstrapping|seL4 |ELF |CAPDL )`
fn is_sys_prefix(s: &str) -> bool {
    s.starts_with("LDR|")
        || s.starts_with("MON|")
        || s.starts_with("MICROKIT|")
        || s.starts_with("Bootstrapping")
        || s.starts_with("seL4 ")
        || s.starts_with("ELF ")
        || s.starts_with("CAPDL ")
}

/// Incrementally read new bytes from the serial log and parse tagged lines.
///
/// Returns a list of new (slot, line) pairs for broadcasting.
pub fn parse_serial_log(cache: &mut SerialCache, path: &str) -> Vec<(usize, String)> {
    let mut file = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return vec![],
    };

    let file_size = match file.seek(SeekFrom::End(0)) {
        Ok(n) => n,
        Err(_) => return vec![],
    };

    // If the file shrank (QEMU restarted), reset
    if file_size < cache.offset {
        cache.reset();
    }

    if file_size <= cache.offset {
        return vec![];
    }

    if file.seek(SeekFrom::Start(cache.offset)).is_err() {
        return vec![];
    }

    let chunk_size = (file_size - cache.offset) as usize;
    let mut buf = vec![0u8; chunk_size];
    match file.read_exact(&mut buf) {
        Ok(_) => {}
        Err(e) => {
            warn!("serial log read error: {}", e);
            return vec![];
        }
    }
    cache.offset = file_size;
    cache.ever_read = true;

    let text = String::from_utf8_lossy(&buf);
    let raw_lines: Vec<&str> = text.split('\n').collect();

    let mut added = Vec::new();
    // true if we've already seen tagged PD lines (from prior chunks)
    let mut seen_pd = cache.offset > chunk_size as u64;

    for raw_line in &raw_lines {
        if let Some(routed) = route_log_line(raw_line, seen_pd) {
            seen_pd = routed.seen_pd;
            if cache.add_line(routed.slot, routed.line.clone()) {
                added.push((routed.slot, routed.line));
            }
        }
    }

    debug!("parse_serial_log: {} new lines", added.len());
    added
}
