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
    /// Optional payload (text, metadata, etc.)
    pub payload: VectorPayload,
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

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;

    // ── Embedding ─────────────────────────────────────────────────────────────

    #[test]
    fn embedding_new_sets_dims() {
        let e = Embedding::new(alloc::vec![1.0, 2.0, 3.0]);
        assert_eq!(e.dims, 3);
        assert_eq!(e.values.len(), 3);
    }

    #[test]
    fn cosine_similarity_identical_vectors_is_one() {
        let a = Embedding::new(alloc::vec![1.0, 0.0, 0.0]);
        let b = Embedding::new(alloc::vec![1.0, 0.0, 0.0]);
        let sim = a.cosine_similarity(&b);
        assert!((sim - 1.0).abs() < 1e-5, "expected ~1.0, got {}", sim);
    }

    #[test]
    fn cosine_similarity_opposite_vectors_is_negative_one() {
        let a = Embedding::new(alloc::vec![1.0, 0.0]);
        let b = Embedding::new(alloc::vec![-1.0, 0.0]);
        let sim = a.cosine_similarity(&b);
        assert!((sim - (-1.0)).abs() < 1e-5, "expected ~-1.0, got {}", sim);
    }

    #[test]
    fn cosine_similarity_orthogonal_vectors_is_zero() {
        let a = Embedding::new(alloc::vec![1.0, 0.0]);
        let b = Embedding::new(alloc::vec![0.0, 1.0]);
        let sim = a.cosine_similarity(&b);
        assert!(sim.abs() < 1e-5, "expected ~0.0, got {}", sim);
    }

    #[test]
    fn cosine_similarity_dimension_mismatch_returns_zero() {
        let a = Embedding::new(alloc::vec![1.0, 2.0]);
        let b = Embedding::new(alloc::vec![1.0, 2.0, 3.0]);
        assert_eq!(a.cosine_similarity(&b), 0.0);
    }

    #[test]
    fn cosine_similarity_zero_vector_returns_zero() {
        let a = Embedding::new(alloc::vec![0.0, 0.0]);
        let b = Embedding::new(alloc::vec![1.0, 0.0]);
        assert_eq!(a.cosine_similarity(&b), 0.0);
    }

    #[test]
    fn cosine_similarity_scaled_vectors_is_one() {
        // Scaling should not affect cosine similarity
        let a = Embedding::new(alloc::vec![2.0, 4.0]);
        let b = Embedding::new(alloc::vec![1.0, 2.0]);
        let sim = a.cosine_similarity(&b);
        assert!((sim - 1.0).abs() < 1e-5, "expected ~1.0, got {}", sim);
    }

    // ── VectorId ──────────────────────────────────────────────────────────────

    #[test]
    fn vector_id_display() {
        let id = VectorId::new(0xABCDEF);
        let s = id.to_string();
        assert!(s.starts_with("vec:"), "expected vec: prefix, got {}", s);
        assert!(s.contains("abcdef"), "expected hex digits in {}", s);
    }

    // ── VectorPartition ───────────────────────────────────────────────────────

    #[test]
    fn partition_starts_empty() {
        let p = VectorPartition::new("test", 3, IndexKind::Flat);
        assert!(p.is_empty());
        assert_eq!(p.len(), 0);
    }

    #[test]
    fn partition_insert_returns_sequential_ids() {
        let mut p = VectorPartition::new("test", 2, IndexKind::Flat);
        let id0 = p.insert(Embedding::new(alloc::vec![1.0, 0.0]), VectorPayload::Empty, 0).unwrap();
        let id1 = p.insert(Embedding::new(alloc::vec![0.0, 1.0]), VectorPayload::Empty, 1).unwrap();
        assert_eq!(id0, VectorId::new(0));
        assert_eq!(id1, VectorId::new(1));
        assert_eq!(p.len(), 2);
    }

    #[test]
    fn partition_insert_dimension_mismatch_fails() {
        let mut p = VectorPartition::new("test", 3, IndexKind::Flat);
        let e = Embedding::new(alloc::vec![1.0, 2.0]); // 2D into 3D partition
        let err = p.insert(e, VectorPayload::Empty, 0).unwrap_err();
        assert!(matches!(err, VectorError::DimensionMismatch { expected: 3, got: 2 }));
    }

    #[test]
    fn partition_search_returns_correct_top_k() {
        let mut p = VectorPartition::new("test", 2, IndexKind::Flat);
        p.insert(Embedding::new(alloc::vec![1.0, 0.0]), VectorPayload::Text("a".into()), 0).unwrap();
        p.insert(Embedding::new(alloc::vec![0.0, 1.0]), VectorPayload::Text("b".into()), 1).unwrap();
        p.insert(Embedding::new(alloc::vec![-1.0, 0.0]), VectorPayload::Text("c".into()), 2).unwrap();

        let query = SearchQuery::new(Embedding::new(alloc::vec![1.0, 0.0]), 2);
        let results = p.search(&query).unwrap();
        assert_eq!(results.len(), 2);
        // Most similar to [1,0] should be [1,0] itself
        assert!((results[0].score - 1.0).abs() < 1e-5);
    }

    #[test]
    fn partition_search_min_score_filters_results() {
        let mut p = VectorPartition::new("test", 2, IndexKind::Flat);
        p.insert(Embedding::new(alloc::vec![1.0, 0.0]), VectorPayload::Empty, 0).unwrap();
        p.insert(Embedding::new(alloc::vec![-1.0, 0.0]), VectorPayload::Empty, 1).unwrap(); // sim = -1.0

        let query = SearchQuery::new(Embedding::new(alloc::vec![1.0, 0.0]), 10)
            .with_min_score(0.5);
        let results = p.search(&query).unwrap();
        assert_eq!(results.len(), 1);
        assert!((results[0].score - 1.0).abs() < 1e-5);
    }

    #[test]
    fn partition_search_dimension_mismatch_fails() {
        let p = VectorPartition::new("test", 3, IndexKind::Flat);
        let query = SearchQuery::new(Embedding::new(alloc::vec![1.0, 0.0]), 5);
        let err = p.search(&query).unwrap_err();
        assert!(matches!(err, VectorError::DimensionMismatch { expected: 3, got: 2 }));
    }

    #[test]
    fn partition_search_results_sorted_by_score_descending() {
        let mut p = VectorPartition::new("test", 2, IndexKind::Flat);
        p.insert(Embedding::new(alloc::vec![0.0, 1.0]), VectorPayload::Empty, 0).unwrap();  // sim ~0
        p.insert(Embedding::new(alloc::vec![1.0, 0.0]), VectorPayload::Empty, 1).unwrap();  // sim  1
        p.insert(Embedding::new(alloc::vec![-1.0, 0.0]), VectorPayload::Empty, 2).unwrap(); // sim -1

        let query = SearchQuery::new(Embedding::new(alloc::vec![1.0, 0.0]), 3);
        let results = p.search(&query).unwrap();
        // Scores should be descending
        for w in results.windows(2) {
            assert!(w[0].score >= w[1].score);
        }
    }

    #[test]
    fn search_result_distance_is_one_minus_score() {
        let mut p = VectorPartition::new("test", 2, IndexKind::Flat);
        p.insert(Embedding::new(alloc::vec![1.0, 0.0]), VectorPayload::Empty, 0).unwrap();

        let query = SearchQuery::new(Embedding::new(alloc::vec![1.0, 0.0]), 1);
        let results = p.search(&query).unwrap();
        let r = &results[0];
        assert!((r.distance - (1.0 - r.score)).abs() < 1e-5);
    }

    // ── VectorError Display ───────────────────────────────────────────────────

    #[test]
    fn vector_error_display() {
        assert!(VectorError::DimensionMismatch { expected: 3, got: 2 }
            .to_string().contains("3"));
        assert!(VectorError::NotFound.to_string().contains("not found"));
        assert!(VectorError::PartitionFull.to_string().contains("full"));
        assert!(VectorError::CapabilityDenied.to_string().contains("denied"));
        assert!(VectorError::IndexError("oops".into()).to_string().contains("oops"));
    }
}
