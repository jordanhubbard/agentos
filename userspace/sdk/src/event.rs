//! EventBus - the nervous system of agentOS
//!
//! Agents communicate primarily through typed, schema-validated events.
//! The EventBus is not just a message queue — it's a capability-gated pub/sub
//! fabric with durability, replay, and priority classes built in.
//!
//! ## Priority Classes
//!
//! - **HardRealTime**: Delivered in bounded time (< 1ms). Used for control signals.
//! - **SoftRealTime**: Best-effort low latency (< 10ms). Interactive events.
//! - **DelayTolerant**: Background delivery, can be batched.
//!
//! ## Durability
//!
//! Events can be durable (persisted to AgentFS) or ephemeral (in-memory only).
//! Durable events survive agent restarts and can be replayed.

use alloc::string::String;
use alloc::vec::Vec;

use crate::identity::AgentId;
use crate::capability::Capability;

/// Priority class for event delivery
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Priority {
    /// Hard real-time: delivery within 1ms, kernel-assisted scheduling
    HardRealTime = 3,
    /// Soft real-time: delivery within 10ms  
    SoftRealTime = 2,
    /// Best-effort: delivery when resources permit
    BestEffort = 1,
    /// Background: batched, low-priority
    Background = 0,
}

/// Event kinds - the vocabulary of agentOS inter-agent communication
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EventKind {
    // System events (emitted by agentOS core)
    AgentSpawned,
    AgentExited { exit_code: i32 },
    AgentFaulted { fault: FaultKind },
    CapabilityGranted,
    CapabilityRevoked,
    MemoryPressure { level: u8 },  // 0-255, 255 = critical
    SchedulerTick,
    
    // Agent-defined events (custom schemas)
    Custom { kind: String },
    
    // AgentFS events
    ObjectCreated,
    ObjectUpdated,
    ObjectDeleted,
    
    // VectorStore events
    EmbeddingAdded,
    EmbeddingDeleted,
    
    // Network events
    ConnectionEstablished,
    ConnectionClosed,
    
    // Inference events
    InferenceStarted,
    InferenceComplete { latency_ms: u64 },
    ContextWindowNearFull { used_pct: u8 },
}

/// A fault kind - what went wrong with an agent
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FaultKind {
    UserException { code: u64 },
    VmFault { addr: u64, write: bool },
    NullDereference,
    CapabilityFault { cptr: u64 },
    Timeout,
    Panic { message: String },
}

/// An event - the fundamental unit of communication
#[derive(Debug, Clone)]
pub struct Event {
    /// Unique event ID (content-addressed if durable)
    pub id: EventId,
    /// Who emitted this event
    pub source: AgentId,
    /// What happened
    pub kind: EventKind,
    /// When this happened (ns since agentOS epoch)
    pub timestamp_ns: u64,
    /// The event payload (schema defined by EventKind)
    pub payload: EventPayload,
    /// Priority class
    pub priority: Priority,
    /// Whether this event is durable (persisted to AgentFS)
    pub durable: bool,
    /// Optional correlation ID (links related events)
    pub correlation_id: Option<EventId>,
}

impl Event {
    pub fn new(
        source: AgentId,
        kind: EventKind,
        timestamp_ns: u64,
        payload: EventPayload,
        priority: Priority,
    ) -> Self {
        // In production: ID is derived from a seL4 monotonic counter + agent badge
        // Here we use a placeholder
        let id = EventId(timestamp_ns ^ (source.random));
        Self {
            id,
            source,
            kind,
            timestamp_ns,
            payload,
            priority,
            durable: false,
            correlation_id: None,
        }
    }

    pub fn durable(mut self) -> Self {
        self.durable = true;
        self
    }

    pub fn correlate(mut self, id: EventId) -> Self {
        self.correlation_id = Some(id);
        self
    }
}

/// An event identifier
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct EventId(pub u64);

impl core::fmt::Display for EventId {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "evt:{:016x}", self.0)
    }
}

/// Event payload - the data carried by an event
#[derive(Debug, Clone)]
pub enum EventPayload {
    Empty,
    Bytes(Vec<u8>),
    // In production: this would be a cap to a shared memory region
    // for zero-copy large payloads
    Inline { data: Vec<u8>, schema: String },
}

/// A channel for subscribing to and publishing events
///
/// EventChannels are backed by seL4 notification objects.
/// The channel capability controls who can publish and who can subscribe.
pub struct EventChannel {
    /// The capability backing this channel
    pub cap: Capability,
    /// Channel name (human-readable)
    pub name: String,
    /// Topic this channel is subscribed to
    pub topic: String,
    /// Priority class for this channel
    pub priority: Priority,
    /// Pending events (ring buffer in production)
    pending: Vec<Event>,
}

impl EventChannel {
    pub fn new(cap: Capability, name: impl Into<String>, topic: impl Into<String>, priority: Priority) -> Self {
        Self {
            cap,
            name: name.into(),
            topic: topic.into(),
            priority,
            pending: Vec::new(),
        }
    }

    /// Publish an event to this channel
    /// In production: IPC call to EventBus server
    pub fn publish(&mut self, event: Event) -> Result<EventId, EventError> {
        let id = event.id;
        self.pending.push(event);
        Ok(id)
    }

    /// Poll for a pending event (non-blocking)
    pub fn poll(&mut self) -> Option<Event> {
        if self.pending.is_empty() {
            None
        } else {
            Some(self.pending.remove(0))
        }
    }

    /// Wait for an event (blocking - seL4 seL4_Wait on notification object)
    /// In production: calls seL4_Wait on the notification capability
    pub fn wait(&mut self) -> Event {
        // Simulation: busy-wait (production uses seL4_Wait)
        loop {
            if let Some(event) = self.poll() {
                return event;
            }
            // In production: seL4_Yield() here
        }
    }
}

/// Errors from the EventBus
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EventError {
    ChannelFull,
    NotSubscribed,
    InvalidPayload,
    CapabilityDenied,
    ChannelClosed,
}

impl core::fmt::Display for EventError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            EventError::ChannelFull => write!(f, "event channel is full"),
            EventError::NotSubscribed => write!(f, "not subscribed to this channel"),
            EventError::InvalidPayload => write!(f, "invalid event payload"),
            EventError::CapabilityDenied => write!(f, "capability denied for channel"),
            EventError::ChannelClosed => write!(f, "event channel is closed"),
        }
    }
}
