//! Raw bindings to the agentOS seL4 C bridge.
//!
//! seL4 message-register helpers are static inline C functions, not exported
//! library symbols. Native target callers must link `runtime/ipc.c` and
//! initialize the IPC buffer through the agentOS PD entry point. Host tests
//! provide bridge mocks.

// ── Message-register bank ─────────────────────────────────────────────────────
//
// The supported 64-bit seL4 ABI exposes 120 message words numbered 0..119.
// The kernel stores them in seL4_GetMR / seL4_SetMR which are themselves thin
// wrappers around the IPC buffer in the thread's TLS region.

extern "C" {
    pub fn agentos_pd_receive(endpoint: u64, badge: *mut u64) -> u64;
    pub fn agentos_pd_reply(info: u64);
    pub fn agentos_pd_call(endpoint: u64, info: u64) -> u64;
    /// Read message register `idx`.
    #[link_name = "agentos_pd_get_mr"]
    pub fn seL4_GetMR(idx: i32) -> u64;
    /// Write message register `idx` with `val`.
    #[link_name = "agentos_pd_set_mr"]
    pub fn seL4_SetMR(idx: i32, val: u64);

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
