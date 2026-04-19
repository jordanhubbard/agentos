//! Typed inter-agent messaging
use alloc::string::String;
use alloc::vec::Vec;
use crate::identity::AgentId;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MessageKind {
    Request,
    Response { to_request: u64 },
    Broadcast,
    SystemAlert,
}

#[derive(Debug, Clone)]
pub struct Message {
    pub id: u64,
    pub from: AgentId,
    pub to: MessageTarget,
    pub kind: MessageKind,
    pub payload: Vec<u8>,
    pub timestamp_ns: u64,
}

#[derive(Debug, Clone)]
pub enum MessageTarget {
    Agent(AgentId),
    Broadcast { topic: String },
    System,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::identity::AgentId;

    fn make_id(name: &str) -> AgentId {
        AgentId::new("test", name, 0, 0)
    }

    // ── MessageKind variants ──────────────────────────────────────────────────

    #[test]
    fn message_kind_request_eq() {
        assert_eq!(MessageKind::Request, MessageKind::Request);
    }

    #[test]
    fn message_kind_response_stores_request_id() {
        let kind = MessageKind::Response { to_request: 42 };
        if let MessageKind::Response { to_request } = kind {
            assert_eq!(to_request, 42);
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn message_kind_broadcast_eq() {
        assert_eq!(MessageKind::Broadcast, MessageKind::Broadcast);
    }

    #[test]
    fn message_kind_system_alert_eq() {
        assert_eq!(MessageKind::SystemAlert, MessageKind::SystemAlert);
    }

    // ── MessageTarget variants ────────────────────────────────────────────────

    #[test]
    fn message_target_agent_stores_id() {
        let id = make_id("worker");
        let target = MessageTarget::Agent(id.clone());
        if let MessageTarget::Agent(stored) = target {
            assert_eq!(stored, id);
        } else {
            panic!("wrong variant");
        }
    }

    #[test]
    fn message_target_broadcast_stores_topic() {
        let target = MessageTarget::Broadcast { topic: "agent.events".into() };
        if let MessageTarget::Broadcast { topic } = target {
            assert_eq!(topic, "agent.events");
        } else {
            panic!("wrong variant");
        }
    }

    // ── Message construction ──────────────────────────────────────────────────

    #[test]
    fn message_fields_accessible() {
        let from = make_id("sender");
        let to = MessageTarget::System;
        let payload = alloc::vec![1u8, 2, 3];
        let msg = Message {
            id: 99,
            from: from.clone(),
            to,
            kind: MessageKind::Request,
            payload: payload.clone(),
            timestamp_ns: 12345,
        };
        assert_eq!(msg.id, 99);
        assert_eq!(msg.from, from);
        assert_eq!(msg.payload, payload);
        assert_eq!(msg.timestamp_ns, 12345);
    }

    #[test]
    fn message_response_links_request() {
        let from = make_id("responder");
        let to = MessageTarget::Agent(make_id("requester"));
        let msg = Message {
            id: 2,
            from,
            to,
            kind: MessageKind::Response { to_request: 1 },
            payload: alloc::vec![],
            timestamp_ns: 0,
        };
        if let MessageKind::Response { to_request } = msg.kind {
            assert_eq!(to_request, 1);
        } else {
            panic!("wrong variant");
        }
    }
}
