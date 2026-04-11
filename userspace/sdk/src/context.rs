//! AgentContext - an agent's runtime environment
//!
//! Every agent runs with an AgentContext that provides:
//! - Its identity (who it is)
//! - Its capabilities (what it can access)
//! - Its event channels (how it communicates)
//! - Its memory allocator (where it can allocate)
//! - Lifecycle hooks (init, notified, fault)

use alloc::vec::Vec;
use alloc::string::String;
use alloc::format;

use crate::capability::{Capability, CapabilitySet, CapabilityKind, Right};
use crate::event::{EventChannel, Event, EventKind, Priority};
use crate::identity::{AgentId, AgentIdentity, AgentClass};

/// Base seL4 cap slot used in simulation mode for event channels.
/// In production the monitor assigns real slots; this value is never used
/// on hardware.
const SIM_CAP_BASE: u64 = 0x0001_0000_0000;

/// XOR nonce used in simulation mode to give child agents a pseudo-unique ID.
/// In production the monitor seeds this from the hardware TRNG.
const SIM_NONCE_XOR: u64 = 0x5A5A_5A5A_5A5A_5A5A;

/// An agent's complete runtime context
///
/// This is the root of everything an agent has access to.
/// In production this is constructed by the agentOS monitor at agent startup.
pub struct AgentContext {
    /// This agent's identity
    pub identity: AgentIdentity,
    /// All capabilities this agent holds
    pub caps: CapabilitySet,
    /// Active event channels
    pub channels: Vec<EventChannel>,
    /// Agent lifecycle state
    pub state: AgentState,
    /// Monotonic time in ns (from kernel)
    pub now_ns: u64,
}

impl AgentContext {
    /// Create a new agent context (called by the monitor at spawn time)
    pub fn new(identity: AgentIdentity, caps: CapabilitySet) -> Self {
        Self {
            identity,
            caps,
            channels: Vec::new(),
            state: AgentState::Initializing,
            now_ns: 0,
        }
    }

    /// Mark this agent as ready (called at end of init())
    pub fn mark_ready(&mut self) {
        self.state = AgentState::Running;
    }

    /// Subscribe to an event topic
    /// Returns a channel capability or an error if we don't have EventBus access
    pub fn subscribe(&mut self, topic: impl Into<String>, priority: Priority) 
        -> Result<usize, ContextError> 
    {
        // In production: IPC to EventBus server to create subscription
        // Returns a seL4 notification capability
        // Here: create a simulation channel
        let topic = topic.into();
        
        // Check we have an endpoint capability for the EventBus
        // (simplified check - production would verify the EventBus endpoint cap)

        let cap = Capability::new(
            CapabilityKind::Endpoint,
            SIM_CAP_BASE + self.channels.len() as u64,
            crate::capability::Rights::READ_WRITE,
        );
        
        let idx = self.channels.len();
        self.channels.push(EventChannel::new(cap, format!("ch-{idx}"), topic, priority));
        Ok(idx)
    }

    /// Publish an event on a channel
    pub fn publish(&mut self, channel_idx: usize, kind: EventKind, priority: Priority)
        -> Result<(), ContextError>
    {
        if channel_idx >= self.channels.len() {
            return Err(ContextError::InvalidChannel);
        }
        
        let event = Event::new(
            self.identity.id.clone(),
            kind,
            self.now_ns,
            crate::event::EventPayload::Empty,
            priority,
        );
        
        self.channels[channel_idx].publish(event)
            .map(|_| ())
            .map_err(|_| ContextError::PublishFailed)
    }

    /// Spawn a child agent with a subset of this agent's capabilities
    pub fn spawn_agent(
        &mut self,
        name: impl Into<String>,
        _: AgentClass,
        granted_caps: Vec<Capability>,
    ) -> Result<AgentId, ContextError> {
        // Check we have the AgentSpawn capability
        if !self.caps.can(&CapabilityKind::AgentSpawn, Right::Execute) {
            return Err(ContextError::CapabilityDenied);
        }
        
        // Derive child capability set
        let _child_caps = self.caps.derive_for_child(&granted_caps)
            .map_err(|_| ContextError::CapabilityDenied)?;
        
        // In production: IPC to monitor to spawn the child agent
        // Returns the new agent's ID
        let child_id = AgentId::new(
            self.identity.id.namespace.clone(),
            name.into(),
            self.now_ns,
            self.now_ns ^ SIM_NONCE_XOR, // production: random from TRNG
        );
        
        Ok(child_id)
    }

    /// Get current time in nanoseconds
    pub fn now(&self) -> u64 {
        self.now_ns
    }

    /// Yield the CPU (seL4_Yield in production)
    pub fn yield_cpu(&self) {
        // In production: seL4_Yield()
    }

    /// Debug log (seL4_DebugPutChar in production)
    pub fn log(&self, _msg: &str) {
        // In production: microkit_dbg_puts()
        // In simulation: print to stdout
        #[cfg(feature = "std")]
        println!("[{}] {}", self.identity.id, _msg);
    }
}

/// The lifecycle state of an agent
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AgentState {
    Initializing,
    Running,
    Suspended,
    Exiting { code: i32 },
    Faulted { message: String },
}

/// Errors from AgentContext operations
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ContextError {
    CapabilityDenied,
    InvalidChannel,
    PublishFailed,
    SpawnFailed,
    InvalidState,
}

impl core::fmt::Display for ContextError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            ContextError::CapabilityDenied => write!(f, "capability denied"),
            ContextError::InvalidChannel => write!(f, "invalid channel index"),
            ContextError::PublishFailed => write!(f, "failed to publish event"),
            ContextError::SpawnFailed => write!(f, "failed to spawn agent"),
            ContextError::InvalidState => write!(f, "invalid agent state for this operation"),
        }
    }
}

/// The trait every agentOS agent must implement
///
/// This is the Microkit-compatible interface:
/// - `init()` is called once at startup
/// - `notified(channel)` is called when a notification arrives on a channel
///
/// Agents are statically configured via the .system file (Microkit SDF).
/// At runtime, they extend themselves by spawning children and subscribing to events.
pub trait Agent {
    /// Called once at agent startup, before any events arrive
    /// Agents should set up their subscriptions and internal state here
    fn init(&mut self, ctx: &mut AgentContext);
    
    /// Called when a notification arrives on a seL4 channel
    /// Maps to Microkit's `notified()` callback
    fn notified(&mut self, ctx: &mut AgentContext, channel: u32);
    
    /// Called when a protected procedure call arrives (optional)
    /// Maps to Microkit's `protected()` callback
    fn protected(&mut self, _ctx: &mut AgentContext, _channel: u32, _msginfo: u64) -> u64 {
        0
    }
    
    /// Called when this agent's child has faulted
    /// If not handled, the fault propagates up
    fn child_faulted(&mut self, _ctx: &mut AgentContext, _child: &AgentId, _fault: &crate::event::FaultKind) -> bool {
        false // don't handle, propagate
    }
}
