//! AgentFS — Content-Addressable Filesystem for agentOS
//!
//! Unlike traditional filesystems designed for human file hierarchies,
//! AgentFS is built for how agents actually work:
//!
//! - **Content-addressable**: Objects are stored by SHA-256 hash. Dedup is automatic.
//! - **Versioned**: Every write creates a new version. Agents can checkpoint and restore.
//! - **Namespaced**: Each agent has a capability-gated namespace. Cross-namespace access
//!   requires explicit capability grants.
//! - **Semantic search**: Objects have optional embeddings. Agents can find related content
//!   by semantic similarity, not just by path.
//! - **Structured data native**: First-class CBOR support. Agents think in structured data,
//!   not raw byte streams.
//! - **Provenance tracking**: Every object records who created it, when, and from what context.
//!
//! ## Architecture
//!
//! AgentFS is a Microkit Protection Domain (userspace server).
//! It implements the `ObjectStoreCap` capability interface from the SDK.
//! Agents interact via IPC (PPC for synchronous ops, notifications for async events).
//!
//! Storage layers (pluggable):
//! - RAM (default): Fast, volatile. Good for working memory.
//! - VirtIO-blk: Persistent. When hardware is available.
//! - Network-backed: S3/MinIO proxy. For shared storage across nodes.
//!
//! The key insight: agents don't do "open file, read bytes, close file."
//! They do "store this artifact, find related artifacts, checkpoint my state."
//! AgentFS is designed for the latter.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

// ============================================================================
// Object — the fundamental storage unit
// ============================================================================

/// Content hash (SHA-256)
pub type ContentHash = [u8; 32];

/// Object version identifier
pub type VersionId = u64;

/// An object in AgentFS. Immutable once written (new versions create new objects).
#[derive(Debug, Clone)]
pub struct FsObject {
    /// Content hash (SHA-256 of content bytes)
    pub hash: ContentHash,
    /// Object content
    pub content: Vec<u8>,
    /// Object metadata
    pub meta: FsObjectMeta,
    /// Version chain — hash of previous version (None if first)
    pub parent_version: Option<ContentHash>,
}

/// Object metadata — the stuff that makes AgentFS agent-native
#[derive(Debug, Clone)]
pub struct FsObjectMeta {
    /// Human/agent-readable key (like a filename, but more flexible)
    pub key: String,
    /// MIME type (or custom schema URI)
    pub content_type: String,
    /// Agent that created this object
    pub creator: [u8; 32],
    /// Creation timestamp (microseconds since boot)
    pub created_at: u64,
    /// Semantic tags (free-form)
    pub tags: Vec<String>,
    /// Optional embedding for semantic search
    pub embedding: Option<Vec<f32>>,
    /// Size in bytes
    pub size: u64,
    /// Version number within the key's history
    pub version: VersionId,
    /// Custom metadata (CBOR-encoded, agent-defined)
    pub custom: Vec<u8>,
}

// ============================================================================
// Namespace — per-agent isolated storage
// ============================================================================

/// A capability-gated namespace. Each agent gets one by default.
/// Cross-namespace access requires explicit capability grants.
#[derive(Debug)]
pub struct Namespace {
    /// Namespace identifier (usually derived from AgentID)
    pub name: String,
    /// Owner agent
    pub owner: [u8; 32],
    /// Key -> latest hash mapping
    pub index: BTreeMap<String, ContentHash>,
    /// Key -> version history (ordered, latest first)
    pub versions: BTreeMap<String, Vec<ContentHash>>,
    /// Total objects in this namespace
    pub object_count: u64,
    /// Total bytes used
    pub bytes_used: u64,
}

impl Namespace {
    pub fn new(name: String, owner: [u8; 32]) -> Self {
        Self {
            name,
            owner,
            index: BTreeMap::new(),
            versions: BTreeMap::new(),
            object_count: 0,
            bytes_used: 0,
        }
    }
}

// ============================================================================
// Content Store — deduplicated content-addressable storage
// ============================================================================

/// The content store. Stores content by hash, deduplicates automatically.
#[derive(Debug)]
pub struct ContentStore {
    /// Hash -> content bytes
    objects: BTreeMap<ContentHash, Vec<u8>>,
    /// Hash -> metadata
    metadata: BTreeMap<ContentHash, FsObjectMeta>,
    /// Reference counts (for garbage collection)
    refcounts: BTreeMap<ContentHash, u32>,
    /// Total unique objects
    pub unique_objects: u64,
    /// Total bytes stored (deduplicated)
    pub total_bytes: u64,
}

impl ContentStore {
    pub fn new() -> Self {
        Self {
            objects: BTreeMap::new(),
            metadata: BTreeMap::new(),
            refcounts: BTreeMap::new(),
            unique_objects: 0,
            total_bytes: 0,
        }
    }
    
    /// Store content. Returns the content hash.
    /// If content already exists (same hash), just increments refcount.
    pub fn put(&mut self, content: Vec<u8>, meta: FsObjectMeta) -> ContentHash {
        let hash = self.compute_hash(&content);
        
        if self.objects.contains_key(&hash) {
            // Dedup: content already exists, increment refcount
            *self.refcounts.entry(hash).or_insert(0) += 1;
        } else {
            // New content
            self.total_bytes += content.len() as u64;
            self.unique_objects += 1;
            self.objects.insert(hash, content);
            self.metadata.insert(hash, meta);
            self.refcounts.insert(hash, 1);
        }
        
        hash
    }
    
    /// Get content by hash
    pub fn get(&self, hash: &ContentHash) -> Option<&Vec<u8>> {
        self.objects.get(hash)
    }
    
    /// Get metadata by hash
    pub fn get_meta(&self, hash: &ContentHash) -> Option<&FsObjectMeta> {
        self.metadata.get(hash)
    }
    
    /// Compute SHA-256 hash of content
    /// NOTE: In production, this uses a proper SHA-256 implementation.
    /// For no_std, we'll use a minimal software SHA-256.
    fn compute_hash(&self, content: &[u8]) -> ContentHash {
        // Minimal FNV-1a based hash for scaffolding
        // TODO: Replace with proper SHA-256 (ring, sha2, or custom)
        let mut hash = [0u8; 32];
        let mut h: u64 = 0xcbf29ce484222325;
        for byte in content {
            h ^= *byte as u64;
            h = h.wrapping_mul(0x100000001b3);
        }
        // Spread the 64-bit hash across 32 bytes (temporary)
        for i in 0..4 {
            let offset = i * 8;
            let val = h.wrapping_add(i as u64).wrapping_mul(0x9e3779b97f4a7c15);
            hash[offset..offset+8].copy_from_slice(&val.to_le_bytes());
        }
        hash
    }
}

// ============================================================================
// AgentFS — the main filesystem service
// ============================================================================

/// The AgentFS server. Manages namespaces and the content store.
pub struct AgentFs {
    /// Content-addressable store (shared across all namespaces)
    pub store: ContentStore,
    /// Per-agent namespaces
    pub namespaces: BTreeMap<String, Namespace>,
    /// Shared namespace (readable by all agents with basic StoreCap)
    pub shared: Namespace,
    /// Total operations processed
    pub ops_count: u64,
}

impl AgentFs {
    pub fn new() -> Self {
        Self {
            store: ContentStore::new(),
            namespaces: BTreeMap::new(),
            shared: Namespace::new("shared".into(), [0u8; 32]),
            ops_count: 0,
        }
    }
    
    /// Create a namespace for an agent
    pub fn create_namespace(&mut self, agent_id: [u8; 32], name: String) -> bool {
        if self.namespaces.contains_key(&name) {
            return false;
        }
        self.namespaces.insert(name.clone(), Namespace::new(name, agent_id));
        true
    }
    
    /// Put an object into an agent's namespace
    pub fn put(&mut self, namespace: &str, key: String, content: Vec<u8>,
               content_type: String, tags: Vec<String>, 
               embedding: Option<Vec<f32>>, creator: [u8; 32]) -> Option<ContentHash> {
        let ns = self.namespaces.get_mut(namespace)?;
        
        // Determine version
        let version = ns.versions.get(&key)
            .map(|v| v.len() as u64 + 1)
            .unwrap_or(1);
        
        let parent_hash = ns.index.get(&key).copied();
        
        let meta = FsObjectMeta {
            key: key.clone(),
            content_type,
            creator,
            created_at: 0, // TODO: System timer
            tags,
            embedding,
            size: content.len() as u64,
            version,
            custom: Vec::new(),
        };
        
        // Store content (deduplicates automatically)
        let hash = self.store.put(content, meta);
        
        // Update namespace index
        ns.index.insert(key.clone(), hash);
        ns.versions.entry(key).or_insert_with(Vec::new).push(hash);
        ns.object_count += 1;
        
        self.ops_count += 1;
        Some(hash)
    }
    
    /// Get an object by key from a namespace
    pub fn get(&self, namespace: &str, key: &str) -> Option<(&Vec<u8>, &FsObjectMeta)> {
        let ns = self.namespaces.get(namespace)?;
        let hash = ns.index.get(key)?;
        let content = self.store.get(hash)?;
        let meta = self.store.get_meta(hash)?;
        Some((content, meta))
    }
    
    /// Get a specific version of an object
    pub fn get_version(&self, namespace: &str, key: &str, version: usize) 
        -> Option<(&Vec<u8>, &FsObjectMeta)> {
        let ns = self.namespaces.get(namespace)?;
        let versions = ns.versions.get(key)?;
        if version == 0 || version > versions.len() {
            return None;
        }
        let hash = &versions[version - 1];
        let content = self.store.get(hash)?;
        let meta = self.store.get_meta(hash)?;
        Some((content, meta))
    }
    
    /// List keys in a namespace (with optional prefix filter)
    pub fn list_keys(&self, namespace: &str, prefix: Option<&str>) -> Vec<String> {
        let ns = match self.namespaces.get(namespace) {
            Some(ns) => ns,
            None => return Vec::new(),
        };
        
        match prefix {
            Some(p) => ns.index.keys()
                .filter(|k| k.starts_with(p))
                .cloned()
                .collect(),
            None => ns.index.keys().cloned().collect(),
        }
    }
    
    /// Semantic search across a namespace
    /// Returns keys sorted by cosine similarity to the query embedding
    pub fn search_semantic(&self, namespace: &str, query_embedding: &[f32], top_k: usize) 
        -> Vec<(String, f32)> {
        let ns = match self.namespaces.get(namespace) {
            Some(ns) => ns,
            None => return Vec::new(),
        };
        
        let mut results: Vec<(String, f32)> = Vec::new();
        
        for (key, hash) in &ns.index {
            if let Some(meta) = self.store.get_meta(hash) {
                if let Some(ref embedding) = meta.embedding {
                    let similarity = cosine_similarity(query_embedding, embedding);
                    results.push((key.clone(), similarity));
                }
            }
        }
        
        // Sort by similarity (descending)
        results.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(core::cmp::Ordering::Equal));
        results.truncate(top_k);
        results
    }
    
    /// Checkpoint a namespace — snapshot current state
    pub fn checkpoint(&self, namespace: &str) -> Option<NamespaceCheckpoint> {
        let ns = self.namespaces.get(namespace)?;
        Some(NamespaceCheckpoint {
            namespace: namespace.into(),
            index_snapshot: ns.index.clone(),
            object_count: ns.object_count,
            bytes_used: ns.bytes_used,
            timestamp: 0, // TODO
        })
    }
    
    /// Restore a namespace from checkpoint
    pub fn restore(&mut self, checkpoint: &NamespaceCheckpoint) -> bool {
        if let Some(ns) = self.namespaces.get_mut(&checkpoint.namespace) {
            ns.index = checkpoint.index_snapshot.clone();
            ns.object_count = checkpoint.object_count;
            ns.bytes_used = checkpoint.bytes_used;
            true
        } else {
            false
        }
    }
}

/// Namespace checkpoint for save/restore
#[derive(Debug, Clone)]
pub struct NamespaceCheckpoint {
    pub namespace: String,
    pub index_snapshot: BTreeMap<String, ContentHash>,
    pub object_count: u64,
    pub bytes_used: u64,
    pub timestamp: u64,
}

// ============================================================================
// Utility
// ============================================================================

/// Cosine similarity between two embedding vectors
fn cosine_similarity(a: &[f32], b: &[f32]) -> f32 {
    if a.len() != b.len() || a.is_empty() {
        return 0.0;
    }
    
    let dot: f32 = a.iter().zip(b.iter()).map(|(x, y)| x * y).sum();
    let norm_a: f32 = a.iter().map(|x| x * x).sum::<f32>().sqrt();
    let norm_b: f32 = b.iter().map(|x| x * x).sum::<f32>().sqrt();
    
    if norm_a == 0.0 || norm_b == 0.0 {
        0.0
    } else {
        dot / (norm_a * norm_b)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    
    #[test]
    fn test_basic_put_get() {
        let mut fs = AgentFs::new();
        let agent = [0x42u8; 32];
        
        fs.create_namespace(agent, "test-agent".into());
        
        let hash = fs.put(
            "test-agent",
            "hello.txt".into(),
            b"Hello from agentOS!".to_vec(),
            "text/plain".into(),
            vec!["greeting".into()],
            None,
            agent,
        );
        
        assert!(hash.is_some());
        
        let (content, meta) = fs.get("test-agent", "hello.txt").unwrap();
        assert_eq!(content.as_slice(), b"Hello from agentOS!");
        assert_eq!(meta.key, "hello.txt");
        assert_eq!(meta.version, 1);
    }
    
    #[test]
    fn test_versioning() {
        let mut fs = AgentFs::new();
        let agent = [0x42u8; 32];
        fs.create_namespace(agent, "test-agent".into());
        
        fs.put("test-agent", "data.json".into(), b"v1".to_vec(),
               "application/json".into(), vec![], None, agent);
        fs.put("test-agent", "data.json".into(), b"v2".to_vec(),
               "application/json".into(), vec![], None, agent);
        
        // Latest version
        let (content, meta) = fs.get("test-agent", "data.json").unwrap();
        assert_eq!(content.as_slice(), b"v2");
        assert_eq!(meta.version, 2);
        
        // Version 1
        let (content, meta) = fs.get_version("test-agent", "data.json", 1).unwrap();
        assert_eq!(content.as_slice(), b"v1");
        assert_eq!(meta.version, 1);
    }
    
    #[test]
    fn test_dedup() {
        let mut fs = AgentFs::new();
        let agent = [0x42u8; 32];
        fs.create_namespace(agent, "test-agent".into());
        
        let content = b"Same content, different keys".to_vec();
        
        let h1 = fs.put("test-agent", "a.txt".into(), content.clone(),
                         "text/plain".into(), vec![], None, agent);
        let h2 = fs.put("test-agent", "b.txt".into(), content,
                         "text/plain".into(), vec![], None, agent);
        
        // Same hash = deduplicated
        assert_eq!(h1, h2);
        // Only 1 unique object in store
        assert_eq!(fs.store.unique_objects, 1);
    }
    
    #[test]
    fn test_semantic_search() {
        let mut fs = AgentFs::new();
        let agent = [0x42u8; 32];
        fs.create_namespace(agent, "test-agent".into());
        
        // Store objects with embeddings
        fs.put("test-agent", "cat.txt".into(), b"about cats".to_vec(),
               "text/plain".into(), vec!["animal".into()],
               Some(vec![1.0, 0.0, 0.0]), agent);
        
        fs.put("test-agent", "dog.txt".into(), b"about dogs".to_vec(),
               "text/plain".into(), vec!["animal".into()],
               Some(vec![0.9, 0.1, 0.0]), agent);
        
        fs.put("test-agent", "math.txt".into(), b"about math".to_vec(),
               "text/plain".into(), vec!["science".into()],
               Some(vec![0.0, 0.0, 1.0]), agent);
        
        // Search for "cat-like" things
        let results = fs.search_semantic("test-agent", &[1.0, 0.0, 0.0], 2);
        assert_eq!(results.len(), 2);
        assert_eq!(results[0].0, "cat.txt"); // Most similar
        assert_eq!(results[1].0, "dog.txt"); // Next most similar
    }
}
