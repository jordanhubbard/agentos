//! # agentOS SDK
//!
//! Core abstractions for the world's first operating system designed for AI agents.
//!
//! ## Design Philosophy
//!
//! Traditional OS primitives were designed for human-driven processes.
//! agentOS inverts this: every primitive is designed around what agents actually need.
//!
//! ### Core Abstractions
//!
//! - **Capability**: Unforgeable access token to a resource (seL4 kernel-enforced)
//! - **AgentContext**: An agent's runtime environment (caps, memory, identity)
//! - **Message**: Typed, schema-validated inter-agent communication
//! - **Object**: Addressable, versioned, capability-gated artifact in AgentFS
//! - **EventChannel**: Pub/sub communication primitive
//! - **VectorRef**: Handle to an embedding in the native VectorStore

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

pub mod agent_context;
pub mod capability;
pub mod context;
pub mod cuda;
pub mod event;
pub mod fs;
pub mod identity;
pub mod message;
pub mod memory;
pub mod net;
pub mod scheduler;
pub mod vector;

// Re-export the most common types
pub use capability::{Capability, CapabilitySet, CapabilityKind};
pub use context::AgentContext;
pub use event::{EventChannel, Event, EventKind, Priority};
pub use identity::{AgentId, AgentIdentity};
pub use message::{Message, MessageKind};
pub use cuda::{CudaKernel, CudaError, CUDA_SECTION_NAME};
pub use fs::{Object, ObjectId, ObjectMeta};

/// agentOS SDK version
pub const VERSION: &str = env!("CARGO_PKG_VERSION");

/// agentOS kernel interface version this SDK targets
pub const KERNEL_ABI_VERSION: u32 = 1;
