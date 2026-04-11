//! Capability system - the core security primitive of agentOS
//!
//! Every resource access in agentOS is mediated by an unforgeable capability token.
//! Capabilities are seL4 kernel objects — you cannot forge or copy them without
//! kernel cooperation. This gives us the strongest possible security guarantees
//! without relying on software enforcement.
//!
//! ## Capability Kinds
//!
//! - **ThreadCap**: Control a thread (start, stop, priority)
//! - **EndpointCap**: Send/receive IPC messages on an endpoint
//! - **NotificationCap**: Signal or wait on a notification object  
//! - **AddressSpaceCap**: Map/unmap memory in an address space
//! - **ObjectStoreCap**: Read/write objects in AgentFS
//! - **VectorStoreCap**: Query/update a VectorStore partition
//! - **NetworkCap**: Establish or accept connections on AgentNet
//! - **AgentSpawnCap**: Spawn a child agent with bounded capabilities
//! - **AuditCap**: Read the capability audit log

use alloc::vec::Vec;
use alloc::string::String;

/// A recorded delegation event — provides an immutable audit trail of every
/// capability grant made through `derive_for_child`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DelegationRecord {
    /// CPtr of the capability in the parent's CSpace
    pub parent_cptr: u64,
    /// CPtr assigned to the capability in the child's CSpace
    pub child_cptr: u64,
    /// Rights granted to the child
    pub rights: Rights,
    /// Monotonic timestamp (nanoseconds) at delegation time
    pub timestamp_ns: u64,
    /// Attestation hash: SHA-256 (or XOR-tag fallback) of
    /// `parent_cptr || child_cptr || rights.0 as u64 || timestamp_ns`
    /// all encoded as little-endian bytes.
    pub chain_hash: [u8; 32],
}

/// Compute the attestation hash for a delegation record.
///
/// When the `sha2` cargo feature is enabled the hash is a proper SHA-256
/// digest.  Otherwise a simple position-tagged XOR construction is used so
/// that the no-std / no-external-dep path always produces a *distinct*
/// 32-byte value for distinct inputs (good enough for the simulation layer).
fn compute_chain_hash(parent_cptr: u64, child_cptr: u64, rights_bits: u64, timestamp_ns: u64) -> [u8; 32] {
    // Concatenate the four 8-byte little-endian fields into a 32-byte message.
    let mut msg = [0u8; 32];
    msg[0..8].copy_from_slice(&parent_cptr.to_le_bytes());
    msg[8..16].copy_from_slice(&child_cptr.to_le_bytes());
    msg[16..24].copy_from_slice(&rights_bits.to_le_bytes());
    msg[24..32].copy_from_slice(&timestamp_ns.to_le_bytes());

    // XOR-tag hash: each output byte is the XOR of the corresponding input
    // byte with a position-dependent constant derived from a simple LFSR
    // sequence.  This ensures output != input and that flipping any input bit
    // flips at least one output bit (weak, but sufficient for the sim layer).
    #[cfg(not(feature = "sha2"))]
    {
        let mut hash = [0u8; 32];
        let mut state: u32 = 0xABCD_1234;
        for (i, &byte) in msg.iter().enumerate() {
            // Advance LFSR (Galois form, polynomial 0x80200003)
            let lsb = state & 1;
            state >>= 1;
            if lsb != 0 {
                state ^= 0x8020_0003;
            }
            hash[i] = byte ^ (state as u8) ^ (i as u8).wrapping_mul(0x6B);
        }
        hash
    }

    #[cfg(feature = "sha2")]
    {
        use sha2::{Digest, Sha256};
        let digest = Sha256::digest(&msg);
        let mut hash = [0u8; 32];
        hash.copy_from_slice(&digest);
        hash
    }
}

/// A capability - an unforgeable reference to a kernel object or agentOS resource.
///
/// In the seL4 layer, this maps to a CPtr (capability pointer) in the thread's CSpace.
/// Above the kernel, agentOS capabilities extend this to cover AgentFS objects,
/// VectorStore partitions, and network endpoints.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Capability {
    /// The kind of resource this capability grants access to
    pub kind: CapabilityKind,
    /// The raw capability pointer (maps to seL4 CPtr)
    pub cptr: u64,
    /// What rights this capability grants
    pub rights: Rights,
    /// Optional badge - identifies this specific capability instance
    pub badge: Option<u64>,
    /// Whether this capability can be delegated to child agents
    pub delegatable: bool,
}

impl Capability {
    /// Create a new capability (kernel-only operation in production;
    /// this constructor is for the host-side simulation layer)
    pub fn new(kind: CapabilityKind, cptr: u64, rights: Rights) -> Self {
        Self {
            kind,
            cptr,
            rights,
            badge: None,
            delegatable: false,
        }
    }

    /// Create a delegatable capability
    pub fn with_delegation(mut self) -> Self {
        self.delegatable = true;
        self
    }

    /// Add a badge to distinguish this capability instance
    pub fn with_badge(mut self, badge: u64) -> Self {
        self.badge = Some(badge);
        self
    }

    /// Check if this capability grants a specific right
    pub fn has_right(&self, right: Right) -> bool {
        self.rights.has(right)
    }

    /// Create a restricted version of this capability (subset of rights only)
    /// Returns None if the requested rights exceed what this cap grants
    pub fn restrict(&self, rights: Rights) -> Option<Capability> {
        if self.rights.is_superset_of(&rights) {
            let mut restricted = self.clone();
            restricted.rights = rights;
            restricted.delegatable = false; // restricted caps aren't delegatable by default
            Some(restricted)
        } else {
            None
        }
    }
}

/// The kinds of capabilities in agentOS
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum CapabilityKind {
    // seL4 native capabilities
    Thread,
    Endpoint,
    Notification,
    AddressSpace,
    CNode,
    Untyped,
    
    // agentOS extension capabilities
    /// Access to an AgentFS namespace (path prefix)
    ObjectStore { namespace: String },
    /// Access to a VectorStore partition
    VectorStore { partition: String },
    /// Network endpoint capability
    Network { protocol: NetworkProtocol, scope: NetworkScope },
    /// Spawn agents with bounded capability sets
    AgentSpawn { max_children: u32 },
    /// Read the capability audit log
    Audit,
    /// Compute resources (CPU time budget)
    Compute { budget_ms: u64 },
    /// Memory allocation from a specific pool
    Memory { pool: MemoryPool, limit_bytes: u64 },
}

/// Access rights on a capability
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Rights(u32);

/// Individual rights bits
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Right {
    Read    = 1 << 0,
    Write   = 1 << 1,
    Execute = 1 << 2,
    Grant   = 1 << 3,  // Can delegate to others
    Revoke  = 1 << 4,  // Can revoke delegated copies
}

impl Rights {
    pub const NONE: Rights = Rights(0);
    pub const READ: Rights = Rights(Right::Read as u32);
    pub const WRITE: Rights = Rights(Right::Write as u32);
    pub const READ_WRITE: Rights = Rights(Right::Read as u32 | Right::Write as u32);
    pub const ALL: Rights = Rights(0x1F);

    pub fn has(&self, right: Right) -> bool {
        self.0 & (right as u32) != 0
    }

    pub fn is_superset_of(&self, other: &Rights) -> bool {
        (self.0 & other.0) == other.0
    }

    pub fn add(&self, right: Right) -> Rights {
        Rights(self.0 | right as u32)
    }

    pub fn remove(&self, right: Right) -> Rights {
        Rights(self.0 & !(right as u32))
    }
}

impl core::ops::BitOr for Rights {
    type Output = Rights;
    fn bitor(self, rhs: Self) -> Self::Output {
        Rights(self.0 | rhs.0)
    }
}

/// A set of capabilities - an agent's CSpace view
#[derive(Debug, Default)]
pub struct CapabilitySet {
    caps: Vec<Capability>,
    /// Immutable log of every capability delegation made from this set
    delegation_log: Vec<DelegationRecord>,
}

impl CapabilitySet {
    pub fn new() -> Self {
        Self { caps: Vec::new(), delegation_log: Vec::new() }
    }

    pub fn add(&mut self, cap: Capability) {
        self.caps.push(cap);
    }

    /// Find capabilities of a given kind
    pub fn find(&self, kind: &CapabilityKind) -> Vec<&Capability> {
        self.caps.iter()
            .filter(|c| &c.kind == kind)
            .collect()
    }

    /// Check if this set has at least one capability with the given kind and right
    pub fn can(&self, kind: &CapabilityKind, right: Right) -> bool {
        self.caps.iter()
            .any(|c| &c.kind == kind && c.has_right(right))
    }

    /// Derive a child capability set (subset only, non-delegatable).
    ///
    /// For each capability granted a `DelegationRecord` is appended to
    /// `self.delegation_log`.  The child cap receives a new `cptr` that is
    /// distinct from the parent's cptr so the simulation layer can tell them
    /// apart: `child_cptr = parent_cptr ^ 0xDEAD_0000_0000_0000 ^ index`.
    pub fn derive_for_child(&mut self, granted: &[Capability]) -> Result<CapabilitySet, CapError> {
        // Use a fixed timestamp placeholder in no-std (0); callers with std can
        // pass a real timestamp by building a DelegationRecord directly.
        let timestamp_ns: u64 = 0;

        let mut child_set = CapabilitySet::new();
        for (index, cap) in granted.iter().enumerate() {
            // Verify we own (and can delegate) each capability being granted
            let owned = self.caps.iter().find(|c| c.cptr == cap.cptr);
            match owned {
                Some(owned_cap) if owned_cap.delegatable => {
                    // Compute the child cptr — distinguishable from the parent.
                    let child_cptr = cap.cptr ^ 0xDEAD_0000_0000_0000u64 ^ index as u64;

                    // Grant a restricted copy with the new child cptr.
                    if let Some(mut restricted) = owned_cap.restrict(cap.rights.clone()) {
                        restricted.cptr = child_cptr;

                        // Record the delegation before moving `restricted` into child_set.
                        let record = DelegationRecord {
                            parent_cptr: cap.cptr,
                            child_cptr,
                            rights: restricted.rights.clone(),
                            timestamp_ns,
                            chain_hash: compute_chain_hash(
                                cap.cptr,
                                child_cptr,
                                restricted.rights.0 as u64,
                                timestamp_ns,
                            ),
                        };
                        self.delegation_log.push(record);

                        child_set.add(restricted);
                    } else {
                        return Err(CapError::RightsExceeded);
                    }
                }
                Some(_) => return Err(CapError::NotDelegatable),
                None => return Err(CapError::NotOwned),
            }
        }
        Ok(child_set)
    }

    /// Read-only view of the delegation audit log.
    pub fn delegation_log(&self) -> &[DelegationRecord] {
        &self.delegation_log
    }

    pub fn len(&self) -> usize {
        self.caps.len()
    }

    pub fn is_empty(&self) -> bool {
        self.caps.is_empty()
    }
}

/// Network protocol for network capabilities
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum NetworkProtocol {
    AgentMesh,  // intra-agentOS agent-to-agent
    Tcp,
    Udp,
    Quic,
}

/// Network scope
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum NetworkScope {
    Local,      // loopback
    Mesh,       // intra-agentOS cluster
    External,   // outbound internet
}

/// Memory pool types
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum MemoryPool {
    Standard,
    HugePage2M,
    HugePage1G,
    VectorAccelerated,  // NUMA-local to vector compute
}

/// Capability errors
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CapError {
    NotOwned,
    NotDelegatable,
    RightsExceeded,
    Revoked,
    InvalidKind,
}

impl core::fmt::Display for CapError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            CapError::NotOwned => write!(f, "capability not owned by this agent"),
            CapError::NotDelegatable => write!(f, "capability is not delegatable"),
            CapError::RightsExceeded => write!(f, "requested rights exceed owned rights"),
            CapError::Revoked => write!(f, "capability has been revoked"),
            CapError::InvalidKind => write!(f, "invalid capability kind for this operation"),
        }
    }
}
