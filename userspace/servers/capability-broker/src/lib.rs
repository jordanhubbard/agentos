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
}

impl core::fmt::Display for BrokerError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            BrokerError::UnknownToken => write!(f, "unknown capability token"),
            BrokerError::InsufficientRights => write!(f, "insufficient rights"),
            BrokerError::PolicyDenied => write!(f, "policy denied"),
            BrokerError::NotDelegatable => write!(f, "capability is not delegatable"),
        }
    }
}

/// The capability broker
#[derive(Debug, Default)]
pub struct CapabilityBroker {
    grants: BTreeMap<GrantToken, Grant>,
    next_token: u64,
    /// ACL: (grantor, CapKind) → allowed
    policy: Vec<(String, CapKind)>,
}

impl CapabilityBroker {
    pub fn new() -> Self {
        Self::default()
    }

    /// Allow `agent_id` to issue grants of `kind`
    pub fn allow(&mut self, agent_id: impl Into<String>, kind: CapKind) {
        self.policy.push((agent_id.into(), kind));
    }

    /// Issue a new capability grant from `grantor` to `grantee`
    pub fn issue(
        &mut self,
        grantor: impl Into<String>,
        grantee: impl Into<String>,
        kind: CapKind,
        rights: Rights,
        delegatable: bool,
    ) -> Result<GrantToken, BrokerError> {
        let grantor = grantor.into();
        // Check policy
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
        token: &GrantToken,
        holder: &str,
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
        token: &GrantToken,
        current_holder: &str,
        new_grantee: impl Into<String>,
        rights: Rights,
    ) -> Result<GrantToken, BrokerError> {
        // Validate caller holds the token with grant rights
        let parent = self.grants.get(token).ok_or(BrokerError::UnknownToken)?;
        if parent.grantee != current_holder {
            return Err(BrokerError::InsufficientRights);
        }
        if !parent.delegatable {
            return Err(BrokerError::NotDelegatable);
        }
        // Derived rights cannot exceed parent rights
        let derived = Rights(parent.rights.0 & rights.0);
        let kind = parent.kind.clone();
        let grantor = alloc::string::String::from(current_holder);
        let new_token = GrantToken(self.next_token);
        self.next_token += 1;
        self.grants.insert(new_token.clone(), Grant {
            token: new_token.clone(),
            kind,
            rights: derived,
            grantor,
            grantee: new_grantee.into(),
            delegatable: false, // derived grants are not further delegatable by default
        });
        Ok(new_token)
    }

    /// Revoke a capability grant
    pub fn revoke(&mut self, token: &GrantToken) -> Result<(), BrokerError> {
        self.grants.remove(token).ok_or(BrokerError::UnknownToken)?;
        Ok(())
    }

    /// Number of live grants
    pub fn grant_count(&self) -> usize {
        self.grants.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn broker_with_policy() -> CapabilityBroker {
        let mut b = CapabilityBroker::new();
        b.allow("monitor", CapKind::Memory);
        b.allow("monitor", CapKind::Endpoint);
        b
    }

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
        let child = b.delegate(&parent, "agent-a", "agent-b", Rights::READ).unwrap();
        let grant = b.validate(&child, "agent-b", Rights::READ).unwrap();
        // Derived rights cannot exceed parent
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
}
