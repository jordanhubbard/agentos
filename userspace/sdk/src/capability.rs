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
    /// Human-readable context hint (e.g. namespace path, partition name).
    /// Not security-enforced — for logging and introspection only.
    pub kind_hint: String,
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
            kind_hint: String::new(),
        }
    }

    /// Attach a human-readable hint (namespace, partition name, etc.).
    /// The hint is not security-enforced and is stripped on `restrict`.
    pub fn with_hint(mut self, hint: impl Into<String>) -> Self {
        self.kind_hint = hint.into();
        self
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

    /// Create a restricted version of this capability (subset of rights only).
    /// Returns None if the requested rights exceed what this cap grants.
    /// The `kind_hint` is preserved so audit logs remain readable.
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

/// The kinds of capabilities in agentOS.
///
/// Associated data (namespace, partition, protocol, etc.) has been moved to
/// `Capability::kind_hint` (a plain `String`).  This makes `CapabilityKind`
/// `Copy`-friendly, removes the `Hash` concern over heap strings, and prevents
/// accidental security decisions based on unverified hint values.
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
    /// Access to an AgentFS namespace (path prefix stored in kind_hint)
    ObjectStore,
    /// Access to a VectorStore partition (partition name in kind_hint)
    VectorStore,
    /// Network endpoint capability (protocol/scope in kind_hint)
    Network,
    /// Spawn agents with bounded capability sets (limit in kind_hint)
    AgentSpawn,
    /// Read the capability audit log
    Audit,
    /// Compute resources (CPU time budget stored in kind_hint)
    Compute,
    /// Memory allocation from a specific pool (pool/limit in kind_hint)
    Memory,
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

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;

    // ── Rights ────────────────────────────────────────────────────────────────

    #[test]
    fn rights_has_returns_correct_bit() {
        let r = Rights::READ_WRITE;
        assert!(r.has(Right::Read));
        assert!(r.has(Right::Write));
        assert!(!r.has(Right::Execute));
        assert!(!r.has(Right::Grant));
    }

    #[test]
    fn rights_none_has_nothing() {
        assert!(!Rights::NONE.has(Right::Read));
        assert!(!Rights::NONE.has(Right::Write));
        assert!(!Rights::NONE.has(Right::Execute));
    }

    #[test]
    fn rights_all_has_everything() {
        assert!(Rights::ALL.has(Right::Read));
        assert!(Rights::ALL.has(Right::Write));
        assert!(Rights::ALL.has(Right::Execute));
        assert!(Rights::ALL.has(Right::Grant));
        assert!(Rights::ALL.has(Right::Revoke));
    }

    #[test]
    fn rights_is_superset_of() {
        assert!(Rights::ALL.is_superset_of(&Rights::READ));
        assert!(Rights::READ_WRITE.is_superset_of(&Rights::READ));
        assert!(!Rights::READ.is_superset_of(&Rights::READ_WRITE));
        assert!(Rights::NONE.is_superset_of(&Rights::NONE));
    }

    #[test]
    fn rights_add_and_remove() {
        let r = Rights::NONE.add(Right::Read).add(Right::Write);
        assert_eq!(r, Rights::READ_WRITE);
        let r2 = r.remove(Right::Write);
        assert_eq!(r2, Rights::READ);
        assert!(!r2.has(Right::Write));
    }

    #[test]
    fn rights_bitor() {
        let r = Rights::READ | Rights::WRITE;
        assert!(r.has(Right::Read));
        assert!(r.has(Right::Write));
        assert!(!r.has(Right::Execute));
    }

    // ── Capability construction ───────────────────────────────────────────────

    #[test]
    fn capability_new_defaults() {
        let cap = Capability::new(CapabilityKind::Endpoint, 42, Rights::READ);
        assert_eq!(cap.kind, CapabilityKind::Endpoint);
        assert_eq!(cap.cptr, 42);
        assert_eq!(cap.rights, Rights::READ);
        assert!(cap.badge.is_none());
        assert!(!cap.delegatable);
        assert!(cap.kind_hint.is_empty());
    }

    #[test]
    fn capability_builder_chain() {
        let cap = Capability::new(CapabilityKind::Network, 10, Rights::ALL)
            .with_hint("tcp://localhost")
            .with_delegation()
            .with_badge(0xBEEF);
        assert_eq!(cap.kind_hint, "tcp://localhost");
        assert!(cap.delegatable);
        assert_eq!(cap.badge, Some(0xBEEF));
    }

    #[test]
    fn capability_has_right() {
        let cap = Capability::new(CapabilityKind::Thread, 1, Rights::READ_WRITE);
        assert!(cap.has_right(Right::Read));
        assert!(cap.has_right(Right::Write));
        assert!(!cap.has_right(Right::Grant));
    }

    #[test]
    fn capability_restrict_subset_succeeds() {
        let cap = Capability::new(CapabilityKind::ObjectStore, 5, Rights::ALL)
            .with_delegation();
        let restricted = cap.restrict(Rights::READ).unwrap();
        assert_eq!(restricted.rights, Rights::READ);
        assert!(!restricted.delegatable);  // restricted caps are not delegatable
    }

    #[test]
    fn capability_restrict_superset_fails() {
        let cap = Capability::new(CapabilityKind::ObjectStore, 5, Rights::READ);
        let result = cap.restrict(Rights::READ_WRITE);
        assert!(result.is_none());
    }

    #[test]
    fn capability_restrict_equal_succeeds() {
        let cap = Capability::new(CapabilityKind::Endpoint, 7, Rights::READ_WRITE);
        let restricted = cap.restrict(Rights::READ_WRITE).unwrap();
        assert_eq!(restricted.rights, Rights::READ_WRITE);
    }

    // ── CapabilitySet ─────────────────────────────────────────────────────────

    #[test]
    fn capability_set_empty_by_default() {
        let cs = CapabilitySet::new();
        assert!(cs.is_empty());
        assert_eq!(cs.len(), 0);
    }

    #[test]
    fn capability_set_add_and_len() {
        let mut cs = CapabilitySet::new();
        cs.add(Capability::new(CapabilityKind::Thread, 1, Rights::ALL));
        cs.add(Capability::new(CapabilityKind::Endpoint, 2, Rights::READ));
        assert_eq!(cs.len(), 2);
        assert!(!cs.is_empty());
    }

    #[test]
    fn capability_set_find_by_kind() {
        let mut cs = CapabilitySet::new();
        cs.add(Capability::new(CapabilityKind::Thread, 1, Rights::ALL));
        cs.add(Capability::new(CapabilityKind::Endpoint, 2, Rights::READ));
        cs.add(Capability::new(CapabilityKind::Endpoint, 3, Rights::WRITE));

        let threads = cs.find(&CapabilityKind::Thread);
        assert_eq!(threads.len(), 1);
        assert_eq!(threads[0].cptr, 1);

        let endpoints = cs.find(&CapabilityKind::Endpoint);
        assert_eq!(endpoints.len(), 2);
    }

    #[test]
    fn capability_set_find_absent_kind_returns_empty() {
        let cs = CapabilitySet::new();
        assert!(cs.find(&CapabilityKind::Network).is_empty());
    }

    #[test]
    fn capability_set_can() {
        let mut cs = CapabilitySet::new();
        cs.add(Capability::new(CapabilityKind::VectorStore, 9, Rights::READ_WRITE));
        assert!(cs.can(&CapabilityKind::VectorStore, Right::Read));
        assert!(cs.can(&CapabilityKind::VectorStore, Right::Write));
        assert!(!cs.can(&CapabilityKind::VectorStore, Right::Execute));
        assert!(!cs.can(&CapabilityKind::Thread, Right::Read));
    }

    // ── derive_for_child ──────────────────────────────────────────────────────

    fn make_delegatable(cptr: u64, kind: CapabilityKind, rights: Rights) -> Capability {
        Capability::new(kind, cptr, rights).with_delegation()
    }

    #[test]
    fn derive_for_child_success() {
        let mut parent = CapabilitySet::new();
        parent.add(make_delegatable(100, CapabilityKind::Endpoint, Rights::ALL));

        let granted = alloc::vec![
            Capability::new(CapabilityKind::Endpoint, 100, Rights::READ),
        ];
        let child = parent.derive_for_child(&granted).unwrap();
        assert_eq!(child.len(), 1);
        assert!(child.can(&CapabilityKind::Endpoint, Right::Read));
        assert!(!child.can(&CapabilityKind::Endpoint, Right::Write));
    }

    #[test]
    fn derive_for_child_records_delegation_log() {
        let mut parent = CapabilitySet::new();
        parent.add(make_delegatable(200, CapabilityKind::Thread, Rights::ALL));

        let granted = alloc::vec![
            Capability::new(CapabilityKind::Thread, 200, Rights::READ),
        ];
        parent.derive_for_child(&granted).unwrap();

        let log = parent.delegation_log();
        assert_eq!(log.len(), 1);
        assert_eq!(log[0].parent_cptr, 200);
    }

    #[test]
    fn derive_for_child_child_cptr_differs_from_parent() {
        let mut parent = CapabilitySet::new();
        parent.add(make_delegatable(300, CapabilityKind::Memory, Rights::ALL));

        let granted = alloc::vec![
            Capability::new(CapabilityKind::Memory, 300, Rights::READ),
        ];
        let child = parent.derive_for_child(&granted).unwrap();
        let child_caps = child.find(&CapabilityKind::Memory);
        assert_ne!(child_caps[0].cptr, 300);
    }

    #[test]
    fn derive_for_child_not_owned_returns_error() {
        let mut parent = CapabilitySet::new();
        // cptr 999 is not in parent set
        let granted = alloc::vec![
            Capability::new(CapabilityKind::Endpoint, 999, Rights::READ),
        ];
        let err = parent.derive_for_child(&granted).unwrap_err();
        assert_eq!(err, CapError::NotOwned);
    }

    #[test]
    fn derive_for_child_not_delegatable_returns_error() {
        let mut parent = CapabilitySet::new();
        // Capability without .with_delegation()
        parent.add(Capability::new(CapabilityKind::Endpoint, 50, Rights::ALL));

        let granted = alloc::vec![
            Capability::new(CapabilityKind::Endpoint, 50, Rights::READ),
        ];
        let err = parent.derive_for_child(&granted).unwrap_err();
        assert_eq!(err, CapError::NotDelegatable);
    }

    #[test]
    fn derive_for_child_rights_exceeded_returns_error() {
        let mut parent = CapabilitySet::new();
        parent.add(make_delegatable(60, CapabilityKind::ObjectStore, Rights::READ));

        // Request WRITE which parent doesn't have
        let granted = alloc::vec![
            Capability::new(CapabilityKind::ObjectStore, 60, Rights::READ_WRITE),
        ];
        let err = parent.derive_for_child(&granted).unwrap_err();
        assert_eq!(err, CapError::RightsExceeded);
    }

    // ── DelegationRecord chain hash ───────────────────────────────────────────

    #[test]
    fn delegation_chain_hash_nonzero_for_nonzero_inputs() {
        let mut parent = CapabilitySet::new();
        parent.add(make_delegatable(0xABCD, CapabilityKind::Endpoint, Rights::ALL));

        let granted = alloc::vec![
            Capability::new(CapabilityKind::Endpoint, 0xABCD, Rights::READ),
        ];
        parent.derive_for_child(&granted).unwrap();
        let hash = parent.delegation_log()[0].chain_hash;
        // Hash should not be all zeros (extremely unlikely collision)
        assert_ne!(hash, [0u8; 32]);
    }

    #[test]
    fn delegation_chain_hash_distinct_for_distinct_cptrss() {
        let mut parent = CapabilitySet::new();
        parent.add(make_delegatable(111, CapabilityKind::Thread, Rights::ALL));
        parent.add(make_delegatable(222, CapabilityKind::Thread, Rights::ALL));

        let g1 = alloc::vec![Capability::new(CapabilityKind::Thread, 111, Rights::READ)];
        let g2 = alloc::vec![Capability::new(CapabilityKind::Thread, 222, Rights::READ)];

        parent.derive_for_child(&g1).unwrap();
        parent.derive_for_child(&g2).unwrap();

        let log = parent.delegation_log();
        assert_ne!(log[0].chain_hash, log[1].chain_hash);
    }

    // ── CapError Display ──────────────────────────────────────────────────────

    #[test]
    fn cap_error_display() {
        assert_eq!(CapError::NotOwned.to_string(), "capability not owned by this agent");
        assert_eq!(CapError::NotDelegatable.to_string(), "capability is not delegatable");
        assert_eq!(CapError::RightsExceeded.to_string(), "requested rights exceed owned rights");
        assert_eq!(CapError::Revoked.to_string(), "capability has been revoked");
        assert_eq!(CapError::InvalidKind.to_string(), "invalid capability kind for this operation");
    }
}
