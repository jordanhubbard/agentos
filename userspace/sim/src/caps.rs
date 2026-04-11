//! Capability enforcement for the simulation layer.
//!
//! The sim reuses the `agentos_sdk::capability` types but adds runtime
//! enforcement:  every host import that accesses a resource checks the
//! calling agent's capability set before proceeding.

use std::collections::HashMap;
use agentos_sdk::capability::{Capability, CapabilityKind, CapabilitySet, Right, Rights};

/// Errors raised by capability checks.
#[derive(Debug, thiserror::Error)]
pub enum CapCheckError {
    #[error("agent has no capability of kind: {0:?}")]
    Missing(String),
    #[error("capability present but right {0:?} not granted")]
    RightDenied(String),
    #[error("capability has been revoked")]
    Revoked,
}

/// Sim-level capability store: grants capabilities to named agents and
/// enforces checks at host-import boundaries.
pub struct SimCapStore {
    /// Per-agent name → CapabilitySet
    agent_caps: HashMap<String, CapabilitySet>,
    /// Revoked cptrs (set by revoke_cap)
    revoked: std::collections::HashSet<u64>,
    /// Next cptr to allocate
    next_cptr: u64,
}

impl SimCapStore {
    pub fn new() -> Self {
        Self {
            agent_caps: HashMap::new(),
            revoked:    std::collections::HashSet::new(),
            next_cptr:  1,
        }
    }

    /// Allocate a new cptr (simulation-only; on hardware the kernel does this).
    pub fn alloc_cptr(&mut self) -> u64 {
        let c = self.next_cptr;
        self.next_cptr += 1;
        c
    }

    /// Grant a capability to a named agent.
    pub fn grant(&mut self, agent: &str, cap: Capability) {
        self.agent_caps.entry(agent.to_string()).or_default().add(cap);
    }

    /// Convenience: grant all default capabilities needed by a generic agent.
    pub fn grant_defaults(&mut self, agent: &str) {

        let caps = [
            Capability::new(CapabilityKind::Compute, self.alloc_cptr(), Rights::ALL)
                .with_hint("budget_ms=1000"),
            Capability::new(CapabilityKind::Memory, self.alloc_cptr(), Rights::ALL)
                .with_hint("pool=standard,limit_bytes=4194304"),
            Capability::new(CapabilityKind::ObjectStore, self.alloc_cptr(), Rights::READ_WRITE)
                .with_hint("namespace=/"),
            Capability::new(CapabilityKind::Audit, self.alloc_cptr(), Rights::READ),
        ];
        for cap in caps {
            self.grant(agent, cap);
        }
    }

    /// Revoke a capability by cptr across all agents.
    pub fn revoke_cap(&mut self, cptr: u64) {
        self.revoked.insert(cptr);
    }

    /// Check that `agent` holds a capability of `kind` with `right`.
    pub fn check(
        &self, agent: &str, kind: &CapabilityKind, right: Right,
    ) -> Result<(), CapCheckError> {
        let set = self.agent_caps.get(agent)
            .ok_or_else(|| CapCheckError::Missing(format!("{:?}", kind)))?;

        let caps = set.find(kind);
        if caps.is_empty() {
            return Err(CapCheckError::Missing(format!("{:?}", kind)));
        }

        for cap in &caps {
            if self.revoked.contains(&cap.cptr) {
                continue; // skip revoked
            }
            if cap.has_right(right) {
                return Ok(());
            }
        }

        Err(CapCheckError::RightDenied(format!("{:?}", right)))
    }

    /// Return all capabilities held by `agent`.
    pub fn caps_for(&self, agent: &str) -> Option<&CapabilitySet> {
        self.agent_caps.get(agent)
    }
}

impl Default for SimCapStore {
    fn default() -> Self { Self::new() }
}
