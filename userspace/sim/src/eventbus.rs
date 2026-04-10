//! Simulated EventBus — topic-based pub/sub matching the real event_bus PD behaviour.

use std::collections::HashMap;

/// A single published event.
#[derive(Debug, Clone)]
pub struct SimEvent {
    pub topic:   String,
    pub payload: Vec<u8>,
    /// Which agent/slot published this event (None = external inject)
    pub source:  Option<String>,
}

/// The simulation EventBus.
///
/// Subscribers register closures keyed by topic glob (`"*"` matches everything).
/// Published events are delivered synchronously; history is kept for test
/// assertions.
pub struct SimEventBus {
    /// Ordered publication history
    pub history: Vec<SimEvent>,

    /// Per-topic subscriber lists.  `"*"` subscribers receive every event.
    subscribers: HashMap<String, Vec<Box<dyn FnMut(&SimEvent) + Send>>>,
}

impl SimEventBus {
    pub fn new() -> Self {
        Self {
            history:     Vec::new(),
            subscribers: HashMap::new(),
        }
    }

    /// Subscribe to a topic.  Use `"*"` to receive every event.
    pub fn subscribe<F>(&mut self, topic: impl Into<String>, handler: F)
    where F: FnMut(&SimEvent) + Send + 'static,
    {
        self.subscribers
            .entry(topic.into())
            .or_default()
            .push(Box::new(handler));
    }

    /// Publish an event; delivers to exact-match and wildcard subscribers.
    pub fn publish(&mut self, topic: impl Into<String>, payload: Vec<u8>, source: Option<String>) {
        let event = SimEvent { topic: topic.into(), payload, source };

        // Collect matching keys first to avoid borrow split
        let keys: Vec<String> = self.subscribers.keys()
            .filter(|k| k.as_str() == "*" || k.as_str() == event.topic)
            .cloned()
            .collect();

        for key in keys {
            if let Some(subs) = self.subscribers.get_mut(&key) {
                for sub in subs.iter_mut() {
                    sub(&event);
                }
            }
        }

        self.history.push(event);
    }

    /// Return all events published on `topic` (exact match).
    pub fn events_for(&self, topic: &str) -> Vec<&SimEvent> {
        self.history.iter().filter(|e| e.topic == topic).collect()
    }

    /// Number of events published in total.
    pub fn total_events(&self) -> usize {
        self.history.len()
    }
}

impl Default for SimEventBus {
    fn default() -> Self { Self::new() }
}
