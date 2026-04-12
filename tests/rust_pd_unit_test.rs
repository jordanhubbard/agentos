//! Unit tests for the `agentos-pd` library (Issue #11).
//!
//! These tests run on the host with `cargo test` and exercise:
//!
//! * `ipc::MsgInfo` encoding / decoding (all field combinations)
//! * `ipc::get_mr` / `ipc::set_mr` round-trips via a thread-local mock bank
//! * `console::log` truncation and NUL-termination behaviour
//! * The health-check opcode path from `example-rust-pd`
//!
//! No seL4 kernel is required; the FFI symbols `seL4_GetMR`, `seL4_SetMR`, and
//! `console_log` are provided by the `#[no_mangle]` mock implementations below.
//!
//! # Running
//!
//! ```sh
//! # From the repo root:
//! cargo test --manifest-path libs/rust-pd/Cargo.toml --features std
//! ```
//!
//! The test binary is self-contained and exits 0 on all-pass, non-zero on any
//! failure (standard `cargo test` behaviour).

// ── Bring the library under test into scope ───────────────────────────────────
//
// We reference the library as an external crate so this file can live in the
// workspace `tests/` directory and still be compiled as part of a `cargo test`
// invocation against `libs/rust-pd` with `--features std`.

// Tell rustc this is a test-only binary with std available.
#![cfg(test)]

// Re-export the library modules we test directly.
use agentos_pd::ipc::{self, MsgInfo};

// ── Mock FFI back-end ─────────────────────────────────────────────────────────
//
// The real FFI calls into the seL4 IPC buffer.  In tests we redirect them to a
// thread-local array of 64 u64 registers so tests can run in parallel without
// interference.

use std::cell::RefCell;

thread_local! {
    /// Simulated message-register bank (64 registers, matching seL4 ABI).
    static MR_BANK: RefCell<[u64; 64]> = RefCell::new([0u64; 64]);

    /// Accumulator for everything written via console_log (for assertion).
    static CONSOLE_OUTPUT: RefCell<String> = RefCell::new(String::new());
}

// These `#[no_mangle]` functions satisfy the `extern "C"` references in
// `agentos_pd::ffi` when linking the test binary.  They must not be `unsafe`
// at the declaration site, but the extern block in ffi.rs declares them unsafe
// to call — that's on the caller side.

#[no_mangle]
extern "C" fn seL4_GetMR(idx: i32) -> u64 {
    if idx < 0 || idx >= 64 {
        return 0;
    }
    MR_BANK.with(|b| b.borrow()[idx as usize])
}

#[no_mangle]
extern "C" fn seL4_SetMR(idx: i32, val: u64) {
    if idx < 0 || idx >= 64 {
        return;
    }
    MR_BANK.with(|b| b.borrow_mut()[idx as usize] = val);
}

#[no_mangle]
extern "C" fn console_log(_level: u32, _color: u32, s: *const u8) {
    // Walk the NUL-terminated C string and append to the capture buffer.
    if s.is_null() {
        return;
    }
    let mut len = 0usize;
    unsafe {
        while *s.add(len) != 0 {
            len += 1;
            if len > 4096 {
                break; // guard against non-terminated strings in tests
            }
        }
        let bytes = std::slice::from_raw_parts(s, len);
        if let Ok(text) = std::str::from_utf8(bytes) {
            CONSOLE_OUTPUT.with(|c| c.borrow_mut().push_str(text));
        }
    }
}

/// Helper: reset the MR bank and console log for a fresh test.
fn reset_state() {
    MR_BANK.with(|b| *b.borrow_mut() = [0u64; 64]);
    CONSOLE_OUTPUT.with(|c| c.borrow_mut().clear());
}

/// Read the captured console output and reset the buffer.
fn take_console() -> String {
    CONSOLE_OUTPUT.with(|c| {
        let s = c.borrow().clone();
        c.borrow_mut().clear();
        s
    })
}

// ─────────────────────────────────────────────────────────────────────────────
// MsgInfo encoding / decoding tests
// ─────────────────────────────────────────────────────────────────────────────

#[test]
fn msginfo_zero_message() {
    let info = MsgInfo::new(0, 0, 0, 0);
    assert_eq!(info.raw(), 0, "zero message should encode to 0");
    assert_eq!(info.label(), 0);
    assert_eq!(info.caps_unwrapped(), 0);
    assert_eq!(info.extra_caps(), 0);
    assert_eq!(info.count(), 0);
}

#[test]
fn msginfo_label_round_trip() {
    // All 52 bits of the label field should survive encoding.
    let label: u64 = 0x000F_FFFF_FFFF_FFFF;
    let info = MsgInfo::new(label, 0, 0, 0);
    assert_eq!(info.label(), label, "52-bit label not preserved");
}

#[test]
fn msginfo_label_overflow_masked() {
    // Bits beyond 52 should be silently discarded.
    let overflow: u64 = u64::MAX; // all 64 bits set
    let info = MsgInfo::new(overflow, 0, 0, 0);
    let expected_label = (1u64 << 52) - 1; // 52 ones
    assert_eq!(info.label(), expected_label);
}

#[test]
fn msginfo_caps_unwrapped_range() {
    // caps_unwrapped is 3 bits (0-7).
    for v in 0u32..=7 {
        let info = MsgInfo::new(0, v, 0, 0);
        assert_eq!(info.caps_unwrapped(), v, "caps_unwrapped={v} not preserved");
    }
}

#[test]
fn msginfo_caps_unwrapped_overflow_masked() {
    let info = MsgInfo::new(0, 0xFF, 0, 0); // only low 3 bits should survive
    assert_eq!(info.caps_unwrapped(), 0x7);
}

#[test]
fn msginfo_extra_caps_range() {
    // extra_caps is 2 bits (0-3).
    for v in 0u32..=3 {
        let info = MsgInfo::new(0, 0, v, 0);
        assert_eq!(info.extra_caps(), v, "extra_caps={v} not preserved");
    }
}

#[test]
fn msginfo_extra_caps_overflow_masked() {
    let info = MsgInfo::new(0, 0, 0xFF, 0); // only low 2 bits survive
    assert_eq!(info.extra_caps(), 0x3);
}

#[test]
fn msginfo_length_range() {
    // length (count) is 7 bits (0-127).
    for v in 0u32..=127 {
        let info = MsgInfo::new(0, 0, 0, v);
        assert_eq!(info.count(), v, "length={v} not preserved");
    }
}

#[test]
fn msginfo_length_overflow_masked() {
    let info = MsgInfo::new(0, 0, 0, 0xFF); // only low 7 bits survive
    assert_eq!(info.count(), 0x7F);
}

#[test]
fn msginfo_all_fields_independent() {
    // Verify fields do not bleed into each other.
    let info = MsgInfo::new(0xABC, 5, 2, 10);
    assert_eq!(info.label(), 0xABC);
    assert_eq!(info.caps_unwrapped(), 5);
    assert_eq!(info.extra_caps(), 2);
    assert_eq!(info.count(), 10);
}

#[test]
fn msginfo_from_raw_round_trip() {
    let original = MsgInfo::new(0xDEAD_BEEF, 3, 1, 7);
    let raw = original.raw();
    let reconstructed = MsgInfo::from_raw(raw);
    assert_eq!(original, reconstructed, "from_raw(raw()) should reconstruct identical MsgInfo");
}

#[test]
fn msginfo_health_check_opcode() {
    // 0xFF is the opcode used by example-rust-pd; verify it encodes cleanly.
    let info = MsgInfo::new(0xFF, 0, 0, 0);
    assert_eq!(info.label(), 0xFF);
    assert_eq!(info.count(), 0);
}

#[test]
fn msginfo_error_reply_opcode() {
    // 0xE0 is the unknown-opcode error reply label used by example-rust-pd.
    let info = MsgInfo::new(0xE0, 0, 0, 0);
    assert_eq!(info.label(), 0xE0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Message-register (MR) accessor tests
// ─────────────────────────────────────────────────────────────────────────────

#[test]
fn mr_get_set_round_trip_all_registers() {
    reset_state();
    // Write a distinct value to each register and read it back.
    for idx in 0u32..64 {
        let val = 0xA5A5_0000_0000_0000u64 | (idx as u64);
        ipc::set_mr(idx, val);
        assert_eq!(
            ipc::get_mr(idx),
            val,
            "MR[{idx}] round-trip failed"
        );
    }
}

#[test]
fn mr_registers_are_independent() {
    reset_state();
    // Write different values to registers 0 and 1 and verify no cross-talk.
    ipc::set_mr(0, 0x1111_1111_1111_1111);
    ipc::set_mr(1, 0x2222_2222_2222_2222);
    assert_eq!(ipc::get_mr(0), 0x1111_1111_1111_1111);
    assert_eq!(ipc::get_mr(1), 0x2222_2222_2222_2222);
}

#[test]
fn mr_default_value_is_zero() {
    reset_state();
    for idx in 0u32..64 {
        assert_eq!(ipc::get_mr(idx), 0, "MR[{idx}] should default to 0 after reset");
    }
}

#[test]
fn mr_overwrite_preserves_other_registers() {
    reset_state();
    // Set all registers to a sentinel value, then overwrite one.
    for idx in 0u32..64 {
        ipc::set_mr(idx, 0xFFFF_FFFF_FFFF_FFFFu64);
    }
    ipc::set_mr(7, 42);
    assert_eq!(ipc::get_mr(7), 42);
    // Check neighbours were not disturbed.
    assert_eq!(ipc::get_mr(6), 0xFFFF_FFFF_FFFF_FFFFu64);
    assert_eq!(ipc::get_mr(8), 0xFFFF_FFFF_FFFF_FFFFu64);
}

// ─────────────────────────────────────────────────────────────────────────────
// console::log tests
// ─────────────────────────────────────────────────────────────────────────────

#[test]
fn console_log_basic_message() {
    reset_state();
    agentos_pd::console::info("[test] hello from Rust PD\n");
    let out = take_console();
    assert!(
        out.contains("[test] hello from Rust PD"),
        "Expected log message not found in console output: {out:?}"
    );
}

#[test]
fn console_log_empty_message() {
    reset_state();
    agentos_pd::console::info("");
    // Should not panic and should produce no output.
    let out = take_console();
    assert_eq!(out, "");
}

#[test]
fn console_log_max_length_message() {
    reset_state();
    // A message exactly at the limit should not be truncated.
    let msg = "x".repeat(agentos_pd::console::MAX_MSG_LEN);
    agentos_pd::console::log(0, 0, &msg);
    let out = take_console();
    assert_eq!(out.len(), agentos_pd::console::MAX_MSG_LEN);
}

#[test]
fn console_log_overlong_message_is_truncated() {
    reset_state();
    // A message longer than MAX_MSG_LEN should be silently clipped.
    let msg = "y".repeat(agentos_pd::console::MAX_MSG_LEN + 100);
    agentos_pd::console::log(0, 0, &msg);
    let out = take_console();
    assert_eq!(
        out.len(),
        agentos_pd::console::MAX_MSG_LEN,
        "Message was not truncated to MAX_MSG_LEN ({}) bytes",
        agentos_pd::console::MAX_MSG_LEN
    );
}

#[test]
fn console_convenience_levels_all_work() {
    reset_state();
    agentos_pd::console::critical("crit\n");
    agentos_pd::console::error("err\n");
    agentos_pd::console::warn("warn\n");
    agentos_pd::console::info("info\n");
    agentos_pd::console::debug("debug\n");
    let out = take_console();
    assert!(out.contains("crit"));
    assert!(out.contains("err"));
    assert!(out.contains("warn"));
    assert!(out.contains("info"));
    assert!(out.contains("debug"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Health-check opcode path (mirrors example-rust-pd logic)
// ─────────────────────────────────────────────────────────────────────────────

/// Simulate the `protected()` handler from `example-rust-pd` without actually
/// linking it (that crate is `no_std` / `no_main`).  We duplicate only the
/// logic under test so the test remains self-contained.
fn simulate_protected_handler(msg: MsgInfo) -> MsgInfo {
    const OPCODE_HEALTH_CHECK: u64 = 0xFF;
    const REPLY_ERR_UNKNOWN_OP: u64 = 0xE0;
    const VERSION_PACKED: u64 = (0u64 << 16) | (1u64 << 8) | 0u64; // 0.1.0

    match msg.label() {
        OPCODE_HEALTH_CHECK => {
            ipc::set_mr(0, VERSION_PACKED);
            ipc::set_mr(1, 1u64); // pretend 1 check served
            MsgInfo::new(0, 0, 0, 2)
        }
        _ => MsgInfo::new(REPLY_ERR_UNKNOWN_OP, 0, 0, 0),
    }
}

#[test]
fn health_check_returns_success_reply() {
    reset_state();
    let req = MsgInfo::new(0xFF, 0, 0, 0);
    let reply = simulate_protected_handler(req);
    assert_eq!(reply.label(), 0, "health check should reply with label 0 (success)");
    assert_eq!(reply.count(), 2, "health check should populate 2 MR words");
}

#[test]
fn health_check_populates_version_in_mr0() {
    reset_state();
    let req = MsgInfo::new(0xFF, 0, 0, 0);
    let _reply = simulate_protected_handler(req);
    // Version 0.1.0 packed: (0 << 16) | (1 << 8) | 0 = 256
    let expected_version: u64 = 256;
    assert_eq!(
        ipc::get_mr(0),
        expected_version,
        "MR0 should contain packed version 0.1.0 = {expected_version}"
    );
}

#[test]
fn health_check_populates_call_count_in_mr1() {
    reset_state();
    let req = MsgInfo::new(0xFF, 0, 0, 0);
    let _reply = simulate_protected_handler(req);
    // The simulated handler always writes 1 (first call).
    assert_eq!(ipc::get_mr(1), 1, "MR1 should contain health-check call count");
}

#[test]
fn unknown_opcode_returns_error_reply() {
    reset_state();
    let req = MsgInfo::new(0x00, 0, 0, 0); // opcode 0 is not handled
    let reply = simulate_protected_handler(req);
    assert_eq!(
        reply.label(),
        0xE0,
        "unknown opcode should produce error reply with label 0xE0"
    );
    assert_eq!(reply.count(), 0, "error reply should have 0 MR words");
}

#[test]
fn various_unknown_opcodes_all_return_error() {
    reset_state();
    for opcode in [0x00u64, 0x01, 0x10, 0xAB, 0xFE, 0x1234] {
        let req = MsgInfo::new(opcode, 0, 0, 0);
        let reply = simulate_protected_handler(req);
        assert_eq!(
            reply.label(),
            0xE0,
            "opcode 0x{opcode:X} should return error reply 0xE0"
        );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Library metadata tests
// ─────────────────────────────────────────────────────────────────────────────

#[test]
fn pd_runtime_version_is_non_empty() {
    assert!(
        !agentos_pd::PD_RUNTIME_VERSION.is_empty(),
        "PD_RUNTIME_VERSION should be set from Cargo.toml"
    );
}

#[test]
fn microkit_abi_version_is_one() {
    // ABI version 1 is the initial Microkit ABI this crate targets.
    assert_eq!(agentos_pd::MICROKIT_ABI_VERSION, 1);
}
