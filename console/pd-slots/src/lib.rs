//! Canonical agentOS protection-domain slot definitions.
//!
//! Single source of truth shared by agentos-console and any other crate
//! that needs to map PD names to slot IDs.

use std::collections::HashMap;
use std::sync::OnceLock;

/// A single protection-domain slot descriptor.
#[derive(Debug, Clone, Copy)]
pub struct PdSlot {
    pub id:      usize,
    pub name:    &'static str,
    pub display: &'static str,
    pub aliases: &'static [&'static str],
}

/// Total number of slots.
pub const MAX_SLOTS: usize = 16;

/// All 16 PD slots — single source of truth.
pub const PD_SLOTS: &[PdSlot] = &[
    PdSlot { id:  0, name: "controller",  display: "controller",            aliases: &["monitor", "boot"] },
    PdSlot { id:  1, name: "event_bus",   display: "event_bus",             aliases: &["eventbus"] },
    PdSlot { id:  2, name: "init_agent",  display: "init_agent",            aliases: &["initagent"] },
    PdSlot { id:  3, name: "agentfs",     display: "agentfs",               aliases: &[] },
    PdSlot { id:  4, name: "vibe_engine", display: "vibe_engine",           aliases: &["vibeengine"] },
    PdSlot { id:  5, name: "worker_0",    display: "WASM Worker 0",         aliases: &["worker"] },
    PdSlot { id:  6, name: "worker_1",    display: "WASM Worker 1",         aliases: &[] },
    PdSlot { id:  7, name: "worker_2",    display: "WASM Worker 2",         aliases: &[] },
    PdSlot { id:  8, name: "worker_3",    display: "WASM Worker 3",         aliases: &[] },
    PdSlot { id:  9, name: "swap_slot_0", display: "WASM Swap 0",           aliases: &["swap_slot"] },
    PdSlot { id: 10, name: "swap_slot_1", display: "WASM Swap 1",           aliases: &[] },
    PdSlot { id: 11, name: "swap_slot_2", display: "WASM Swap 2",           aliases: &[] },
    PdSlot { id: 12, name: "swap_slot_3", display: "WASM Swap 3",           aliases: &[] },
    PdSlot { id: 13, name: "console_mux", display: "console_mux",           aliases: &[] },
    PdSlot { id: 14, name: "linux_vmm",   display: "Linux VM (Buildroot)",  aliases: &["linux_vm"] },
    PdSlot { id: 15, name: "fault_hndlr", display: "fault_hndlr",           aliases: &["fault_handler"] },
];

/// Canonical names indexed by slot id.
pub const SLOT_NAMES: [&str; MAX_SLOTS] = [
    "controller",
    "event_bus",
    "init_agent",
    "agentfs",
    "vibe_engine",
    "worker_0",
    "worker_1",
    "worker_2",
    "worker_3",
    "swap_slot_0",
    "swap_slot_1",
    "swap_slot_2",
    "swap_slot_3",
    "console_mux",
    "linux_vmm",
    "fault_hndlr",
];

// Build the reverse-lookup map exactly once.
static NAME_INDEX: OnceLock<HashMap<String, usize>> = OnceLock::new();

fn name_index() -> &'static HashMap<String, usize> {
    NAME_INDEX.get_or_init(|| {
        let mut map = HashMap::new();
        for pd in PD_SLOTS {
            map.insert(pd.name.to_string(), pd.id);
            for alias in pd.aliases {
                map.insert(alias.to_string(), pd.id);
            }
        }
        map
    })
}

/// Resolve any PD name or alias → slot id.
///
/// Normalises input (lowercase, spaces/hyphens → underscores) before lookup.
/// Returns `None` if the name is unknown.
pub fn name_to_slot(raw: &str) -> Option<usize> {
    let key: String = raw
        .to_lowercase()
        .chars()
        .map(|c| if c == ' ' || c == '-' { '_' } else { c })
        .collect();
    name_index().get(&key).copied()
}

/// Return the human-readable display name for a slot id.
/// Falls back to `"slot_N"` for out-of-range ids.
pub fn slot_display(id: usize) -> &'static str {
    if id < PD_SLOTS.len() {
        PD_SLOTS[id].display
    } else {
        // We can't construct a &'static str at runtime, so we use a static
        // fallback string for the "out of range" case.  Callers that need the
        // exact numeric fallback can format `slot_{id}` themselves.
        "slot_unknown"
    }
}

// ─── tests ───────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn canonical_names() {
        assert_eq!(name_to_slot("controller"), Some(0));
        assert_eq!(name_to_slot("event_bus"),  Some(1));
        assert_eq!(name_to_slot("linux_vmm"),  Some(14));
        assert_eq!(name_to_slot("fault_hndlr"), Some(15));
    }

    #[test]
    fn aliases() {
        assert_eq!(name_to_slot("monitor"),       Some(0));
        assert_eq!(name_to_slot("boot"),          Some(0));
        assert_eq!(name_to_slot("eventbus"),      Some(1));
        assert_eq!(name_to_slot("initagent"),     Some(2));
        assert_eq!(name_to_slot("vibeengine"),    Some(4));
        assert_eq!(name_to_slot("worker"),        Some(5));
        assert_eq!(name_to_slot("swap_slot"),     Some(9));
        assert_eq!(name_to_slot("linux_vm"),      Some(14));
        assert_eq!(name_to_slot("fault_handler"), Some(15));
    }

    #[test]
    fn normalisation() {
        // Mixed case
        assert_eq!(name_to_slot("Controller"), Some(0));
        assert_eq!(name_to_slot("LINUX_VMM"),  Some(14));
        // Hyphens → underscores
        assert_eq!(name_to_slot("fault-hndlr"),    Some(15));
        assert_eq!(name_to_slot("fault-handler"),  Some(15));
        // Spaces → underscores
        assert_eq!(name_to_slot("event bus"), Some(1));
    }

    #[test]
    fn unknown_returns_none() {
        assert_eq!(name_to_slot("unknown_pd"), None);
        assert_eq!(name_to_slot(""), None);
    }

    #[test]
    fn slot_display_valid() {
        assert_eq!(slot_display(0),  "controller");
        assert_eq!(slot_display(14), "Linux VM (Buildroot)");
        assert_eq!(slot_display(5),  "WASM Worker 0");
    }

    #[test]
    fn slot_display_out_of_range() {
        // Should not panic; returns a fallback string.
        let s = slot_display(99);
        assert!(!s.is_empty());
    }

    #[test]
    fn pd_slots_length() {
        assert_eq!(PD_SLOTS.len(), MAX_SLOTS);
        assert_eq!(SLOT_NAMES.len(), MAX_SLOTS);
    }
}
