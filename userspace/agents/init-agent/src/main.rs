//! init-agent — the first protection domain started by agentOS
//!
//! Responsibilities:
//! 1. Register core topics with the event-bus
//! 2. Bootstrap the capability-broker policy (monitor may issue Memory + Endpoint caps)
//! 3. Spawn child agents / service PDs as needed
//! 4. Enter the Microkit notification loop

#![cfg_attr(not(feature = "std"), no_std)]
#![cfg_attr(not(feature = "std"), no_main)]

extern crate alloc;

use alloc::string::ToString;

/// Microkit channel IDs assigned to this PD in the .system file
const CH_EVENT_BUS: u32 = 1;
const CH_CAP_BROKER: u32 = 2;

/// Core event topics created at boot
const TOPICS: &[&str] = &[
    "agent.fault",
    "agent.spawn",
    "agent.exit",
    "model.request",
    "model.response",
    "cap.grant",
    "cap.revoke",
];

/// Entry point called by the Microkit runtime after seL4 initialisation.
///
/// In production this calls `microkit_dbg_puts` for structured logs and uses
/// IPC to configure downstream PDs.  The `std` feature enables a hosted
/// simulation mode for integration tests.
#[cfg(feature = "std")]
fn main() {
    println!("[init-agent] starting");

    // In production these become seL4 IPC calls to the respective PDs.
    // Here we just validate the boot sequence is structurally correct.
    boot_sequence();

    println!("[init-agent] boot complete — entering notification loop");
    // Simulation: nothing to wait for
}

/// Microkit `init` entrypoint (no_std target)
#[cfg(not(feature = "std"))]
#[no_mangle]
pub extern "C" fn init() {
    boot_sequence();
}

/// Microkit `notified` entrypoint
#[cfg(not(feature = "std"))]
#[no_mangle]
pub extern "C" fn notified(channel: microkit_channel) {
    handle_notification(channel);
}

fn boot_sequence() {
    // 1. Register core topics with the event-bus PD
    for topic in TOPICS {
        ipc_create_topic(CH_EVENT_BUS, topic);
    }

    // 2. Grant the monitor permission to issue Memory + Endpoint caps
    ipc_allow_grant(CH_CAP_BROKER, "monitor", "Memory");
    ipc_allow_grant(CH_CAP_BROKER, "monitor", "Endpoint");
}

fn handle_notification(channel: u32) {
    match channel {
        CH_EVENT_BUS => { /* event-bus signalled — process pending events */ }
        CH_CAP_BROKER => { /* cap-broker signalled — process pending grants */ }
        _ => { /* unknown channel — ignore */ }
    }
}

// ---------------------------------------------------------------------------
// IPC stubs — replaced by real seL4 IPC in the kernel build
// ---------------------------------------------------------------------------

fn ipc_create_topic(_ch: u32, topic: &str) {
    #[cfg(feature = "std")]
    println!("[init-agent] create_topic: {}", topic);
}

fn ipc_allow_grant(_ch: u32, agent: &str, kind: &str) {
    #[cfg(feature = "std")]
    println!("[init-agent] allow_grant: {} -> {}", agent, kind);
    let _ = (agent.to_string(), kind.to_string()); // suppress unused warning in no_std
}

#[cfg(not(feature = "std"))]
type microkit_channel = u32;
