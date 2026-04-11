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

/// Per-topic subscriber list with ownership tracking
#[derive(Debug, Default)]
struct TopicEntry {
    subscribers: Vec<SubscriberId>,
    /// The protection-domain ID that owns this topic.
    /// None means the topic is unowned (created before ownership was introduced).
    /// Owner 0 is the kernel / privileged layer.
    owner_pd: Option<u32>,
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
    /// Publisher is not the owner of the topic
    Unauthorized,
    /// Subscriber queue is full — publisher must back off
    QueueFull,
}

impl core::fmt::Display for BusError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            BusError::UnknownTopic => write!(f, "unknown topic"),
            BusError::UnknownSubscriber => write!(f, "unknown subscriber"),
            BusError::PayloadTooLarge => write!(f, "payload too large"),
            BusError::Unauthorized => write!(f, "publisher is not the topic owner"),
            BusError::QueueFull => write!(f, "subscriber queue full — publisher must back off"),
        }
    }
}

/// Maximum event payload in bytes (matches seL4 IPC message register budget)
pub const MAX_PAYLOAD_BYTES: usize = 512;

/// Maximum number of pending events per subscriber before backpressure kicks in
pub const MAX_PENDING_PER_SUBSCRIBER: usize = 256;

impl EventBus {
    pub fn new() -> Self {
        Self::default()
    }

    /// Create a topic owned by the kernel (owner PD = 0).  Idempotent.
    pub fn create_topic(&mut self, topic: impl Into<Topic>) {
        self.create_topic_owned(0, topic);
    }

    /// Create a topic owned by `owner_pd`.  Idempotent — if the topic already
    /// exists the call is a no-op (first creator wins).
    pub fn create_topic_owned(&mut self, owner_pd: u32, topic: impl Into<Topic>) {
        self.topics.entry(topic.into()).or_insert_with(|| TopicEntry {
            subscribers: Vec::new(),
            owner_pd: Some(owner_pd),
        });
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

    /// Publish an event as the kernel (publisher PD = 0).
    ///
    /// In production this would signal seL4 Notification caps.
    /// Here events are queued and retrieved via `drain`.
    pub fn publish(
        &mut self,
        topic: impl Into<Topic>,
        payload: Vec<u8>,
        timestamp_ns: u64,
    ) -> Result<usize, BusError> {
        self.publish_as(0, topic, payload, timestamp_ns)
    }

    /// Publish an event as a specific protection domain.
    ///
    /// Returns `BusError::Unauthorized` if `publisher_pd` does not own the topic.
    /// Returns `BusError::QueueFull` if any subscriber's pending queue has reached
    /// `MAX_PENDING_PER_SUBSCRIBER`.
    pub fn publish_as(
        &mut self,
        publisher_pd: u32,
        topic: impl Into<Topic>,
        payload: Vec<u8>,
        timestamp_ns: u64,
    ) -> Result<usize, BusError> {
        if payload.len() > MAX_PAYLOAD_BYTES {
            return Err(BusError::PayloadTooLarge);
        }
        let topic = topic.into();
        let entry = self.topics.get(&topic).ok_or(BusError::UnknownTopic)?;

        // Ownership check: an owned topic may only be published to by its owner.
        if entry.owner_pd.map_or(false, |o| o != publisher_pd) {
            return Err(BusError::Unauthorized);
        }

        let subs = entry.subscribers.clone();

        // Backpressure check: fail fast if any subscriber queue is at capacity.
        for sub in &subs {
            if self.pending.get(sub).map_or(0, |v| v.len()) >= MAX_PENDING_PER_SUBSCRIBER {
                return Err(BusError::QueueFull);
            }
        }

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

    #[test]
    fn test_topic_ownership_prevents_squatting() {
        let mut bus = EventBus::new();
        // PD 42 creates and owns "secure.topic"
        bus.create_topic_owned(42, "secure.topic");
        let sub = bus.subscribe("secure.topic").unwrap();

        // Owner can publish successfully
        let n = bus.publish_as(42, "secure.topic", b"from owner".to_vec(), 1).unwrap();
        assert_eq!(n, 1);
        let events = bus.drain(sub).unwrap();
        assert_eq!(events[0].payload, b"from owner");

        // Another PD (99) attempting to publish is rejected
        let sub2 = bus.subscribe("secure.topic").unwrap();
        let err = bus.publish_as(99, "secure.topic", b"squatter".to_vec(), 2).unwrap_err();
        assert_eq!(err, BusError::Unauthorized);

        // Drain should be empty (nothing was delivered from the squatter)
        let nothing = bus.drain(sub2).unwrap();
        assert!(nothing.is_empty());
    }

    #[test]
    fn test_queue_full_backpressure() {
        let mut bus = EventBus::new();
        bus.create_topic("flood");
        let sub = bus.subscribe("flood").unwrap();

        // Fill up to the limit
        for i in 0..MAX_PENDING_PER_SUBSCRIBER {
            bus.publish("flood", alloc::format!("msg {}", i).into_bytes(), i as u64)
                .expect("should succeed before queue full");
        }

        // The next publish should fail with QueueFull
        let err = bus
            .publish("flood", b"overflow".to_vec(), MAX_PENDING_PER_SUBSCRIBER as u64)
            .unwrap_err();
        assert_eq!(err, BusError::QueueFull);

        // After draining, publishing works again
        let drained = bus.drain(sub).unwrap();
        assert_eq!(drained.len(), MAX_PENDING_PER_SUBSCRIBER);

        let sub2 = bus.subscribe("flood").unwrap();
        bus.publish("flood", b"after drain".to_vec(), 0).unwrap();
        assert_eq!(bus.drain(sub2).unwrap().len(), 1);
    }
}
