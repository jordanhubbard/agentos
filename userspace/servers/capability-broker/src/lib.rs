//! Capability Broker — seL4 capability delegation between protection domains
//!
//! Agents cannot hold direct seL4 caps to each other.  Instead they request
//! the capability broker to mint, validate, delegate, and revoke capability
//! grants on their behalf.
//!
//! The broker is the only PD allowed to perform CNode operations that move
//! caps between address spaces.  All grant requests carry the requester's
//! identity so the broker can enforce policy (ACL / RBAC).

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

// ── Maximum table sizes ───────────────────────────────────────────────────────
const MAX_CAP_GRANTS: usize = 1024;
const MAX_AUDIT_ENTRIES: usize = 256;

/// A capability kind (mirrors `sdk::capability::CapabilityKind` but kept
/// independent so this crate does not depend on the full SDK)
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub enum CapKind {
    Memory,
    Endpoint,
    Notification,
    IrqHandler,
    Frame,
    Custom(String),
}

/// Access rights bitmask
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Rights(pub u32);

impl Rights {
    pub const READ: Rights = Rights(0x01);
    pub const WRITE: Rights = Rights(0x02);
    pub const GRANT: Rights = Rights(0x04);
    pub const ALL: Rights = Rights(0x07);
    pub const NONE: Rights = Rights(0x00);

    pub fn contains(self, other: Rights) -> bool {
        (self.0 & other.0) == other.0
    }
}

/// A granted capability token — opaque handle passed between PDs
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct GrantToken(pub u64);

/// A record of a live capability grant
#[derive(Debug, Clone)]
pub struct Grant {
    pub token: GrantToken,
    pub kind: CapKind,
    pub rights: Rights,
    /// Who issued this grant
    pub grantor: String,
    /// Who holds it
    pub grantee: String,
    /// Whether it can be further delegated
    pub delegatable: bool,
}

// ── Simple grant record for the agent_id+cap_kind API ─────────────────────────

/// A capability grant stored by the grant/check/revoke API
#[derive(Debug, Clone)]
pub struct CapGrant {
    pub agent_id:   String,
    pub cap_kind:   CapKind,
    pub rights:     Rights,
    /// Expiry as a monotonic millisecond timestamp; `u64::MAX` = never expires
    pub expires_at: u64,
}

// ── Audit log ─────────────────────────────────────────────────────────────────

/// Audit operation kind
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AuditOp {
    Grant,
    Check,
    Revoke,
}

/// Result of an audited operation
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AuditResult {
    Allowed,
    Denied,
    NotFound,
}

/// One entry in the audit log
#[derive(Debug, Clone)]
pub struct AuditEntry {
    /// Monotonic timestamp in milliseconds (caller-supplied; use 0 if unavailable)
    pub timestamp_ms: u64,
    pub op:           AuditOp,
    pub agent_id:     String,
    pub cap_kind:     CapKind,
    pub result:       AuditResult,
}

/// Errors from the CapabilityBroker
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BrokerError {
    /// Requested token is not known / already revoked
    UnknownToken,
    /// Requester does not hold sufficient rights
    InsufficientRights,
    /// Grantor is not allowed to issue this kind of capability
    PolicyDenied,
    /// Token cannot be delegated further
    NotDelegatable,
    /// Grant table is full
    TableFull,
}

impl core::fmt::Display for BrokerError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            BrokerError::UnknownToken       => write!(f, "unknown capability token"),
            BrokerError::InsufficientRights => write!(f, "insufficient rights"),
            BrokerError::PolicyDenied       => write!(f, "policy denied"),
            BrokerError::NotDelegatable     => write!(f, "capability is not delegatable"),
            BrokerError::TableFull          => write!(f, "grant table full"),
        }
    }
}

// ── Ring-buffer audit log helper ─────────────────────────────────────────────

#[derive(Debug)]
struct AuditRing {
    entries: Vec<AuditEntry>,
    head:    usize,
    full:    bool,
}

impl AuditRing {
    fn new() -> Self {
        Self {
            entries: Vec::new(),
            head:    0,
            full:    false,
        }
    }

    fn push(&mut self, entry: AuditEntry) {
        if self.entries.len() < MAX_AUDIT_ENTRIES {
            self.entries.push(entry);
        } else {
            self.entries[self.head] = entry;
            self.head = (self.head + 1) % MAX_AUDIT_ENTRIES;
            self.full = true;
        }
    }

    /// Return the last `n` entries in chronological order (most-recent last).
    fn recent(&self, n: usize) -> &[AuditEntry] {
        let len = self.entries.len();
        if len == 0 {
            return &[];
        }
        let want = n.min(len);
        let start = len.saturating_sub(want);
        &self.entries[start..]
    }
}

// ── The capability broker ─────────────────────────────────────────────────────

/// The capability broker
#[derive(Debug, Default)]
pub struct CapabilityBroker {
    // ── Token-based API (original) ─────────────────────────────────────────
    grants:     BTreeMap<GrantToken, Grant>,
    next_token: u64,
    /// ACL: (grantor, CapKind) → allowed
    policy:     Vec<(String, CapKind)>,

    // ── agent_id + cap_kind API (new) ──────────────────────────────────────
    cap_grants: Vec<CapGrant>,

    // ── Audit log ──────────────────────────────────────────────────────────
    #[allow(dead_code)]
    audit:      AuditRing,
}

// We need a manual Default impl because AuditRing doesn't derive Default.
impl CapabilityBroker {
    pub fn new() -> Self {
        Self {
            grants:     BTreeMap::new(),
            next_token: 0,
            policy:     Vec::new(),
            cap_grants: Vec::new(),
            audit:      AuditRing::new(),
        }
    }

    // ── Policy (token-based API) ─────────────────────────────────────────────

    /// Allow `agent_id` to issue grants of `kind`
    pub fn allow(&mut self, agent_id: impl Into<String>, kind: CapKind) {
        self.policy.push((agent_id.into(), kind));
    }

    // ── Token-based API ──────────────────────────────────────────────────────

    /// Issue a new capability grant from `grantor` to `grantee`
    pub fn issue(
        &mut self,
        grantor:     impl Into<String>,
        grantee:     impl Into<String>,
        kind:        CapKind,
        rights:      Rights,
        delegatable: bool,
    ) -> Result<GrantToken, BrokerError> {
        let grantor = grantor.into();
        if !self.policy.iter().any(|(id, k)| id == &grantor && k == &kind) {
            return Err(BrokerError::PolicyDenied);
        }
        let token = GrantToken(self.next_token);
        self.next_token += 1;
        self.grants.insert(token.clone(), Grant {
            token: token.clone(),
            kind,
            rights,
            grantor,
            grantee: grantee.into(),
            delegatable,
        });
        Ok(token)
    }

    /// Validate that `holder` holds `token` with at least `required` rights
    pub fn validate(
        &self,
        token:    &GrantToken,
        holder:   &str,
        required: Rights,
    ) -> Result<&Grant, BrokerError> {
        let grant = self.grants.get(token).ok_or(BrokerError::UnknownToken)?;
        if grant.grantee != holder {
            return Err(BrokerError::InsufficientRights);
        }
        if !grant.rights.contains(required) {
            return Err(BrokerError::InsufficientRights);
        }
        Ok(grant)
    }

    /// Delegate an existing grant to a third party (requires delegatable flag)
    pub fn delegate(
        &mut self,
        token:          &GrantToken,
        current_holder: &str,
        new_grantee:    impl Into<String>,
        rights:         Rights,
    ) -> Result<GrantToken, BrokerError> {
        let parent = self.grants.get(token).ok_or(BrokerError::UnknownToken)?;
        if parent.grantee != current_holder {
            return Err(BrokerError::InsufficientRights);
        }
        if !parent.delegatable {
            return Err(BrokerError::NotDelegatable);
        }
        let derived = Rights(parent.rights.0 & rights.0);
        let kind    = parent.kind.clone();
        let grantor = alloc::string::String::from(current_holder);
        let new_token = GrantToken(self.next_token);
        self.next_token += 1;
        self.grants.insert(new_token.clone(), Grant {
            token: new_token.clone(),
            kind,
            rights: derived,
            grantor,
            grantee: new_grantee.into(),
            delegatable: false,
        });
        Ok(new_token)
    }

    /// Revoke a token-based capability grant
    pub fn revoke(&mut self, token: &GrantToken) -> Result<(), BrokerError> {
        self.grants.remove(token).ok_or(BrokerError::UnknownToken)?;
        Ok(())
    }

    /// Number of live token-based grants
    pub fn grant_count(&self) -> usize {
        self.grants.len()
    }

    // ── agent_id + cap_kind API ───────────────────────────────────────────────

    /// Store a capability grant for `agent_id` to exercise `cap_kind` with
    /// `rights`.  Returns `Err(BrokerError::TableFull)` if the grant table
    /// has reached `MAX_CAP_GRANTS` entries.
    ///
    /// `expires_at` is a caller-supplied monotonic millisecond timestamp;
    /// pass `u64::MAX` for a non-expiring grant.
    pub fn grant(
        &mut self,
        agent_id:   impl Into<String>,
        cap_kind:   CapKind,
        rights:     Rights,
        expires_at: u64,
        now_ms:     u64,
    ) -> Result<(), BrokerError> {
        let agent_id = agent_id.into();
        // Prune expired entries first to reclaim space.
        self.cap_grants.retain(|g| g.expires_at > now_ms);

        if self.cap_grants.len() >= MAX_CAP_GRANTS {
            self.audit_push(now_ms, AuditOp::Grant, &agent_id, &cap_kind, AuditResult::Denied);
            return Err(BrokerError::TableFull);
        }

        self.cap_grants.push(CapGrant {
            agent_id:   agent_id.clone(),
            cap_kind:   cap_kind.clone(),
            rights,
            expires_at,
        });
        self.audit_push(now_ms, AuditOp::Grant, &agent_id, &cap_kind, AuditResult::Allowed);
        Ok(())
    }

    /// Check whether `agent_id` holds `cap_kind` with at least `required` rights
    /// and the grant has not expired (using `now_ms` as the current time).
    pub fn check(
        &self,
        agent_id:  &str,
        cap_kind:  &CapKind,
        required:  Rights,
        now_ms:    u64,
    ) -> bool {
        let found = self.cap_grants.iter().any(|g| {
            g.agent_id == agent_id
                && g.cap_kind == *cap_kind
                && g.rights.contains(required)
                && g.expires_at > now_ms
        });
        // Note: check() takes &self so we cannot mutate the audit log here.
        // Callers that need audit records for checks should use check_audited().
        found
    }

    /// Like `check` but also records an audit entry (requires `&mut self`).
    pub fn check_audited(
        &mut self,
        agent_id: &str,
        cap_kind: &CapKind,
        required: Rights,
        now_ms:   u64,
    ) -> bool {
        let result = self.check(agent_id, cap_kind, required, now_ms);
        let audit_result = if result { AuditResult::Allowed } else { AuditResult::Denied };
        self.audit_push(now_ms, AuditOp::Check, agent_id, cap_kind, audit_result);
        result
    }

    /// Remove all grants for `agent_id + cap_kind`.
    pub fn revoke_by_agent_cap(
        &mut self,
        agent_id: &str,
        cap_kind: &CapKind,
        now_ms:   u64,
    ) {
        let before = self.cap_grants.len();
        self.cap_grants.retain(|g| !(g.agent_id == agent_id && g.cap_kind == *cap_kind));
        let removed = before - self.cap_grants.len();
        let audit_result = if removed > 0 { AuditResult::Allowed } else { AuditResult::NotFound };
        self.audit_push(now_ms, AuditOp::Revoke, agent_id, cap_kind, audit_result);
    }

    // ── Audit helpers ─────────────────────────────────────────────────────────

    fn audit_push(
        &mut self,
        timestamp_ms: u64,
        op:           AuditOp,
        agent_id:     &str,
        cap_kind:     &CapKind,
        result:       AuditResult,
    ) {
        self.audit.push(AuditEntry {
            timestamp_ms,
            op,
            agent_id: agent_id.into(),
            cap_kind: cap_kind.clone(),
            result,
        });
    }

    /// Return the last `n` audit entries (chronological order).
    pub fn audit_recent(&self, n: usize) -> &[AuditEntry] {
        self.audit.recent(n)
    }

    /// Total number of stored audit entries
    pub fn audit_len(&self) -> usize {
        self.audit.entries.len()
    }
}

// Manually implement Default to satisfy the derive(Default) on the struct
// (AuditRing doesn't derive Default, so we delegate to new()).
impl Default for AuditRing {
    fn default() -> Self {
        Self::new()
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn broker_with_policy() -> CapabilityBroker {
        let mut b = CapabilityBroker::new();
        b.allow("monitor", CapKind::Memory);
        b.allow("monitor", CapKind::Endpoint);
        b
    }

    // ── Token-based API tests (original) ────────────────────────────────────

    #[test]
    fn test_issue_and_validate() {
        let mut b = broker_with_policy();
        let tok = b.issue("monitor", "agent-a", CapKind::Memory, Rights::READ, false).unwrap();
        let grant = b.validate(&tok, "agent-a", Rights::READ).unwrap();
        assert_eq!(grant.grantee, "agent-a");
    }

    #[test]
    fn test_policy_denied() {
        let mut b = broker_with_policy();
        let err = b.issue("agent-a", "agent-b", CapKind::Memory, Rights::READ, false).unwrap_err();
        assert_eq!(err, BrokerError::PolicyDenied);
    }

    #[test]
    fn test_revoke() {
        let mut b = broker_with_policy();
        let tok = b.issue("monitor", "agent-a", CapKind::Memory, Rights::ALL, true).unwrap();
        b.revoke(&tok).unwrap();
        let err = b.validate(&tok, "agent-a", Rights::READ).unwrap_err();
        assert_eq!(err, BrokerError::UnknownToken);
    }

    #[test]
    fn test_delegate() {
        let mut b = broker_with_policy();
        let parent = b.issue("monitor", "agent-a", CapKind::Endpoint, Rights::ALL, true).unwrap();
        let child  = b.delegate(&parent, "agent-a", "agent-b", Rights::READ).unwrap();
        let grant  = b.validate(&child, "agent-b", Rights::READ).unwrap();
        assert!(grant.rights.contains(Rights::READ));
        assert!(!grant.rights.contains(Rights::GRANT));
    }

    #[test]
    fn test_not_delegatable() {
        let mut b = broker_with_policy();
        let tok = b.issue("monitor", "agent-a", CapKind::Memory, Rights::ALL, false).unwrap();
        let err = b.delegate(&tok, "agent-a", "agent-b", Rights::READ).unwrap_err();
        assert_eq!(err, BrokerError::NotDelegatable);
    }

    #[test]
    fn test_wrong_holder_validate_fails() {
        let mut b = broker_with_policy();
        let tok = b.issue("monitor", "agent-a", CapKind::Memory, Rights::READ, false).unwrap();
        let err = b.validate(&tok, "agent-b", Rights::READ).unwrap_err();
        assert_eq!(err, BrokerError::InsufficientRights);
    }

    // ── agent_id + cap_kind API tests ────────────────────────────────────────

    #[test]
    fn test_grant_and_check() {
        let mut b = CapabilityBroker::new();
        b.grant("agent-x", CapKind::Memory, Rights::READ, u64::MAX, 0).unwrap();
        assert!(b.check("agent-x", &CapKind::Memory, Rights::READ, 1));
        // Wrong rights — WRITE not granted
        assert!(!b.check("agent-x", &CapKind::Memory, Rights::WRITE, 1));
        // Wrong agent
        assert!(!b.check("agent-y", &CapKind::Memory, Rights::READ, 1));
        // Wrong cap kind
        assert!(!b.check("agent-x", &CapKind::Endpoint, Rights::READ, 1));
    }

    #[test]
    fn test_grant_expiry() {
        let mut b = CapabilityBroker::new();
        // Grant expires at t=100
        b.grant("agent-x", CapKind::Frame, Rights::ALL, 100, 0).unwrap();
        // Valid at t=50
        assert!(b.check("agent-x", &CapKind::Frame, Rights::READ, 50));
        // Expired at t=100 (expires_at is exclusive upper bound: > now_ms)
        assert!(!b.check("agent-x", &CapKind::Frame, Rights::READ, 100));
        // Expired at t=200
        assert!(!b.check("agent-x", &CapKind::Frame, Rights::READ, 200));
    }

    #[test]
    fn test_revoke_by_agent_cap() {
        let mut b = CapabilityBroker::new();
        b.grant("agent-x", CapKind::Memory,   Rights::ALL, u64::MAX, 0).unwrap();
        b.grant("agent-x", CapKind::Endpoint, Rights::ALL, u64::MAX, 0).unwrap();
        // Revoke only Memory
        b.revoke_by_agent_cap("agent-x", &CapKind::Memory, 0);
        assert!(!b.check("agent-x", &CapKind::Memory,   Rights::READ, 1));
        assert!( b.check("agent-x", &CapKind::Endpoint, Rights::READ, 1));
    }

    #[test]
    fn test_table_full() {
        let mut b = CapabilityBroker::new();
        for i in 0..MAX_CAP_GRANTS {
            b.grant(
                alloc::format!("agent-{}", i),
                CapKind::Memory,
                Rights::READ,
                u64::MAX,
                0,
            ).unwrap();
        }
        let err = b.grant("overflow", CapKind::Memory, Rights::READ, u64::MAX, 0).unwrap_err();
        assert_eq!(err, BrokerError::TableFull);
    }

    #[test]
    fn test_audit_log_records_operations() {
        let mut b = CapabilityBroker::new();
        b.grant("agent-a", CapKind::Memory, Rights::READ, u64::MAX, 0).unwrap();
        b.check_audited("agent-a", &CapKind::Memory, Rights::READ, 1);
        b.check_audited("agent-a", &CapKind::Memory, Rights::WRITE, 1); // should fail
        b.revoke_by_agent_cap("agent-a", &CapKind::Memory, 2);

        let entries = b.audit_recent(10);
        assert_eq!(entries.len(), 4);
        assert_eq!(entries[0].op, AuditOp::Grant);
        assert_eq!(entries[0].result, AuditResult::Allowed);
        assert_eq!(entries[1].op, AuditOp::Check);
        assert_eq!(entries[1].result, AuditResult::Allowed);
        assert_eq!(entries[2].op, AuditOp::Check);
        assert_eq!(entries[2].result, AuditResult::Denied);
        assert_eq!(entries[3].op, AuditOp::Revoke);
        assert_eq!(entries[3].result, AuditResult::Allowed);
    }

    #[test]
    fn test_audit_recent_limited() {
        let mut b = CapabilityBroker::new();
        for i in 0..10u64 {
            b.grant(alloc::format!("a-{}", i), CapKind::Memory, Rights::READ, u64::MAX, i).unwrap();
        }
        // audit_recent(3) should return only the last 3
        let recent = b.audit_recent(3);
        assert_eq!(recent.len(), 3);
    }

    #[test]
    fn test_audit_ring_wraps() {
        let mut b = CapabilityBroker::new();
        // Fill the audit ring past MAX_AUDIT_ENTRIES
        for i in 0..(MAX_AUDIT_ENTRIES + 10) as u64 {
            b.grant(
                alloc::format!("a-{}", i),
                CapKind::Memory,
                Rights::READ,
                u64::MAX,
                i,
            ).unwrap_or(());  // ignore TableFull — we only care about audit wrapping
            // Use check_audited to produce extra audit entries without touching the grant table
            let _ = b.check_audited(
                &alloc::format!("a-{}", i),
                &CapKind::Memory,
                Rights::READ,
                i,
            );
        }
        // Audit ring should be capped at MAX_AUDIT_ENTRIES
        assert_eq!(b.audit_len(), MAX_AUDIT_ENTRIES);
    }
}
