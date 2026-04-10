//! agentOS WASM agent simulator.
//!
//! Provides a complete host-side simulation environment for running
//! signed `.wasm` agents without real seL4 hardware:
//!
//! ```text
//!   ┌─────────────────────────────────────────────────────────┐
//!   │  SimEngine                                              │
//!   │   ├─ SimCapStore   — capability enforcement             │
//!   │   ├─ SimEventBus   — topic pub/sub                     │
//!   │   └─ AgentRunner[] — per-agent wasmi instances         │
//!   │        └─ MicrokitShim — mock IPC + notifications      │
//!   └─────────────────────────────────────────────────────────┘
//! ```
//!
//! ## Quick start
//!
//! ```no_run
//! use agentos_sim::SimEngine;
//!
//! let mut engine = SimEngine::new();
//! engine.caps_mut().grant_defaults("my-agent");
//! let wasm = std::fs::read("my_agent.wasm").unwrap();
//! let mut runner = engine.spawn_agent("my-agent", &wasm).unwrap();
//! runner.init().unwrap();
//! println!("health: {}", runner.health_check().unwrap());
//! ```

pub mod caps;
pub mod eventbus;
pub mod microkit;
pub mod runner;

use std::sync::{Arc, Mutex};
use anyhow::Result;

pub use caps::SimCapStore;
pub use eventbus::SimEventBus;
pub use microkit::{MicrokitShim, MsgInfo, CapturedCall, PpcResult};
pub use runner::{AgentRunner, AgentState};

/// Top-level simulation engine.  Owns shared state (caps + eventbus) and
/// spawns agents that all share the same event fabric.
pub struct SimEngine {
    caps:     Arc<Mutex<SimCapStore>>,
    eventbus: Arc<Mutex<SimEventBus>>,
}

impl SimEngine {
    pub fn new() -> Self {
        Self {
            caps:     Arc::new(Mutex::new(SimCapStore::new())),
            eventbus: Arc::new(Mutex::new(SimEventBus::new())),
        }
    }

    /// Mutable access to the capability store (grant/revoke before spawning).
    pub fn caps_mut(&self) -> std::sync::MutexGuard<'_, SimCapStore> {
        self.caps.lock().unwrap()
    }

    /// Mutable access to the event bus (inject events, add subscribers).
    pub fn eventbus_mut(&self) -> std::sync::MutexGuard<'_, SimEventBus> {
        self.eventbus.lock().unwrap()
    }

    /// Compile and instantiate a WASM agent.
    ///
    /// The agent's `AgentState` is pre-populated with default capabilities
    /// unless you call [`caps_mut().grant_defaults`] beforehand.
    pub fn spawn_agent(&self, name: &str, wasm_bytes: &[u8]) -> Result<AgentRunner> {
        let state = AgentState::new(name, self.eventbus.clone(), self.caps.clone());
        AgentRunner::new(wasm_bytes, state)
    }

    /// Arc clone of the eventbus (for injecting events from test code).
    pub fn eventbus_arc(&self) -> Arc<Mutex<SimEventBus>> {
        self.eventbus.clone()
    }

    /// Arc clone of the caps store.
    pub fn caps_arc(&self) -> Arc<Mutex<SimCapStore>> {
        self.caps.clone()
    }
}

impl Default for SimEngine {
    fn default() -> Self { Self::new() }
}
