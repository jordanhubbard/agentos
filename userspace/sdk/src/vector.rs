//! VectorStore - native embedding store for agentOS
//!
//! Semantic memory is a first-class primitive in agentOS.
//! Agents store, query, and share vector embeddings through capability-gated partitions.
//!
//! This is not "add a vector DB" — it's designed into the OS from day one.
//! The VectorStore is a system server (like the filesystem), and every agent
//! gets a private partition by default, with the ability to share partitions
//! via capability grants.

use alloc::string::String;
use alloc::vec::Vec;
use crate::fs::ObjectId;

/// A vector embedding
#[derive(Debug, Clone)]
pub struct Embedding {
    /// The raw float values
    pub values: Vec<f32>,
    /// The dimensionality
    pub dims: usize,
}

impl Embedding {
    pub fn new(values: Vec<f32>) -> Self {
        let dims = values.len();
        Self { values, dims }
    }
    
    /// Cosine similarity with another embedding
    pub fn cosine_similarity(&self, other: &Embedding) -> f32 {
        if self.dims != other.dims {
            return 0.0;
        }
        
        let dot: f32 = self.values.iter().zip(other.values.iter())
            .map(|(a, b)| a * b)
            .sum();
        
        let norm_a: f32 = f32_sqrt(self.values.iter().map(|x| x * x).sum::<f32>());
        let norm_b: f32 = f32_sqrt(other.values.iter().map(|x| x * x).sum::<f32>());
        
        if norm_a == 0.0 || norm_b == 0.0 {
            0.0
        } else {
            dot / (norm_a * norm_b)
        }
    }
}

/// Newton-Raphson sqrt for no_std f32 (used by cosine_similarity).
#[cfg(not(feature = "std"))]
fn f32_sqrt(x: f32) -> f32 {
    if x <= 0.0 { return 0.0; }
    let mut r = x;
    for _ in 0..24 { r = 0.5 * (r + x / r); }
    r
}

#[cfg(feature = "std")]
fn f32_sqrt(x: f32) -> f32 { x.sqrt() }

/// A stored vector record
#[derive(Debug, Clone)]
pub struct VectorRecord {
    /// Unique ID within the partition
    pub id: VectorId,
    /// The embedding
    pub embedding: Embedding,
    /// Optional payload (text, object reference, etc.)
    pub payload: VectorPayload,
    /// Link to an AgentFS object (if this embedding represents an object)
    pub object_ref: Option<ObjectId>,
    /// Insertion timestamp
    pub created_at_ns: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct VectorId(pub u64);

impl VectorId {
    pub fn new(id: u64) -> Self { Self(id) }
}

impl core::fmt::Display for VectorId {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "vec:{:016x}", self.0)
    }
}

/// The payload stored alongside a vector
#[derive(Debug, Clone)]
pub enum VectorPayload {
    Empty,
    Text(String),
    Bytes(Vec<u8>),
}

/// A search result from the VectorStore
#[derive(Debug, Clone)]
pub struct SearchResult {
    pub record: VectorRecord,
    pub score: f32,
    pub distance: f32,
}

/// Query parameters for vector search
#[derive(Debug, Clone)]
pub struct SearchQuery {
    pub query: Embedding,
    pub top_k: usize,
    pub min_score: f32,
    pub filter: Option<VectorFilter>,
}

impl SearchQuery {
    pub fn new(query: Embedding, top_k: usize) -> Self {
        Self {
            query,
            top_k,
            min_score: 0.0,
            filter: None,
        }
    }
    
    pub fn with_min_score(mut self, min_score: f32) -> Self {
        self.min_score = min_score;
        self
    }
    
    pub fn with_filter(mut self, filter: VectorFilter) -> Self {
        self.filter = Some(filter);
        self
    }
}

/// Optional filter for vector search
#[derive(Debug, Clone)]
pub struct VectorFilter {
    /// Only return vectors created after this timestamp
    pub after_ns: Option<u64>,
    /// Only return vectors with object refs
    pub has_object_ref: Option<bool>,
}

/// A VectorStore partition
///
/// Agents get a private partition by default.
/// They can create additional partitions and share them via capability grants.
pub struct VectorPartition {
    pub id: String,
    pub dims: usize,
    pub index_kind: IndexKind,
    records: Vec<VectorRecord>,
    next_id: u64,
}

impl VectorPartition {
    pub fn new(id: impl Into<String>, dims: usize, index_kind: IndexKind) -> Self {
        Self {
            id: id.into(),
            dims,
            index_kind,
            records: Vec::new(),
            next_id: 0,
        }
    }
    
    /// Add a vector to this partition
    pub fn insert(&mut self, embedding: Embedding, payload: VectorPayload, now_ns: u64)
        -> Result<VectorId, VectorError>
    {
        if embedding.dims != self.dims {
            return Err(VectorError::DimensionMismatch { 
                expected: self.dims, 
                got: embedding.dims 
            });
        }
        
        let id = VectorId::new(self.next_id);
        self.next_id += 1;
        
        self.records.push(VectorRecord {
            id: id.clone(),
            embedding,
            payload,
            object_ref: None,
            created_at_ns: now_ns,
        });
        
        Ok(id)
    }
    
    /// Search for similar vectors (brute-force cosine similarity for now)
    /// Production backend will use HNSW or IVF indexing
    pub fn search(&self, query: &SearchQuery) -> Result<Vec<SearchResult>, VectorError> {
        if query.query.dims != self.dims {
            return Err(VectorError::DimensionMismatch {
                expected: self.dims,
                got: query.query.dims,
            });
        }
        
        let mut scored: Vec<(f32, &VectorRecord)> = self.records.iter()
            .map(|r| (r.embedding.cosine_similarity(&query.query), r))
            .filter(|(score, _)| *score >= query.min_score)
            .collect();
        
        // Sort by score descending
        scored.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap_or(core::cmp::Ordering::Equal));
        scored.truncate(query.top_k);
        
        Ok(scored.into_iter().map(|(score, record)| SearchResult {
            record: record.clone(),
            score,
            distance: 1.0 - score,
        }).collect())
    }
    
    pub fn len(&self) -> usize {
        self.records.len()
    }
    
    pub fn is_empty(&self) -> bool {
        self.records.is_empty()
    }
}

/// Index types for vector search
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum IndexKind {
    /// Brute force (accurate, slow for large corpora)
    Flat,
    /// Hierarchical Navigable Small World (fast approximate)
    Hnsw { m: u16, ef_construction: u16 },
    /// Inverted File Index (good for large, lower accuracy)
    Ivf { nlist: u32 },
}

/// VectorStore errors
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VectorError {
    DimensionMismatch { expected: usize, got: usize },
    NotFound,
    PartitionFull,
    CapabilityDenied,
    IndexError(String),
}

impl core::fmt::Display for VectorError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            VectorError::DimensionMismatch { expected, got } =>
                write!(f, "dimension mismatch: expected {expected}, got {got}"),
            VectorError::NotFound => write!(f, "vector not found"),
            VectorError::PartitionFull => write!(f, "vector partition is full"),
            VectorError::CapabilityDenied => write!(f, "capability denied for vector partition"),
            VectorError::IndexError(e) => write!(f, "index error: {e}"),
        }
    }
}
