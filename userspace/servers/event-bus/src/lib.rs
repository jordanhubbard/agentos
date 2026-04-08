//! Event Bus — seL4 IPC event routing between protection domains
//!
//! Agents publish typed events to topics; subscribers receive them via
//! seL4 notification capabilities.  This server runs as a protection domain
//! and brokers all inter-PD event traffic.
//!
//! In production this is the only component that holds Notification caps to
//! every subscriber; agents never hold caps to each other directly.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

/// A topic name (e.g. "agent.fault", "model.response")
pub type Topic = String;

/// A subscriber handle — opaque u32 maps to a seL4 Notification cap slot
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct SubscriberId(pub u32);

/// A published event (opaque bytes + topic)
#[derive(Debug, Clone)]
pub struct RawEvent {
    pub topic: Topic,
    pub payload: Vec<u8>,
    pub timestamp_ns: u64,
}

/// Per-topic subscriber list
#[derive(Debug, Default)]
struct TopicEntry {
    subscribers: Vec<SubscriberId>,
}

/// The event bus
#[derive(Debug, Default)]
pub struct EventBus {
    /// topic → subscriber list
    topics: BTreeMap<Topic, TopicEntry>,
    /// pending deliveries: subscriber → events
    pending: BTreeMap<SubscriberId, Vec<RawEvent>>,
    next_id: u32,
}

/// Errors from the EventBus
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BusError {
    /// Topic does not exist
    UnknownTopic,
    /// Subscriber handle is not valid
    UnknownSubscriber,
    /// Payload exceeds the maximum allowed size
    PayloadTooLarge,
}

impl core::fmt::Display for BusError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            BusError::UnknownTopic => write!(f, "unknown topic"),
            BusError::UnknownSubscriber => write!(f, "unknown subscriber"),
            BusError::PayloadTooLarge => write!(f, "payload too large"),
        }
    }
}

/// Maximum event payload in bytes (matches seL4 IPC message register budget)
pub const MAX_PAYLOAD_BYTES: usize = 512;

impl EventBus {
    pub fn new() -> Self {
        Self::default()
    }

    /// Create a topic.  Idempotent.
    pub fn create_topic(&mut self, topic: impl Into<Topic>) {
        self.topics.entry(topic.into()).or_default();
    }

    /// Subscribe to a topic.  Returns a fresh SubscriberId.
    pub fn subscribe(&mut self, topic: impl Into<Topic>) -> Result<SubscriberId, BusError> {
        let topic = topic.into();
        let entry = self.topics.get_mut(&topic).ok_or(BusError::UnknownTopic)?;
        let id = SubscriberId(self.next_id);
        self.next_id += 1;
        entry.subscribers.push(id);
        Ok(id)
    }

    /// Unsubscribe from all topics.
    pub fn unsubscribe(&mut self, id: SubscriberId) {
        for entry in self.topics.values_mut() {
            entry.subscribers.retain(|s| *s != id);
        }
        self.pending.remove(&id);
    }

    /// Publish an event to all subscribers of a topic.
    ///
    /// In production this would signal seL4 Notification caps.
    /// Here events are queued and retrieved via `drain`.
    pub fn publish(
        &mut self,
        topic: impl Into<Topic>,
        payload: Vec<u8>,
        timestamp_ns: u64,
    ) -> Result<usize, BusError> {
        if payload.len() > MAX_PAYLOAD_BYTES {
            return Err(BusError::PayloadTooLarge);
        }
        let topic = topic.into();
        let subs = self.topics.get(&topic)
            .ok_or(BusError::UnknownTopic)?
            .subscribers
            .clone();

        let event = RawEvent { topic, payload, timestamp_ns };
        for sub in &subs {
            self.pending.entry(*sub).or_default().push(event.clone());
        }
        Ok(subs.len())
    }

    /// Drain pending events for a subscriber (models a seL4 recv loop).
    pub fn drain(&mut self, id: SubscriberId) -> Result<Vec<RawEvent>, BusError> {
        // Verify the subscriber exists
        let exists = self.topics.values().any(|e| e.subscribers.contains(&id));
        if !exists {
            return Err(BusError::UnknownSubscriber);
        }
        Ok(self.pending.remove(&id).unwrap_or_default())
    }

    /// Number of topics registered
    pub fn topic_count(&self) -> usize {
        self.topics.len()
    }
}

#[cfg(feature = "std")]
impl core::fmt::Display for EventBus {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "EventBus({} topics)", self.topics.len())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn test_subscribe_and_publish() {
        let mut bus = EventBus::new();
        bus.create_topic("agent.fault");
        let sub = bus.subscribe("agent.fault").unwrap();
        let delivered = bus.publish("agent.fault", b"test-payload".to_vec(), 1_000).unwrap();
        assert_eq!(delivered, 1);
        let events = bus.drain(sub).unwrap();
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].payload, b"test-payload");
    }

    #[test]
    fn test_unknown_topic_publish_fails() {
        let mut bus = EventBus::new();
        let err = bus.publish("no.such.topic", vec![], 0).unwrap_err();
        assert_eq!(err, BusError::UnknownTopic);
    }

    #[test]
    fn test_unknown_topic_subscribe_fails() {
        let mut bus = EventBus::new();
        let err = bus.subscribe("no.such.topic").unwrap_err();
        assert_eq!(err, BusError::UnknownTopic);
    }

    #[test]
    fn test_payload_too_large() {
        let mut bus = EventBus::new();
        bus.create_topic("t");
        let big = vec![0u8; MAX_PAYLOAD_BYTES + 1];
        let err = bus.publish("t", big, 0).unwrap_err();
        assert_eq!(err, BusError::PayloadTooLarge);
    }

    #[test]
    fn test_unsubscribe_stops_delivery() {
        let mut bus = EventBus::new();
        bus.create_topic("t");
        let sub = bus.subscribe("t").unwrap();
        bus.unsubscribe(sub);
        // After unsubscribe, drain should return UnknownSubscriber
        let err = bus.drain(sub).unwrap_err();
        assert_eq!(err, BusError::UnknownSubscriber);
    }

    #[test]
    fn test_multiple_subscribers() {
        let mut bus = EventBus::new();
        bus.create_topic("t");
        let s1 = bus.subscribe("t").unwrap();
        let s2 = bus.subscribe("t").unwrap();
        let n = bus.publish("t", b"hi".to_vec(), 0).unwrap();
        assert_eq!(n, 2);
        assert_eq!(bus.drain(s1).unwrap().len(), 1);
        assert_eq!(bus.drain(s2).unwrap().len(), 1);
    }
}
