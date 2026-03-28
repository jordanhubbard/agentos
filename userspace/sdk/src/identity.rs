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
