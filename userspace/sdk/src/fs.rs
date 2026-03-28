//! AgentFS - The agent-native filesystem
//!
//! AgentFS is NOT POSIX. It doesn't pretend to be.
//!
//! Everything is an **Object**:
//! - Uniquely addressed by content hash (like git objects)
//! - Versioned by default (every write creates a new version)
//! - Capability-gated (you need a capability to access any object)
//! - Metadata-rich (agents annotate objects with typed metadata)
//! - Event-emitting (every mutation generates an event on the EventBus)
//!
//! ## Namespace Structure
//!
//! ```
//! /agents/<agent-id>/          - agent's private namespace
//! /agents/<agent-id>/context   - serialized agent context (checkpoint)
//! /agents/<agent-id>/logs/     - agent's structured log
//! /shared/<namespace>/         - shared namespaces (cap-gated)
//! /system/                     - system objects (read-only for most agents)
//! /tmp/<agent-id>/             - ephemeral scratch space
//! ```
//!
//! ## Backends
//!
//! The AgentFS namespace is served by pluggable backends:
//! - **BlobStore**: content-addressed, immutable objects (the default)
//! - **MutableStore**: mutable objects with conflict-free versioning
//! - **EphemeralStore**: RAM-backed, cleared on reboot
//! - **External**: agent-provided backend (vibe-coded!)
//!
//! Agents register backends by implementing the `AgentFsBackend` trait
//! and calling `AgentFs::register_backend()`.

use alloc::string::String;
use alloc::vec::Vec;

/// A unique object identifier
///
/// In the BlobStore, this is the BLAKE3 hash of the object content.
/// In the MutableStore, this is a UUID assigned at creation.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct ObjectId {
    /// The addressing scheme used
    pub scheme: IdScheme,
    /// The raw identifier bytes
    pub bytes: [u8; 32],
}

impl ObjectId {
    pub fn from_hash(hash: [u8; 32]) -> Self {
        Self { scheme: IdScheme::Blake3, bytes: hash }
    }
    
    pub fn from_uuid(uuid: u128) -> Self {
        let mut bytes = [0u8; 32];
        bytes[..16].copy_from_slice(&uuid.to_le_bytes());
        Self { scheme: IdScheme::Uuid, bytes }
    }
    
    /// Null/zero object ID (used as placeholder)
    pub const NULL: ObjectId = ObjectId {
        scheme: IdScheme::Null,
        bytes: [0u8; 32],
    };
}

impl core::fmt::Display for ObjectId {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let prefix = match self.scheme {
            IdScheme::Blake3 => "b3",
            IdScheme::Uuid => "uuid",
            IdScheme::Null => "null",
        };
        write!(f, "{}:", prefix)?;
        for b in &self.bytes[..8] { // show first 8 bytes
            write!(f, "{b:02x}")?;
        }
        write!(f, "...")
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub enum IdScheme {
    Blake3,
    Uuid,
    Null,
}

/// An object in AgentFS
#[derive(Debug, Clone)]
pub struct Object {
    /// Unique identifier
    pub id: ObjectId,
    /// Object metadata
    pub meta: ObjectMeta,
    /// The object's data
    pub data: ObjectData,
}

impl Object {
    /// Create a new object with the given data
    pub fn new(data: impl Into<Vec<u8>>, meta: ObjectMeta) -> Self {
        let data = data.into();
        // In production: BLAKE3 hash of data
        let id = ObjectId::from_hash([0u8; 32]); // placeholder
        Self {
            id,
            meta,
            data: ObjectData::Inline(data),
        }
    }
    
    /// Create an object reference (cap to data in a large object store)
    pub fn reference(id: ObjectId, meta: ObjectMeta, size: u64) -> Self {
        Self {
            id,
            meta,
            data: ObjectData::Reference { size },
        }
    }
    
    pub fn size(&self) -> u64 {
        match &self.data {
            ObjectData::Inline(v) => v.len() as u64,
            ObjectData::Reference { size } => *size,
        }
    }
}

/// Object metadata - richly typed annotations
#[derive(Debug, Clone)]
pub struct ObjectMeta {
    /// MIME type of the content
    pub content_type: String,
    /// Human-readable description
    pub description: Option<String>,
    /// Creator's agent ID
    pub creator: String,
    /// Creation time (ns since agentOS epoch)
    pub created_at_ns: u64,
    /// Last modification time
    pub modified_at_ns: u64,
    /// Previous version's ObjectId (for mutable objects)
    pub previous_version: Option<ObjectId>,
    /// Arbitrary typed tags
    pub tags: Vec<ObjectTag>,
    /// Access control: which capabilities can access this
    pub access_control: AccessControl,
}

impl ObjectMeta {
    pub fn new(content_type: impl Into<String>, creator: impl Into<String>, now_ns: u64) -> Self {
        Self {
            content_type: content_type.into(),
            description: None,
            creator: creator.into(),
            created_at_ns: now_ns,
            modified_at_ns: now_ns,
            previous_version: None,
            tags: Vec::new(),
            access_control: AccessControl::OwnerOnly,
        }
    }
    
    pub fn with_description(mut self, desc: impl Into<String>) -> Self {
        self.description = Some(desc.into());
        self
    }
    
    pub fn with_tag(mut self, tag: ObjectTag) -> Self {
        self.tags.push(tag);
        self
    }
    
    pub fn public(mut self) -> Self {
        self.access_control = AccessControl::Public;
        self
    }
}

/// A typed metadata tag on an object
#[derive(Debug, Clone)]
pub enum ObjectTag {
    /// Text label
    Label { key: String, value: String },
    /// Numeric metric
    Metric { key: String, value: f64 },
    /// Reference to another object
    Reference { key: String, id: ObjectId },
    /// Embedding vector reference (links to VectorStore)
    Embedding { vector_id: String },
}

/// Object access control policy
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AccessControl {
    /// Only the owning agent can access
    OwnerOnly,
    /// Any agent with the ObjectStore capability can access
    Public,
    /// Access controlled by a specific capability badge
    CapabilityGated { badge: u64 },
}

/// The actual data of an object
#[derive(Debug, Clone)]
pub enum ObjectData {
    /// Data is inline (small objects)
    Inline(Vec<u8>),
    /// Data is stored externally (large objects - use the ObjectId to fetch)
    Reference { size: u64 },
}

/// The AgentFS interface - how agents interact with the filesystem
pub struct AgentFs {
    backend: FsBackend,
    namespace: String,
}

impl AgentFs {
    pub fn new(namespace: impl Into<String>) -> Self {
        Self {
            backend: FsBackend::Ephemeral(EphemeralStore::new()),
            namespace: namespace.into(),
        }
    }
    
    /// Store an object, return its ID
    pub fn put(&mut self, obj: Object) -> Result<ObjectId, FsError> {
        match &mut self.backend {
            FsBackend::Ephemeral(store) => store.put(obj),
            FsBackend::External(_) => Err(FsError::Unimplemented),
        }
    }
    
    /// Retrieve an object by ID
    pub fn get(&self, id: &ObjectId) -> Result<Object, FsError> {
        match &self.backend {
            FsBackend::Ephemeral(store) => store.get(id),
            FsBackend::External(_) => Err(FsError::Unimplemented),
        }
    }
    
    /// List objects in this namespace
    pub fn list(&self) -> Result<Vec<ObjectId>, FsError> {
        match &self.backend {
            FsBackend::Ephemeral(store) => Ok(store.list()),
            FsBackend::External(_) => Err(FsError::Unimplemented),
        }
    }
    
    /// Delete an object
    pub fn delete(&mut self, id: &ObjectId) -> Result<(), FsError> {
        match &mut self.backend {
            FsBackend::Ephemeral(store) => store.delete(id),
            FsBackend::External(_) => Err(FsError::Unimplemented),
        }
    }
}

/// The trait for pluggable AgentFS backends
///
/// Agents implement this to provide their own storage backend.
/// Register via AgentFs::register_backend().
pub trait AgentFsBackend: Send + Sync {
    fn put(&mut self, obj: Object) -> Result<ObjectId, FsError>;
    fn get(&self, id: &ObjectId) -> Result<Object, FsError>;
    fn list(&self) -> Vec<ObjectId>;
    fn delete(&mut self, id: &ObjectId) -> Result<(), FsError>;
    fn flush(&mut self) -> Result<(), FsError>;
}

enum FsBackend {
    Ephemeral(EphemeralStore),
    External(alloc::boxed::Box<dyn AgentFsBackend>),
}

/// In-memory ephemeral store (clears on reboot)
struct EphemeralStore {
    objects: Vec<(ObjectId, Object)>,
}

impl EphemeralStore {
    fn new() -> Self {
        Self { objects: Vec::new() }
    }
    
    fn put(&mut self, obj: Object) -> Result<ObjectId, FsError> {
        let id = obj.id.clone();
        // Remove existing if same ID
        self.objects.retain(|(oid, _)| oid != &id);
        self.objects.push((id.clone(), obj));
        Ok(id)
    }
    
    fn get(&self, id: &ObjectId) -> Result<Object, FsError> {
        self.objects.iter()
            .find(|(oid, _)| oid == id)
            .map(|(_, obj)| obj.clone())
            .ok_or(FsError::NotFound)
    }
    
    fn list(&self) -> Vec<ObjectId> {
        self.objects.iter().map(|(id, _)| id.clone()).collect()
    }
    
    fn delete(&mut self, id: &ObjectId) -> Result<(), FsError> {
        let before = self.objects.len();
        self.objects.retain(|(oid, _)| oid != id);
        if self.objects.len() == before {
            Err(FsError::NotFound)
        } else {
            Ok(())
        }
    }
}

/// Errors from AgentFS
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FsError {
    NotFound,
    PermissionDenied,
    QuotaExceeded,
    CorruptData,
    BackendError(String),
    Unimplemented,
}

impl core::fmt::Display for FsError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            FsError::NotFound => write!(f, "object not found"),
            FsError::PermissionDenied => write!(f, "permission denied"),
            FsError::QuotaExceeded => write!(f, "storage quota exceeded"),
            FsError::CorruptData => write!(f, "data corruption detected"),
            FsError::BackendError(e) => write!(f, "backend error: {e}"),
            FsError::Unimplemented => write!(f, "operation not implemented by backend"),
        }
    }
}
