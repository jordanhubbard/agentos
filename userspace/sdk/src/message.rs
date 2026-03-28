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
