//! `cargo xtask audit-caps` — query capability audit data from agentOS.
//!
//! Sends OP_CAP_AUDIT (or OP_CAP_AUDIT_GUEST) via the cc_pd IPC bridge and
//! formats the result as a human-readable table.
//!
//! In the absence of a live seL4 target (i.e. the cc_pd socket is not
//! reachable), the subcommand falls back to **test mode**: it prints a table
//! of hardcoded example entries that reflect a canonical system boot state.
//! Test mode is always used when `--test` is passed explicitly.
//!
//! # Output format
//!
//! ```text
//! PD-ID  CSLOT  TYPE       REVOCABLE  NAME
//! 0      1      CapTable   no         root-cnode
//! 0      2      VSpace     no         root-vspace
//! 0      3      TCB        no         root-tcb
//! 1      100    TCB        yes        ns-tcb
//! 1      101    Endpoint   yes        ns-ep
//! ...
//! Total: 10 caps across 4 PDs
//! ```
//!
//! # Filtering
//!
//! `--pd <id>`       — show only caps for one PD (passes pd_id to OP_CAP_AUDIT)
//! `--guest <handle>` — use OP_CAP_AUDIT_GUEST to show guest caps
//!
//! # Exit codes
//!
//! 0 — success (entries printed or no entries found)
//! 1 — IPC error or invalid arguments

use crate::AuditCapsArgs;
use anyhow::{Result, bail};

/// One capability entry as decoded from the audit reply.
#[derive(Debug)]
struct CapEntry {
    pd_id:     u32,
    cslot:     u32,
    cap_type:  u32,
    revocable: bool,
    name:      String,
}

/// Map a numeric seL4 object type to a short human-readable label.
fn type_name(cap_type: u32) -> &'static str {
    match cap_type {
        1  => "TCB",
        2  => "Endpoint",
        3  => "VCPU",
        4  => "Notification",
        5  => "Untyped",
        9  => "IRQHandler",
        10 => "CapTable",
        11 => "VSpace",
        12 => "Frame",
        _  => "Unknown",
    }
}

/// Print entries as a fixed-width table and emit the summary line.
fn print_table(entries: &[CapEntry]) {
    // Count distinct PDs.
    let mut pd_set = std::collections::BTreeSet::new();
    for e in entries {
        pd_set.insert(e.pd_id);
    }

    // Header.
    println!(
        "{:<6} {:<6} {:<12} {:<10} {}",
        "PD-ID", "CSLOT", "TYPE", "REVOCABLE", "NAME"
    );
    println!("{}", "-".repeat(50));

    // Rows.
    for e in entries {
        println!(
            "{:<6} {:<6} {:<12} {:<10} {}",
            e.pd_id,
            e.cslot,
            type_name(e.cap_type),
            if e.revocable { "yes" } else { "no" },
            e.name,
        );
    }

    println!();
    println!(
        "Total: {} caps across {} PDs",
        entries.len(),
        pd_set.len()
    );
}

/// Build the hardcoded example table used in test mode.
///
/// The entries reflect a 4-PD system (root task + nameserver + controller +
/// VMM) immediately after boot, before any vibeOS guests are created.  The
/// cap types match the numeric constants in seL4's object model.
fn test_mode_entries(pd_filter: Option<u32>, guest_handle: Option<u32>) -> Vec<CapEntry> {
    let all: Vec<CapEntry> = vec![
        // Root task (pd_id=0) — root-level, not revocable.
        CapEntry { pd_id: 0, cslot: 1,   cap_type: 10, revocable: false, name: "root-cnode".into()  },
        CapEntry { pd_id: 0, cslot: 2,   cap_type: 11, revocable: false, name: "root-vspace".into() },
        CapEntry { pd_id: 0, cslot: 3,   cap_type: 1,  revocable: false, name: "root-tcb".into()    },
        // Nameserver (pd_id=1).
        CapEntry { pd_id: 1, cslot: 100, cap_type: 1,  revocable: true,  name: "ns-tcb".into()      },
        CapEntry { pd_id: 1, cslot: 101, cap_type: 2,  revocable: true,  name: "ns-ep".into()       },
        // Controller (pd_id=2).
        CapEntry { pd_id: 2, cslot: 200, cap_type: 1,  revocable: true,  name: "ctrl-tcb".into()    },
        CapEntry { pd_id: 2, cslot: 201, cap_type: 2,  revocable: true,  name: "ctrl-ep".into()     },
        // VMM / guest (pd_id=3, handle=3).
        CapEntry { pd_id: 3, cslot: 300, cap_type: 1,  revocable: true,  name: "vmm-tcb".into()     },
        CapEntry { pd_id: 3, cslot: 301, cap_type: 2,  revocable: true,  name: "vmm-ep".into()      },
        CapEntry { pd_id: 3, cslot: 302, cap_type: 3,  revocable: true,  name: "vmm-vcpu".into()    },
    ];

    // Apply filters.
    if let Some(h) = guest_handle {
        // OP_CAP_AUDIT_GUEST: match pd_id == guest handle.
        return all.into_iter().filter(|e| e.pd_id == h).collect();
    }
    if let Some(pd) = pd_filter {
        if pd != 0 {
            return all.into_iter().filter(|e| e.pd_id == pd).collect();
        }
    }
    all
}

/// Main entry point for the `audit-caps` subcommand.
pub fn run(args: &AuditCapsArgs) -> Result<()> {
    // Validate: --pd and --guest are mutually exclusive.
    if args.pd.is_some() && args.guest.is_some() {
        bail!("--pd and --guest are mutually exclusive; use one or the other");
    }

    // Determine mode.
    let use_test_mode = args.test || {
        // Auto-detect: if cc_pd socket path doesn't exist, fall back to test mode.
        // The socket path is conventionally /tmp/agentos-cc.sock; absent = test mode.
        let sock = std::path::Path::new("/tmp/agentos-cc.sock");
        !sock.exists()
    };

    let entries = if use_test_mode {
        eprintln!("[audit-caps] no live target — using test-mode stub data");
        test_mode_entries(args.pd, args.guest)
    } else {
        // Live IPC path: send OP_CAP_AUDIT or OP_CAP_AUDIT_GUEST via the
        // cc_pd Unix socket bridge.  The bridge accepts a binary request frame
        // and returns binary reply + shared-memory dump.
        //
        // This path is stubbed for now: the cc_pd bridge protocol is defined in
        // contracts/cc_contract.h and is implemented in a forthcoming sprint.
        // Callers in CI will always use test mode; live invocation requires a
        // running QEMU or hardware target.
        eprintln!("[audit-caps] live IPC path not yet implemented — falling back to test mode");
        test_mode_entries(args.pd, args.guest)
    };

    if entries.is_empty() {
        println!("No capability entries found for the requested filter.");
        return Ok(());
    }

    // Describe the operation.
    if let Some(h) = args.guest {
        println!("[audit-caps] OP_CAP_AUDIT_GUEST  handle={}", h);
    } else if let Some(pd) = args.pd {
        println!("[audit-caps] OP_CAP_AUDIT  pd_id={}", pd);
    } else {
        println!("[audit-caps] OP_CAP_AUDIT  pd_id=0 (all PDs)");
    }
    println!();

    print_table(&entries);

    Ok(())
}
