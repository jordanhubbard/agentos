//! Raw seL4 Microkit FFI bindings.
//!
//! These mirror the C declarations in `microkit.h` that every Protection Domain
//! compiled against the Microkit SDK sees.  In a real PD build the linker resolves
//! these symbols from the Microkit runtime; in host-side tests they are replaced by
//! the mock implementations in `tests/rust_pd_unit_test.rs`.

// ── Message-register bank ─────────────────────────────────────────────────────
//
// The Microkit ABI exposes up to 64 message registers (MRs) numbered 0..63.
// The kernel stores them in seL4_GetMR / seL4_SetMR which are themselves thin
// wrappers around the IPC buffer in the thread's TLS region.

extern "C" {
    /// Read message register `idx`.
    pub fn seL4_GetMR(idx: i32) -> u64;
    /// Write message register `idx` with `val`.
    pub fn seL4_SetMR(idx: i32, val: u64);

    /// Construct a `microkit_msginfo` word from its fields.
    ///
    /// `label`      — caller-defined opcode (52 bits)
    /// `caps_unwrapped` — number of unwrapped caps in the message
    /// `extra_caps` — number of extra caps transferred
    /// `length`     — number of message-register words populated
    pub fn microkit_msginfo_new(
        label: u64,
        caps_unwrapped: u64,
        extra_caps: u64,
        length: u64,
    ) -> u64;

    /// Extract the label field from a `microkit_msginfo` word.
    pub fn microkit_msginfo_get_label(info: u64) -> u64;

    /// Extract the MR count field from a `microkit_msginfo` word.
    pub fn microkit_msginfo_get_length(info: u64) -> u64;

    /// Log a NUL-terminated string via the Microkit debug console.
    ///
    /// The `level` and `color` parameters correspond to the agentOS
    /// `console_log(level, color, str)` calling convention used throughout
    /// the kernel PDs.  Pass 0 for both to get plain output.
    pub fn console_log(level: u32, color: u32, s: *const u8);
}

// ── Microkit channel type ─────────────────────────────────────────────────────

/// A Microkit channel number (0-62).  Corresponds to `microkit_channel` in C.
pub type MicrokitChannel = u32;
