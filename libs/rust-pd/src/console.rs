//! Logging via the agentOS `console_log` extern.
//!
//! The kernel-side `console_log(level, color, str)` function writes a
//! NUL-terminated string to the Microkit debug console (typically routed to a
//! serial UART or socket visible in QEMU output).
//!
//! This module provides a safe Rust wrapper that:
//! * Accepts a `&str` (no allocation needed — the string is written byte-by-byte
//!   via a small stack buffer with a NUL terminator).
//! * Clips messages that exceed `MAX_MSG_LEN` to avoid stack overflows in PDs
//!   that have constrained stack space.

/// Maximum number of UTF-8 bytes written to the console in a single call
/// (not counting the NUL terminator).
pub const MAX_MSG_LEN: usize = 255;

/// Log a message at `level` with ANSI color index `color`.
///
/// The `level` and `color` values mirror the calling convention used by the
/// C PDs in `kernel/agentos-root-task/src/`:
///
/// | level | meaning        |
/// |-------|----------------|
/// | 0     | CRITICAL / plain |
/// | 1     | ERROR          |
/// | 2     | WARN           |
/// | 3     | INFO           |
/// | 7     | DEBUG          |
///
/// Color is an ANSI 256-color palette index (0 = default).
///
/// The function is safe: it copies `msg` into a stack-allocated `[u8; 257]`
/// buffer (256 bytes + NUL), truncating silently if `msg` is longer than
/// `MAX_MSG_LEN`.
pub fn log(level: u32, color: u32, msg: &str) {
    // Stack buffer: MAX_MSG_LEN content bytes + 1 NUL terminator.
    let mut buf = [0u8; MAX_MSG_LEN + 1];

    let bytes = msg.as_bytes();
    let len = bytes.len().min(MAX_MSG_LEN);
    buf[..len].copy_from_slice(&bytes[..len]);
    // buf[len] is already 0 (NUL).

    // SAFETY: `buf` is a valid NUL-terminated byte array for the duration of
    // this call.  `console_log` is defined by the Microkit runtime and does
    // not retain the pointer after it returns.
    unsafe {
        crate::ffi::console_log(level, color, buf.as_ptr());
    }
}

// ── Convenience wrappers ──────────────────────────────────────────────────────

/// Log at CRITICAL / plain level (level 0, no color).
#[inline]
pub fn critical(msg: &str) {
    log(0, 0, msg);
}

/// Log at ERROR level (level 1).
#[inline]
pub fn error(msg: &str) {
    log(1, 1, msg);
}

/// Log at WARN level (level 2).
#[inline]
pub fn warn(msg: &str) {
    log(2, 3, msg);
}

/// Log at INFO level (level 3).
#[inline]
pub fn info(msg: &str) {
    log(3, 2, msg);
}

/// Log at DEBUG level (level 7).
#[inline]
pub fn debug(msg: &str) {
    log(7, 7, msg);
}
