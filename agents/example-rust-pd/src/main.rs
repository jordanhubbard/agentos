//! # example-rust-pd
//!
//! A minimal agentOS Protection Domain written entirely in Rust.
//!
//! ## Behaviour
//!
//! * On `init`: logs a boot banner including the PD runtime version.
//! * On `notified`: logs the channel number at DEBUG level.
//! * On `protected`:
//!   - Opcode **0xFF** (health-check): writes version info into MR0 / MR1 and
//!     returns a reply with label 0 and length 2.
//!   - Any other opcode: returns an error reply with label 0xE0 and length 0.
//!
//! ## Channels (illustrative)
//!
//! The actual channel assignments come from the System Description File (SDF);
//! the constants below document the expected wiring.

#![no_std]
#![no_main]

use agentos_pd::ipc::{self, MsgInfo};
use agentos_pd::pd::ProtectionDomain;
use agentos_pd::{console, export_pd};

// ── Opcodes ───────────────────────────────────────────────────────────────────

/// Health-check RPC opcode.  Caller sends this label; the PD replies with
/// version info packed into MR0 (major.minor) and MR1 (patch).
const OPCODE_HEALTH_CHECK: u64 = 0xFF;

/// Error reply label: unknown opcode.
const REPLY_ERR_UNKNOWN_OP: u64 = 0xE0;

// ── Version ───────────────────────────────────────────────────────────────────

/// Semantic version packed into a u64: bits 31..16 = major, 15..8 = minor,
/// 7..0 = patch.
const VERSION_PACKED: u64 = (0u64 << 16) | (1u64 << 8) | 0u64; // 0.1.0

// ── PD state ──────────────────────────────────────────────────────────────────

struct ExamplePd {
    health_checks_served: u64,
    notifications_received: u64,
}

impl ExamplePd {
    const fn new() -> Self {
        ExamplePd {
            health_checks_served: 0,
            notifications_received: 0,
        }
    }
}

// ── ProtectionDomain implementation ──────────────────────────────────────────

impl ProtectionDomain for ExamplePd {
    /// One-time boot initialisation.
    fn init(&mut self) {
        // Log the boot banner so it appears in QEMU serial output and can be
        // grepped by the end-to-end test script.
        console::info("[example-rust-pd] booting\n");
        console::info("[example-rust-pd] runtime version: ");
        console::info(agentos_pd::PD_RUNTIME_VERSION);
        console::info("\n");
        console::info("[example-rust-pd] READY\n");
    }

    /// Handle an asynchronous notification on `channel`.
    fn notified(&mut self, channel: u32) {
        self.notifications_received = self.notifications_received.wrapping_add(1);

        // Build a small log message without heap allocation.
        // We encode the channel number as two decimal digits (channels 0-99).
        let hi = b'0' + ((channel / 10) % 10) as u8;
        let lo = b'0' + (channel % 10) as u8;
        let msg: [u8; 38] = *b"[example-rust-pd] notified channel=XX\n";
        let mut msg = msg;
        msg[35] = hi;
        msg[36] = lo;
        // SAFETY: `msg` is valid ASCII, so it is valid UTF-8.
        let s = unsafe { core::str::from_utf8_unchecked(&msg) };
        console::debug(s);
    }

    /// Handle a synchronous protected procedure call.
    fn protected(&mut self, _channel: u32, msg: MsgInfo) -> MsgInfo {
        match msg.label() {
            OPCODE_HEALTH_CHECK => {
                self.health_checks_served = self.health_checks_served.wrapping_add(1);

                // MR0: packed version word (major|minor|patch)
                ipc::set_mr(0, VERSION_PACKED);
                // MR1: number of health checks served so far
                ipc::set_mr(1, self.health_checks_served);

                console::debug("[example-rust-pd] health check OK\n");

                // Reply: label = 0 (success), 2 MR words populated
                MsgInfo::new(0, 0, 0, 2)
            }

            unknown => {
                // Log the unknown opcode (encode as 4 hex nibbles).
                let nibble = |n: u64, shift: u32| -> u8 {
                    let v = (n >> shift) & 0xF;
                    if v < 10 { b'0' + v as u8 } else { b'a' + (v - 10) as u8 }
                };
                let msg_buf: [u8; 48] =
                    *b"[example-rust-pd] unknown opcode 0xXXXX — error\n";
                let mut msg_buf = msg_buf;
                msg_buf[37] = nibble(unknown, 12);
                msg_buf[38] = nibble(unknown, 8);
                msg_buf[39] = nibble(unknown, 4);
                msg_buf[40] = nibble(unknown, 0);
                let s = unsafe { core::str::from_utf8_unchecked(&msg_buf) };
                console::warn(s);

                // Error reply: label = 0xE0, 0 MR words
                MsgInfo::new(REPLY_ERR_UNKNOWN_OP, 0, 0, 0)
            }
        }
    }
}

// ── Static PD instance and C entry-point export ───────────────────────────────

// SAFETY: Microkit delivers all entry-point calls on a single thread; there is
// no concurrent access to `PD`.
static mut PD: ExamplePd = ExamplePd::new();

// Generates `init()`, `notified()`, and `protected()` with `#[no_mangle]`
// `extern "C"` linkage that Microkit looks for when linking the final image.
export_pd!(ExamplePd, unsafe { &mut PD });
