//! Agent identity - who an agent is in agentOS
//!
//! Every agent has a cryptographically unique identity assigned at spawn time.
//! Identity is immutable — an agent cannot change its ID, only its metadata.

use alloc::string::String;
use alloc::vec::Vec;

/// A unique agent identifier
///
/// Format: `<namespace>/<name>@<epoch>:<random>`
/// Example: `core/event-bus@1@4a7f3bc2`
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct AgentId {
    pub namespace: String,
    pub name: String,
    pub epoch: u64,
    pub random: u64,
}

impl AgentId {
    pub fn new(namespace: impl Into<String>, name: impl Into<String>, epoch: u64, random: u64) -> Self {
        Self {
            namespace: namespace.into(),
            name: name.into(),
            epoch,
            random,
        }
    }
    
    /// Parse an AgentId from its canonical string representation
    pub fn parse(s: &str) -> Result<Self, IdentityError> {
        // Format: namespace/name@epoch:random
        let parts: Vec<&str> = s.splitn(2, '/').collect();
        if parts.len() != 2 {
            return Err(IdentityError::InvalidFormat);
        }
        let namespace = parts[0];
        
        let rest: Vec<&str> = parts[1].splitn(2, '@').collect();
        if rest.len() != 2 {
            return Err(IdentityError::InvalidFormat);
        }
        let name = rest[0];
        
        let ep_rnd: Vec<&str> = rest[1].splitn(2, ':').collect();
        if ep_rnd.len() != 2 {
            return Err(IdentityError::InvalidFormat);
        }
        
        let epoch = ep_rnd[0].parse::<u64>().map_err(|_| IdentityError::InvalidFormat)?;
        let random = u64::from_str_radix(ep_rnd[1], 16).map_err(|_| IdentityError::InvalidFormat)?;
        
        Ok(AgentId::new(namespace, name, epoch, random))
    }
}

impl core::fmt::Display for AgentId {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{}/{}@{}:{:016x}", self.namespace, self.name, self.epoch, self.random)
    }
}

/// An agent's full identity - immutable descriptor
#[derive(Debug, Clone)]
pub struct AgentIdentity {
    /// Unique identifier
    pub id: AgentId,
    /// Human-readable display name (mutable, but not the ID)
    pub display_name: String,
    /// The agent's parent (who spawned it)
    pub parent_id: Option<AgentId>,
    /// Spawn timestamp (nanoseconds since agentOS epoch)
    pub spawned_at_ns: u64,
    /// Attestation: hash of the agent binary + config at spawn time
    pub attestation: [u8; 32],
    /// Agent class - what kind of work this agent does
    pub agent_class: AgentClass,
    /// Metadata tags (agent-defined key-value pairs)
    pub tags: Vec<(String, String)>,
}

impl AgentIdentity {
    pub fn new(
        id: AgentId,
        display_name: impl Into<String>,
        parent_id: Option<AgentId>,
        spawned_at_ns: u64,
        agent_class: AgentClass,
    ) -> Self {
        Self {
            id,
            display_name: display_name.into(),
            parent_id,
            spawned_at_ns,
            attestation: [0u8; 32],
            agent_class,
            tags: Vec::new(),
        }
    }

    pub fn with_tag(mut self, key: impl Into<String>, value: impl Into<String>) -> Self {
        self.tags.push((key.into(), value.into()));
        self
    }

    pub fn with_attestation(mut self, attestation: [u8; 32]) -> Self {
        self.attestation = attestation;
        self
    }
}

/// What kind of agent this is - influences scheduling and resource allocation
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AgentClass {
    /// Core system services (event bus, capability broker, etc.)
    /// Highest priority, killed last
    SystemService,
    
    /// Persistent agent that runs indefinitely
    /// Example: a filesystem server, a monitor agent
    Persistent,
    
    /// An agent spawned to complete a specific task, then exits
    TaskAgent {
        /// Optional deadline in ns
        deadline_ns: Option<u64>,
    },
    
    /// An interactive agent that responds to external events
    /// Gets lowest latency scheduling
    Interactive,
    
    /// A batch processing agent (can be preempted freely)
    Batch,
    
    /// An inference agent (model weights in memory, long-running compute)
    Inference {
        /// Approximate working set size in bytes (for memory planning)
        model_size_bytes: u64,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum IdentityError {
    InvalidFormat,
    DuplicateId,
    InvalidNamespace,
}

impl core::fmt::Display for IdentityError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            IdentityError::InvalidFormat => write!(f, "invalid agent ID format"),
            IdentityError::DuplicateId => write!(f, "agent ID already exists"),
            IdentityError::InvalidNamespace => write!(f, "invalid namespace"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;

    // ── AgentId construction ──────────────────────────────────────────────────

    #[test]
    fn agent_id_new_stores_fields() {
        let id = AgentId::new("core", "event-bus", 1, 0xDEADBEEF);
        assert_eq!(id.namespace, "core");
        assert_eq!(id.name, "event-bus");
        assert_eq!(id.epoch, 1);
        assert_eq!(id.random, 0xDEADBEEF);
    }

    // ── AgentId Display ───────────────────────────────────────────────────────

    #[test]
    fn agent_id_display_format() {
        let id = AgentId::new("core", "event-bus", 1, 0x4a7f3bc2);
        let s = id.to_string();
        assert_eq!(s, "core/event-bus@1:000000004a7f3bc2");
    }

    // ── AgentId parse ─────────────────────────────────────────────────────────

    #[test]
    fn agent_id_parse_valid() {
        let id = AgentId::parse("core/event-bus@1:4a7f3bc2").unwrap();
        assert_eq!(id.namespace, "core");
        assert_eq!(id.name, "event-bus");
        assert_eq!(id.epoch, 1);
        assert_eq!(id.random, 0x4a7f3bc2);
    }

    #[test]
    fn agent_id_parse_round_trip() {
        let original = AgentId::new("sys", "monitor", 42, 0xCAFE0000BEEF);
        let s = original.to_string();
        let parsed = AgentId::parse(&s).unwrap();
        assert_eq!(parsed, original);
    }

    #[test]
    fn agent_id_parse_missing_slash_fails() {
        let err = AgentId::parse("core-event-bus@1:abc").unwrap_err();
        assert_eq!(err, IdentityError::InvalidFormat);
    }

    #[test]
    fn agent_id_parse_missing_at_fails() {
        let err = AgentId::parse("core/event-bus-1:abc").unwrap_err();
        assert_eq!(err, IdentityError::InvalidFormat);
    }

    #[test]
    fn agent_id_parse_missing_colon_fails() {
        let err = AgentId::parse("core/event-bus@1-abc").unwrap_err();
        assert_eq!(err, IdentityError::InvalidFormat);
    }

    #[test]
    fn agent_id_parse_non_numeric_epoch_fails() {
        let err = AgentId::parse("core/event-bus@xyz:abc").unwrap_err();
        assert_eq!(err, IdentityError::InvalidFormat);
    }

    #[test]
    fn agent_id_parse_non_hex_random_fails() {
        let err = AgentId::parse("core/event-bus@1:zzzz").unwrap_err();
        assert_eq!(err, IdentityError::InvalidFormat);
    }

    // ── AgentIdentity ─────────────────────────────────────────────────────────

    #[test]
    fn agent_identity_new_defaults() {
        let id = AgentId::new("core", "test", 0, 0);
        let ident = AgentIdentity::new(id.clone(), "Test Agent", None, 1000, AgentClass::Persistent);
        assert_eq!(ident.id, id);
        assert_eq!(ident.display_name, "Test Agent");
        assert!(ident.parent_id.is_none());
        assert_eq!(ident.spawned_at_ns, 1000);
        assert_eq!(ident.attestation, [0u8; 32]);
        assert!(ident.tags.is_empty());
    }

    #[test]
    fn agent_identity_with_tag() {
        let id = AgentId::new("core", "test", 0, 0);
        let ident = AgentIdentity::new(id, "Test", None, 0, AgentClass::Batch)
            .with_tag("env", "prod")
            .with_tag("version", "1.2.3");
        assert_eq!(ident.tags.len(), 2);
        assert_eq!(ident.tags[0], ("env".into(), "prod".into()));
        assert_eq!(ident.tags[1], ("version".into(), "1.2.3".into()));
    }

    #[test]
    fn agent_identity_with_attestation() {
        let id = AgentId::new("core", "test", 0, 0);
        let attest = [0xABu8; 32];
        let ident = AgentIdentity::new(id, "Test", None, 0, AgentClass::Interactive)
            .with_attestation(attest);
        assert_eq!(ident.attestation, attest);
    }

    #[test]
    fn agent_identity_with_parent() {
        let parent_id = AgentId::new("core", "init", 0, 0);
        let child_id = AgentId::new("user", "worker", 1, 42);
        let ident = AgentIdentity::new(child_id, "Worker", Some(parent_id.clone()), 500, AgentClass::TaskAgent { deadline_ns: Some(1_000_000) });
        assert_eq!(ident.parent_id, Some(parent_id));
    }

    // ── AgentClass variants ───────────────────────────────────────────────────

    #[test]
    fn agent_class_task_agent_has_deadline() {
        let class = AgentClass::TaskAgent { deadline_ns: Some(1_000_000_000) };
        if let AgentClass::TaskAgent { deadline_ns } = class {
            assert_eq!(deadline_ns, Some(1_000_000_000));
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn agent_class_inference_has_model_size() {
        let class = AgentClass::Inference { model_size_bytes: 7_000_000_000 };
        if let AgentClass::Inference { model_size_bytes } = class {
            assert_eq!(model_size_bytes, 7_000_000_000);
        } else {
            panic!("wrong variant");
        }
    }

    // ── IdentityError Display ─────────────────────────────────────────────────

    #[test]
    fn identity_error_display() {
        assert_eq!(IdentityError::InvalidFormat.to_string(), "invalid agent ID format");
        assert_eq!(IdentityError::DuplicateId.to_string(), "agent ID already exists");
        assert_eq!(IdentityError::InvalidNamespace.to_string(), "invalid namespace");
    }
}
