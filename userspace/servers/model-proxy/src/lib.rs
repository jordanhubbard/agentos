//! Model Proxy — capability-gated LLM inference service for agentOS
//!
//! The thinking engine of agentOS. Every agent that needs to reason,
//! generate, or understand does so through ModelProxy.
//!
//! ## Why OS-level inference?
//!
//! In a traditional OS, if you want to call an LLM, you import a library,
//! configure an API key, and make HTTP requests. Every application manages
//! its own inference stack.
//!
//! In agentOS, inference is a system service:
//!
//! - **Capability-gated**: An agent needs a `ModelCap` to query any model.
//!   The cap specifies which models, token budgets, rate limits.
//! - **Centralized routing**: One service manages all model endpoints,
//!   load balancing, failover, and token accounting.
//! - **Budget enforcement**: Each agent has a token budget per capability
//!   period. The OS enforces it — no single agent can drain inference capacity.
//! - **Model abstraction**: Agents request inference by task, not by model name.
//!   ModelProxy routes to the best available model for the request.
//! - **Caching**: Identical prompts from different agents get cached responses.
//!   The content-addressable design of AgentFS makes this natural.
//! - **Audit trail**: Every inference request is logged with agent identity,
//!   tokens used, latency, and model selected. Full accountability.
//!
//! ## Architecture
//!
//! ModelProxy is a Microkit PD that:
//! 1. Receives inference requests via IPC (PPC from agent PDs)
//! 2. Validates the caller's ModelCap (token budget, model access)
//! 3. Routes to the best available backend
//! 4. Returns the response via IPC
//!
//! Backends (pluggable):
//! - HTTP API (OpenAI-compatible, NVIDIA NIM, Anthropic, etc.)
//! - Local inference (via GPU PD — Natasha/Boris-class hardware)
//! - Peer agent (another agentOS node with model capacity)
//! - Cache (exact prompt match from AgentFS)

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

// ============================================================================
// Types
// ============================================================================

/// Model identifier
pub type ModelId = String;

/// Token count
pub type TokenCount = u64;

/// A registered model endpoint
#[derive(Debug, Clone)]
pub struct ModelEndpoint {
    /// Unique model identifier (e.g., "claude-sonnet-4", "gpt-4o")
    pub model_id: ModelId,
    /// Backend type
    pub backend: BackendType,
    /// Maximum context window (tokens)
    pub context_window: TokenCount,
    /// Maximum output tokens per request
    pub max_output_tokens: TokenCount,
    /// Whether this model supports structured output
    pub structured_output: bool,
    /// Whether this model supports tool use
    pub tool_use: bool,
    /// Whether this model supports vision
    pub vision: bool,
    /// Cost tier (0 = free/local, 1 = cheap, 2 = standard, 3 = premium)
    pub cost_tier: u8,
    /// Current availability
    pub available: bool,
    /// Statistics
    pub stats: ModelStats,
}

/// Backend types for model routing
#[derive(Debug, Clone, PartialEq)]
pub enum BackendType {
    /// HTTP API (OpenAI-compatible endpoint)
    HttpApi {
        endpoint_url: String,
        api_key_env: String,
        model_name: String,
    },
    /// Local inference via GPU PD
    LocalGpu {
        gpu_pd_channel: u32,
        quantization: Option<String>,
    },
    /// Peer agentOS node
    PeerNode {
        node_id: [u8; 32],
        endpoint_url: String,
    },
    /// Cache-only (serves from AgentFS cache, no actual inference)
    CacheOnly,
}

/// Model statistics
#[derive(Debug, Clone, Default)]
pub struct ModelStats {
    pub total_requests: u64,
    pub total_tokens_in: TokenCount,
    pub total_tokens_out: TokenCount,
    pub total_latency_us: u64,
    pub cache_hits: u64,
    pub errors: u64,
}

/// Inference request from an agent
#[derive(Debug, Clone)]
pub struct InferenceRequest {
    /// Requesting agent's identity
    pub agent_id: [u8; 32],
    /// Agent's ModelCap badge
    pub cap_badge: u64,
    /// Preferred model (None = let ModelProxy choose)
    pub model_preference: Option<ModelId>,
    /// System prompt
    pub system: Option<String>,
    /// User/agent prompt
    pub prompt: String,
    /// Messages history (for multi-turn)
    pub messages: Vec<ChatMessage>,
    /// Parameters
    pub params: InferenceParams,
    /// Request metadata
    pub metadata: RequestMetadata,
}

/// Chat message (for multi-turn conversations)
#[derive(Debug, Clone)]
pub struct ChatMessage {
    pub role: MessageRole,
    pub content: String,
}

#[derive(Debug, Clone, PartialEq)]
pub enum MessageRole {
    System,
    User,
    Assistant,
    Tool,
}

/// Inference parameters
#[derive(Debug, Clone)]
pub struct InferenceParams {
    /// Sampling temperature (0.0 = deterministic, 2.0 = creative)
    pub temperature: f32,
    /// Maximum output tokens
    pub max_tokens: TokenCount,
    /// Top-p sampling
    pub top_p: Option<f32>,
    /// Stop sequences
    pub stop: Vec<String>,
    /// Request structured JSON output
    pub json_mode: bool,
    /// Timeout in microseconds
    pub timeout_us: u64,
}

impl Default for InferenceParams {
    fn default() -> Self {
        Self {
            temperature: 0.7,
            max_tokens: 4096,
            top_p: None,
            stop: Vec::new(),
            json_mode: false,
            timeout_us: 30_000_000, // 30 seconds
        }
    }
}

/// Request metadata (for routing decisions and audit)
#[derive(Debug, Clone)]
pub struct RequestMetadata {
    /// Task type hint (for intelligent routing)
    pub task_type: TaskType,
    /// Priority (0 = background, 1 = normal, 2 = urgent)
    pub priority: u8,
    /// Whether caching is acceptable
    pub allow_cache: bool,
    /// Request ID (for correlation)
    pub request_id: u64,
}

/// Task type hints for intelligent model routing
#[derive(Debug, Clone, PartialEq)]
pub enum TaskType {
    /// General text generation
    General,
    /// Code generation/analysis
    Code,
    /// Structured data extraction
    Extraction,
    /// Classification/categorization
    Classification,
    /// Mathematical/logical reasoning
    Reasoning,
    /// Summarization
    Summary,
    /// Creative writing
    Creative,
    /// Tool use / function calling
    ToolUse,
    /// Agent-to-agent coordination
    Coordination,
    /// Embedding generation
    Embedding,
}

/// Inference response
#[derive(Debug, Clone)]
pub struct InferenceResponse {
    pub status: InferenceStatus,
    /// Generated content
    pub content: String,
    /// Which model actually served this
    pub model_used: ModelId,
    /// Whether this was served from cache
    pub from_cache: bool,
    /// Token usage
    pub tokens_in: TokenCount,
    pub tokens_out: TokenCount,
    /// Latency in microseconds
    pub latency_us: u64,
    /// Correlation ID
    pub request_id: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum InferenceStatus {
    Ok,
    /// Agent doesn't have ModelCap for requested model
    AccessDenied,
    /// Agent's token budget exhausted for this period
    BudgetExhausted,
    /// All backends failed
    AllBackendsFailed,
    /// Request timed out
    Timeout,
    /// Input too large for any available model
    ContextOverflow,
    /// Service error
    Error(String),
}

// ============================================================================
// Token Budget
// ============================================================================

/// Per-agent token budget (enforced by ModelProxy)
#[derive(Debug, Clone)]
pub struct TokenBudget {
    /// Agent identity
    pub agent_id: [u8; 32],
    /// ModelCap badge
    pub cap_badge: u64,
    /// Allowed model IDs (empty = all models the agent has caps for)
    pub allowed_models: Vec<ModelId>,
    /// Token budget per period
    pub tokens_per_period: TokenCount,
    /// Budget period in microseconds
    pub period_us: u64,
    /// Tokens used in current period
    pub tokens_used: TokenCount,
    /// Period start timestamp
    pub period_start: u64,
    /// Maximum tokens per single request
    pub max_per_request: TokenCount,
}

impl TokenBudget {
    /// Check if a request is within budget
    pub fn check(&self, estimated_tokens: TokenCount, model_id: Option<&str>) -> bool {
        // Check model access
        if let Some(model) = model_id {
            if !self.allowed_models.is_empty() 
               && !self.allowed_models.iter().any(|m| m == model) {
                return false;
            }
        }
        
        // Check per-request limit
        if estimated_tokens > self.max_per_request {
            return false;
        }
        
        // Check period budget
        if self.tokens_used + estimated_tokens > self.tokens_per_period {
            return false;
        }
        
        true
    }
    
    /// Consume tokens from the budget
    pub fn consume(&mut self, tokens: TokenCount) {
        self.tokens_used += tokens;
    }
    
    /// Reset the budget for a new period
    pub fn reset_period(&mut self, now: u64) {
        self.tokens_used = 0;
        self.period_start = now;
    }
}

// ============================================================================
// Model Router
// ============================================================================

/// Selects the best model for a given request
pub struct ModelRouter {
    /// Registered model endpoints
    endpoints: BTreeMap<ModelId, ModelEndpoint>,
    /// Routing preferences by task type
    task_preferences: BTreeMap<u8, Vec<ModelId>>, // task_type ordinal -> preferred models
}

impl ModelRouter {
    pub fn new() -> Self {
        Self {
            endpoints: BTreeMap::new(),
            task_preferences: BTreeMap::new(),
        }
    }
    
    /// Register a model endpoint
    pub fn register_endpoint(&mut self, endpoint: ModelEndpoint) {
        self.endpoints.insert(endpoint.model_id.clone(), endpoint);
    }
    
    /// Select the best model for a request
    pub fn route(&self, request: &InferenceRequest, budget: &TokenBudget) -> Option<ModelId> {
        // If agent specified a preference, try it first
        if let Some(ref pref) = request.model_preference {
            if let Some(ep) = self.endpoints.get(pref) {
                if ep.available && budget.check(request.params.max_tokens, Some(pref)) {
                    return Some(pref.clone());
                }
            }
        }
        
        // Otherwise, find the best available model
        let mut candidates: Vec<&ModelEndpoint> = self.endpoints.values()
            .filter(|ep| {
                ep.available && 
                budget.check(request.params.max_tokens, Some(&ep.model_id)) &&
                ep.max_output_tokens >= request.params.max_tokens
            })
            .collect();
        
        if candidates.is_empty() {
            return None;
        }
        
        // Sort by suitability:
        // 1. Task-type match (if configured)
        // 2. Tool use capability (if request needs it)
        // 3. Lower cost tier preferred (budget-conscious)
        // 4. Lower average latency preferred
        candidates.sort_by(|a, b| {
            // Prefer models that support needed capabilities
            let a_score = self.score_model(a, request);
            let b_score = self.score_model(b, request);
            b_score.partial_cmp(&a_score).unwrap_or(core::cmp::Ordering::Equal)
        });
        
        candidates.first().map(|ep| ep.model_id.clone())
    }
    
    /// Score a model for a request (higher = better fit)
    fn score_model(&self, model: &ModelEndpoint, request: &InferenceRequest) -> f32 {
        let mut score: f32 = 10.0;
        
        // Prefer lower cost tier
        score -= model.cost_tier as f32 * 2.0;
        
        // Bonus for tool use capability when needed
        if request.metadata.task_type == TaskType::ToolUse && model.tool_use {
            score += 5.0;
        }
        
        // Bonus for code models when doing code tasks
        if request.metadata.task_type == TaskType::Code {
            if model.model_id.contains("code") || model.model_id.contains("codex") {
                score += 3.0;
            }
        }
        
        // Prefer models with lower average latency
        if model.stats.total_requests > 0 {
            let avg_latency = model.stats.total_latency_us / model.stats.total_requests;
            if avg_latency < 1_000_000 { // < 1 second
                score += 2.0;
            }
        }
        
        // High priority requests get routed to premium models
        if request.metadata.priority >= 2 {
            score += model.cost_tier as f32; // Flip: premium models score higher
        }
        
        score
    }
}

// ============================================================================
// Prompt Cache
// ============================================================================

/// Simple prompt cache using content-addressable hashing
pub struct PromptCache {
    /// Hash of (system + prompt + params) -> cached response
    cache: BTreeMap<[u8; 32], CachedResponse>,
    /// Cache statistics
    pub hits: u64,
    pub misses: u64,
    /// Maximum cache entries
    max_entries: usize,
}

#[derive(Debug, Clone)]
struct CachedResponse {
    content: String,
    model_used: ModelId,
    tokens_out: TokenCount,
    cached_at: u64,
}

impl PromptCache {
    pub fn new(max_entries: usize) -> Self {
        Self {
            cache: BTreeMap::new(),
            hits: 0,
            misses: 0,
            max_entries,
        }
    }
    
    /// Look up a cached response
    pub fn get(&mut self, request: &InferenceRequest) -> Option<&CachedResponse> {
        if !request.metadata.allow_cache {
            self.misses += 1;
            return None;
        }
        
        let key = self.cache_key(request);
        if let Some(resp) = self.cache.get(&key) {
            self.hits += 1;
            Some(resp)
        } else {
            self.misses += 1;
            None
        }
    }
    
    /// Store a response in cache
    pub fn put(&mut self, request: &InferenceRequest, response: &InferenceResponse) {
        if self.cache.len() >= self.max_entries {
            // Evict oldest (simple strategy — LRU would be better)
            if let Some(first_key) = self.cache.keys().next().cloned() {
                self.cache.remove(&first_key);
            }
        }
        
        let key = self.cache_key(request);
        self.cache.insert(key, CachedResponse {
            content: response.content.clone(),
            model_used: response.model_used.clone(),
            tokens_out: response.tokens_out,
            cached_at: 0, // TODO: system time
        });
    }
    
    /// Generate cache key from request
    fn cache_key(&self, request: &InferenceRequest) -> [u8; 32] {
        // Simple FNV hash of system + prompt + temperature
        // TODO: Replace with proper SHA-256
        let mut h: u64 = 0xcbf29ce484222325;
        
        if let Some(ref sys) = request.system {
            for b in sys.bytes() {
                h ^= b as u64;
                h = h.wrapping_mul(0x100000001b3);
            }
        }
        for b in request.prompt.bytes() {
            h ^= b as u64;
            h = h.wrapping_mul(0x100000001b3);
        }
        // Include temperature (rounded to 2 decimals)
        let temp_bits = (request.params.temperature * 100.0) as u64;
        h ^= temp_bits;
        h = h.wrapping_mul(0x100000001b3);
        
        let mut key = [0u8; 32];
        for i in 0..4 {
            let val = h.wrapping_add(i).wrapping_mul(0x9e3779b97f4a7c15);
            key[i as usize * 8..(i as usize + 1) * 8].copy_from_slice(&val.to_le_bytes());
        }
        key
    }
}

// ============================================================================
// Model Proxy Service
// ============================================================================

/// The main model proxy service
pub struct ModelProxy {
    /// Model router
    pub router: ModelRouter,
    /// Per-agent token budgets
    pub budgets: BTreeMap<u64, TokenBudget>, // badge -> budget
    /// Prompt cache
    pub cache: PromptCache,
    /// Global statistics
    pub total_requests: u64,
    pub total_tokens: TokenCount,
}

impl ModelProxy {
    pub fn new() -> Self {
        Self {
            router: ModelRouter::new(),
            budgets: BTreeMap::new(),
            cache: PromptCache::new(1024),
            total_requests: 0,
            total_tokens: 0,
        }
    }
    
    /// Register a model endpoint
    pub fn register_model(&mut self, endpoint: ModelEndpoint) {
        self.router.register_endpoint(endpoint);
    }
    
    /// Set a token budget for an agent
    pub fn set_budget(&mut self, budget: TokenBudget) {
        self.budgets.insert(budget.cap_badge, budget);
    }
    
    /// Process an inference request
    pub fn infer(&mut self, request: &InferenceRequest) -> InferenceResponse {
        self.total_requests += 1;
        
        // 1. Check budget
        let budget = match self.budgets.get(&request.cap_badge) {
            Some(b) => b,
            None => return InferenceResponse {
                status: InferenceStatus::AccessDenied,
                content: String::new(),
                model_used: String::new(),
                from_cache: false,
                tokens_in: 0,
                tokens_out: 0,
                latency_us: 0,
                request_id: request.metadata.request_id,
            },
        };
        
        // 2. Check cache
        if let Some(cached) = self.cache.get(request) {
            return InferenceResponse {
                status: InferenceStatus::Ok,
                content: cached.content.clone(),
                model_used: cached.model_used.clone(),
                from_cache: true,
                tokens_in: 0,
                tokens_out: cached.tokens_out,
                latency_us: 0,
                request_id: request.metadata.request_id,
            };
        }
        
        // 3. Route to best model
        let model_id = match self.router.route(request, budget) {
            Some(id) => id,
            None => return InferenceResponse {
                status: InferenceStatus::BudgetExhausted,
                content: String::new(),
                model_used: String::new(),
                from_cache: false,
                tokens_in: 0,
                tokens_out: 0,
                latency_us: 0,
                request_id: request.metadata.request_id,
            },
        };
        
        // 4. Execute inference (backend-specific)
        // In the real implementation, this dispatches to the backend:
        // - HttpApi: Make HTTP request via NetStack PD
        // - LocalGpu: PPC to GPU PD with model weights reference
        // - PeerNode: IPC to network PD for remote inference
        // - CacheOnly: Already handled above
        
        // Placeholder response for scaffolding
        let response = InferenceResponse {
            status: InferenceStatus::Ok,
            content: String::from("[ModelProxy: backend dispatch pending — IPC to NetStack/GPU PD not yet wired]"),
            model_used: model_id,
            from_cache: false,
            tokens_in: estimate_tokens(&request.prompt),
            tokens_out: 20,
            latency_us: 0,
            request_id: request.metadata.request_id,
        };
        
        // 5. Update budget
        if let Some(budget) = self.budgets.get_mut(&request.cap_badge) {
            budget.consume(response.tokens_in + response.tokens_out);
        }
        self.total_tokens += response.tokens_in + response.tokens_out;
        
        // 6. Cache the response
        self.cache.put(request, &response);
        
        response
    }
}

/// Rough token count estimate (1 token ≈ 4 chars for English)
fn estimate_tokens(text: &str) -> TokenCount {
    (text.len() as u64 + 3) / 4
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    
    fn make_endpoint(id: &str, cost: u8) -> ModelEndpoint {
        ModelEndpoint {
            model_id: id.into(),
            backend: BackendType::CacheOnly,
            context_window: 128_000,
            max_output_tokens: 4096,
            structured_output: true,
            tool_use: true,
            vision: false,
            cost_tier: cost,
            available: true,
            stats: ModelStats::default(),
        }
    }
    
    fn make_budget(badge: u64) -> TokenBudget {
        TokenBudget {
            agent_id: [0x42u8; 32],
            cap_badge: badge,
            allowed_models: Vec::new(), // All models
            tokens_per_period: 100_000,
            period_us: 60_000_000, // 60 seconds
            tokens_used: 0,
            period_start: 0,
            max_per_request: 8192,
        }
    }
    
    fn make_request(prompt: &str, badge: u64) -> InferenceRequest {
        InferenceRequest {
            agent_id: [0x42u8; 32],
            cap_badge: badge,
            model_preference: None,
            system: None,
            prompt: prompt.into(),
            messages: Vec::new(),
            params: InferenceParams::default(),
            metadata: RequestMetadata {
                task_type: TaskType::General,
                priority: 1,
                allow_cache: true,
                request_id: 1,
            },
        }
    }
    
    #[test]
    fn test_basic_inference() {
        let mut proxy = ModelProxy::new();
        proxy.register_model(make_endpoint("test-model", 1));
        proxy.set_budget(make_budget(42));
        
        let req = make_request("Hello, agentOS!", 42);
        let resp = proxy.infer(&req);
        
        assert_eq!(resp.status, InferenceStatus::Ok);
        assert!(!resp.content.is_empty());
        assert_eq!(resp.model_used, "test-model");
    }
    
    #[test]
    fn test_access_denied_no_budget() {
        let mut proxy = ModelProxy::new();
        proxy.register_model(make_endpoint("test-model", 1));
        // No budget set for badge 99
        
        let req = make_request("Hello", 99);
        let resp = proxy.infer(&req);
        
        assert_eq!(resp.status, InferenceStatus::AccessDenied);
    }
    
    #[test]
    fn test_budget_enforcement() {
        let mut proxy = ModelProxy::new();
        proxy.register_model(make_endpoint("test-model", 1));
        
        let mut budget = make_budget(42);
        budget.tokens_per_period = 100; // Very small budget
        proxy.set_budget(budget);
        
        // First request should work
        let req = make_request("Short prompt", 42);
        let resp = proxy.infer(&req);
        assert_eq!(resp.status, InferenceStatus::Ok);
        
        // After consuming tokens, budget should be reduced
        let budget = proxy.budgets.get(&42).unwrap();
        assert!(budget.tokens_used > 0);
    }
    
    #[test]
    fn test_cache_hit() {
        let mut proxy = ModelProxy::new();
        proxy.register_model(make_endpoint("test-model", 1));
        proxy.set_budget(make_budget(42));
        
        let req = make_request("Same prompt twice", 42);
        
        // First call: cache miss
        let resp1 = proxy.infer(&req);
        assert!(!resp1.from_cache);
        
        // Second call: cache hit
        let resp2 = proxy.infer(&req);
        assert!(resp2.from_cache);
        
        assert_eq!(proxy.cache.hits, 1);
    }
    
    #[test]
    fn test_model_routing_prefers_cheaper() {
        let mut proxy = ModelProxy::new();
        proxy.register_model(make_endpoint("cheap-model", 1));
        proxy.register_model(make_endpoint("expensive-model", 3));
        proxy.set_budget(make_budget(42));
        
        let req = make_request("Route me", 42);
        let resp = proxy.infer(&req);
        
        // Should prefer cheaper model for general tasks
        assert_eq!(resp.model_used, "cheap-model");
    }
}
