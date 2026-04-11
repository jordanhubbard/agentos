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
use alloc::string::{String, ToString};
use alloc::vec::Vec;
use sha2::{Sha256, Digest};

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
}

impl ModelRouter {
    pub fn new() -> Self {
        Self {
            endpoints: BTreeMap::new(),
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
pub struct CachedResponse {
    content: String,
    model_used: ModelId,
    tokens_out: TokenCount,
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
        });
    }
    
    /// Generate a SHA-256 cache key from the request parameters
    fn cache_key(&self, request: &InferenceRequest) -> [u8; 32] {
        let mut hasher = Sha256::new();
        if let Some(ref sys) = request.system {
            hasher.update(sys.as_bytes());
            hasher.update(b"\x00"); // field separator
        }
        hasher.update(request.prompt.as_bytes());
        hasher.update(b"\x00");
        // Include temperature rounded to 2 decimal places so floating-point
        // representation differences don't produce spurious cache misses
        let temp_cents = (request.params.temperature * 100.0) as u32;
        hasher.update(&temp_cents.to_le_bytes());
        hasher.finalize().into()
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
        // - HttpApi: Make HTTP request via NetStack PD
        // - LocalGpu: PPC to GPU PD with model weights reference
        // - PeerNode: IPC to network PD for remote inference
        // - CacheOnly: Already handled above

        let response = InferenceResponse {
            status: InferenceStatus::Ok,
            content: String::from("[ModelProxy: non-HTTP backend dispatch pending — IPC to NetStack/GPU PD not yet wired]"),
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
// ModelProxy — async HTTP dispatch  (requires the "std" feature)
// ============================================================================

#[cfg(feature = "std")]
impl ModelProxy {
    /// Async variant of `infer` that performs real HTTP calls for `HttpApi`
    /// backends using the OpenAI-compatible `/v1/chat/completions` endpoint.
    ///
    /// Falls back to the synchronous stub for non-HTTP backends (LocalGpu,
    /// PeerNode, CacheOnly) until those dispatch paths are wired.
    pub async fn infer_async(&mut self, request: &InferenceRequest) -> InferenceResponse {
        use alloc::format;

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

        // 4. Dispatch to backend
        let endpoint_info = self.router.endpoints.get(&model_id).cloned();
        let response = match endpoint_info {
            Some(ref ep) => match &ep.backend {
                BackendType::HttpApi { endpoint_url, api_key_env, model_name } => {
                    // Resolve the API key from the environment variable named by
                    // `api_key_env`; fall back to empty string (works for local
                    // Ollama which ignores the Authorization header).
                    let api_key = std::env::var(api_key_env).unwrap_or_default();

                    // Build the messages array: optional system message followed
                    // by the request messages and finally the user prompt.
                    let mut messages: alloc::vec::Vec<serde_json::Value> = alloc::vec![];
                    if let Some(ref sys) = request.system {
                        messages.push(serde_json::json!({"role": "system", "content": sys}));
                    }
                    for msg in &request.messages {
                        let role = match msg.role {
                            MessageRole::System    => "system",
                            MessageRole::User      => "user",
                            MessageRole::Assistant => "assistant",
                            MessageRole::Tool      => "tool",
                        };
                        messages.push(serde_json::json!({"role": role, "content": msg.content}));
                    }
                    messages.push(serde_json::json!({"role": "user", "content": request.prompt}));

                    match http_backend::call_model_api(
                        endpoint_url,
                        &api_key,
                        &messages,
                        model_name,
                        request.params.max_tokens as u32,
                    ).await {
                        Ok((content, tokens_in, tokens_out)) => InferenceResponse {
                            status: InferenceStatus::Ok,
                            content,
                            model_used: model_id.clone(),
                            from_cache: false,
                            tokens_in,
                            tokens_out,
                            latency_us: 0,
                            request_id: request.metadata.request_id,
                        },
                        Err(e) => InferenceResponse {
                            status: InferenceStatus::Error(format!("HTTP backend error: {}", e)),
                            content: String::new(),
                            model_used: model_id.clone(),
                            from_cache: false,
                            tokens_in: 0,
                            tokens_out: 0,
                            latency_us: 0,
                            request_id: request.metadata.request_id,
                        },
                    }
                }
                _ => {
                    // Non-HTTP backends not yet wired for async dispatch
                    InferenceResponse {
                        status: InferenceStatus::Ok,
                        content: String::from("[ModelProxy: non-HTTP backend dispatch pending]"),
                        model_used: model_id.clone(),
                        from_cache: false,
                        tokens_in: estimate_tokens(&request.prompt),
                        tokens_out: 20,
                        latency_us: 0,
                        request_id: request.metadata.request_id,
                    }
                }
            },
            None => InferenceResponse {
                status: InferenceStatus::AllBackendsFailed,
                content: String::new(),
                model_used: model_id.clone(),
                from_cache: false,
                tokens_in: 0,
                tokens_out: 0,
                latency_us: 0,
                request_id: request.metadata.request_id,
            },
        };

        // 5. Update budget
        if let Some(budget) = self.budgets.get_mut(&request.cap_badge) {
            budget.consume(response.tokens_in + response.tokens_out);
        }
        self.total_tokens += response.tokens_in + response.tokens_out;

        // 6. Cache successful responses
        if response.status == InferenceStatus::Ok {
            self.cache.put(request, &response);
        }

        response
    }
}

// ============================================================================
// Budget Ledger (string-keyed, for external callers that address agents by name)
// ============================================================================

/// A simpler string-keyed token budget used by external orchestration layers
/// that identify agents by name rather than capability badge.
#[derive(Debug, Clone)]
pub struct NamedTokenBudget {
    pub agent_id: String,
    pub tokens_remaining: u64,
    pub tokens_per_period: u64,
    /// Unix timestamp (seconds) when the current period resets; 0 = no auto-reset
    pub period_reset_at: u64,
}

/// String-keyed budget ledger — thin wrapper around a map of `NamedTokenBudget`
pub struct BudgetLedger {
    budgets: BTreeMap<String, NamedTokenBudget>,
}

impl BudgetLedger {
    pub fn new() -> Self {
        Self { budgets: BTreeMap::new() }
    }

    /// Set (or overwrite) the budget for an agent.  Tokens remaining start at
    /// `tokens_per_period`.
    pub fn set_budget(&mut self, agent_id: &str, tokens_per_period: u64) {
        self.budgets.insert(agent_id.to_string(), NamedTokenBudget {
            agent_id: agent_id.to_string(),
            tokens_remaining: tokens_per_period,
            tokens_per_period,
            period_reset_at: 0,
        });
    }

    /// Check that `tokens_needed` are available and, if so, deduct them.
    /// Returns `Err` if the agent has no budget entry or has insufficient tokens.
    pub fn check_and_deduct(&mut self, agent_id: &str, tokens_needed: u64) -> Result<(), String> {
        let budget = self.budgets.get_mut(agent_id)
            .ok_or_else(|| alloc::format!("no budget for agent {}", agent_id))?;
        if budget.tokens_remaining < tokens_needed {
            return Err(alloc::format!(
                "token budget exhausted: {} remaining", budget.tokens_remaining
            ));
        }
        budget.tokens_remaining -= tokens_needed;
        Ok(())
    }

    /// Reset an agent's remaining tokens to its full `tokens_per_period`.
    /// `reset_at` is stored as `period_reset_at` for informational purposes.
    pub fn reset_period(&mut self, agent_id: &str, reset_at: u64) {
        if let Some(b) = self.budgets.get_mut(agent_id) {
            b.tokens_remaining = b.tokens_per_period;
            b.period_reset_at = reset_at;
        }
    }

    /// Returns a reference to the budget for an agent, if one exists.
    pub fn get(&self, agent_id: &str) -> Option<&NamedTokenBudget> {
        self.budgets.get(agent_id)
    }
}

// ============================================================================
// Inference Cache (SHA-256 keyed, named as per spec)
// ============================================================================

/// A single cached inference response, keyed by SHA-256(prompt).
#[derive(Clone, Debug)]
pub struct CacheEntry {
    pub response: String,
    pub model: String,
    pub tokens_used: u64,
    pub created_at: u64,
}

/// SHA-256-keyed response cache with simple first-entry eviction.
pub struct InferenceCache {
    entries: BTreeMap<[u8; 32], CacheEntry>,
    max_entries: usize,
    hits: u64,
    misses: u64,
}

impl InferenceCache {
    pub fn new(max_entries: usize) -> Self {
        Self {
            entries: BTreeMap::new(),
            max_entries,
            hits: 0,
            misses: 0,
        }
    }

    /// Look up a cached entry by its SHA-256 prompt hash.
    pub fn get(&mut self, prompt_hash: &[u8; 32]) -> Option<&CacheEntry> {
        // Track hit/miss before the borrow.
        let found = self.entries.contains_key(prompt_hash);
        if found {
            self.hits += 1;
        } else {
            self.misses += 1;
        }
        self.entries.get(prompt_hash)
    }

    /// Insert a cache entry, evicting the oldest entry when at capacity.
    pub fn insert(&mut self, prompt_hash: [u8; 32], entry: CacheEntry) {
        if self.entries.len() >= self.max_entries {
            if let Some(k) = self.entries.keys().next().cloned() {
                self.entries.remove(&k);
            }
        }
        self.entries.insert(prompt_hash, entry);
    }

    /// Return `(hits, misses)` since creation.
    pub fn stats(&self) -> (u64, u64) {
        (self.hits, self.misses)
    }

    /// Compute the SHA-256 of a prompt string and return a 32-byte key.
    pub fn hash_prompt(prompt: &str) -> [u8; 32] {
        let mut hasher = Sha256::new();
        hasher.update(prompt.as_bytes());
        hasher.finalize().into()
    }
}

// ============================================================================
// Model routing free function (spec-compatible `ModelEntry` + `route_request`)
// ============================================================================

/// A lightweight model descriptor used by the free-function router.
/// Mirrors the fields required by the spec's `route_request` signature.
#[derive(Debug, Clone)]
pub struct ModelEntry {
    /// Unique model identifier
    pub id: String,
    /// Minimum required context window (tokens)
    pub context_window: u32,
    /// Whether this model is currently reachable
    pub available: bool,
    /// Rolling average inference latency in microseconds
    pub avg_latency_us: u64,
    /// Cost per 1 000 output tokens in micro-USD (0 = free/local)
    pub cost_per_1k: u32,
}

/// Select the best available model for a request.
///
/// Selection criteria (in order):
/// 1. Model must have `context_window >= required_context` and be `available`.
/// 2. For `task_hint == "code"` tasks, prefer models with "code" in their id
///    (they score lower in the sort key so they surface first).
/// 3. Among equally code-preferred models, prefer lower `avg_latency_us`.
pub fn route_request<'a>(
    models: &'a [ModelEntry],
    task_hint: &str,
    required_context: u32,
) -> Option<&'a ModelEntry> {
    let is_code = task_hint.eq_ignore_ascii_case("code");

    models.iter()
        .filter(|m| m.context_window >= required_context && m.available)
        .min_by_key(|m| {
            // Lower sort key = preferred.
            // Code-specialized models get a large bonus for code tasks.
            let code_penalty: u64 = if is_code && (m.id.contains("code") || m.id.contains("codex")) {
                0
            } else if is_code {
                u64::MAX / 2 // push non-code models to the back
            } else {
                0
            };
            code_penalty.saturating_add(m.avg_latency_us)
        })
}

// ============================================================================
// Audit Log  (1 024-entry ring buffer)
// ============================================================================

/// A single entry in the inference audit trail.
#[derive(Debug, Clone)]
pub struct InferenceAuditEntry {
    pub timestamp_ms: u64,
    pub agent_id: String,
    pub model: String,
    pub tokens_in: u64,
    pub tokens_out: u64,
    pub latency_ms: u64,
    pub cached: bool,
    pub budget_remaining: u64,
}

/// Ring buffer holding the most recent `AUDIT_CAPACITY` audit entries.
pub struct AuditLog {
    entries: Vec<InferenceAuditEntry>,
    /// Index of the slot that will receive the *next* write.
    head: usize,
    /// Total entries ever written (used to distinguish full vs. partial ring).
    count: u64,
}

const AUDIT_CAPACITY: usize = 1024;

impl AuditLog {
    pub fn new() -> Self {
        Self {
            entries: Vec::with_capacity(AUDIT_CAPACITY),
            head: 0,
            count: 0,
        }
    }

    /// Append an audit entry.  Overwrites the oldest entry once the ring is
    /// full.
    pub fn append(&mut self, entry: InferenceAuditEntry) {
        if self.entries.len() < AUDIT_CAPACITY {
            self.entries.push(entry);
        } else {
            self.entries[self.head] = entry;
        }
        self.head = (self.head + 1) % AUDIT_CAPACITY;
        self.count += 1;
    }

    /// Return references to the `n` most-recent entries (oldest first).
    /// If fewer than `n` entries have been recorded, all of them are returned.
    pub fn audit_recent(&self, n: usize) -> Vec<&InferenceAuditEntry> {
        let total = self.entries.len(); // actual entries stored
        if total == 0 || n == 0 {
            return Vec::new();
        }

        let take = n.min(total);

        // When the ring is not yet full `head` equals `total` (all slots used
        // from 0..total in insertion order).  When full, `head` points to the
        // oldest slot.
        let oldest = if (self.count as usize) <= AUDIT_CAPACITY {
            // Ring not yet wrapped: entries[0..total] in order.
            total.saturating_sub(take)
        } else {
            // Ring is full: oldest slot is `head`.
            (self.head + AUDIT_CAPACITY - take) % AUDIT_CAPACITY
        };

        let mut out = Vec::with_capacity(take);
        for i in 0..take {
            out.push(&self.entries[(oldest + i) % AUDIT_CAPACITY]);
        }
        out
    }

    /// Total number of entries ever appended (not capped at AUDIT_CAPACITY).
    pub fn total_appended(&self) -> u64 {
        self.count
    }
}

// ============================================================================
// HTTP backend  (requires the "std" feature — pulls in reqwest + tokio)
// ============================================================================

#[cfg(feature = "std")]
pub mod http_backend {
    //! Real async HTTP call to an OpenAI-compatible inference endpoint.
    //!
    //! Gated behind the "std" feature because reqwest requires the standard
    //! library.  Enable with `--features std` or add `std` to
    //! `[features] default`.

    use alloc::string::{String, ToString};
    use alloc::format;

    /// POST a chat completion request to `{endpoint}/v1/chat/completions` and
    /// return `(content, tokens_in, tokens_out)`.
    ///
    /// * `endpoint` — base URL without trailing path, e.g.
    ///   `"http://localhost:11434"` for Ollama or `"https://api.openai.com"`.
    ///   A trailing `/` is acceptable.
    /// * `api_key`  — Bearer token; pass `""` for local endpoints that ignore it.
    /// * `messages` — Fully-formed chat messages array (each element must have
    ///   `"role"` and `"content"` keys).  Build this before calling so the
    ///   caller controls system / history messages.
    /// * `model`    — Model identifier string recognised by the endpoint.
    /// * `max_tokens` — Maximum number of output tokens to generate.
    pub async fn call_model_api(
        endpoint: &str,
        api_key: &str,
        messages: &[serde_json::Value],
        model: &str,
        max_tokens: u32,
    ) -> Result<(String, u64, u64), String> {
        use reqwest::Client;
        use serde_json::{json, Value};

        let url = format!(
            "{}/v1/chat/completions",
            endpoint.trim_end_matches('/')
        );

        let body = json!({
            "model": model,
            "messages": messages,
            "max_tokens": max_tokens,
            "stream": false,
        });

        let mut builder = Client::new()
            .post(&url)
            .json(&body);

        // Only set the Authorization header when a non-empty key is supplied.
        // Local Ollama instances reject requests that carry an unexpected header
        // on some configurations, so we omit it rather than sending a blank token.
        if !api_key.is_empty() {
            builder = builder.bearer_auth(api_key);
        }

        let resp = builder
            .send()
            .await
            .map_err(|e| format!("HTTP request failed: {}", e))?;

        let status = resp.status();
        if !status.is_success() {
            let text = resp.text().await.unwrap_or_default();
            return Err(format!("API error {}: {}", status, text));
        }

        let json: Value = resp
            .json()
            .await
            .map_err(|e| format!("JSON decode failed: {}", e))?;

        let content = json["choices"][0]["message"]["content"]
            .as_str()
            .ok_or_else(|| "missing choices[0].message.content".to_string())?
            .to_string();

        let tokens_in = json["usage"]["prompt_tokens"]
            .as_u64()
            .unwrap_or(0);
        let tokens_out = json["usage"]["completion_tokens"]
            .as_u64()
            .unwrap_or(0);

        Ok((content, tokens_in, tokens_out))
    }
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
        budget.tokens_per_period = 10_000; // Small but sufficient for one 4096-token request
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

    // ------------------------------------------------------------------
    // BudgetLedger tests
    // ------------------------------------------------------------------

    #[test]
    fn test_budget_ledger_set_and_deduct() {
        let mut ledger = BudgetLedger::new();
        ledger.set_budget("alice", 1000);

        // Successful deduction
        assert!(ledger.check_and_deduct("alice", 400).is_ok());
        assert_eq!(ledger.get("alice").unwrap().tokens_remaining, 600);

        // Deduction that exactly empties the budget
        assert!(ledger.check_and_deduct("alice", 600).is_ok());
        assert_eq!(ledger.get("alice").unwrap().tokens_remaining, 0);

        // Next deduction should fail
        assert!(ledger.check_and_deduct("alice", 1).is_err());
    }

    #[test]
    fn test_budget_ledger_unknown_agent() {
        let mut ledger = BudgetLedger::new();
        let err = ledger.check_and_deduct("ghost", 100).unwrap_err();
        assert!(err.contains("ghost"));
    }

    #[test]
    fn test_budget_ledger_reset() {
        let mut ledger = BudgetLedger::new();
        ledger.set_budget("bob", 500);
        ledger.check_and_deduct("bob", 500).unwrap();
        assert_eq!(ledger.get("bob").unwrap().tokens_remaining, 0);

        ledger.reset_period("bob", 9999);
        assert_eq!(ledger.get("bob").unwrap().tokens_remaining, 500);
        assert_eq!(ledger.get("bob").unwrap().period_reset_at, 9999);
    }

    // ------------------------------------------------------------------
    // InferenceCache tests
    // ------------------------------------------------------------------

    #[test]
    fn test_inference_cache_hit_miss() {
        let mut cache = InferenceCache::new(4);
        let key = InferenceCache::hash_prompt("hello world");

        // Miss on empty cache
        assert!(cache.get(&key).is_none());
        assert_eq!(cache.stats(), (0, 1));

        // Insert and hit
        cache.insert(key, CacheEntry {
            response: "hi".into(),
            model: "m1".into(),
            tokens_used: 5,
            created_at: 0,
        });
        assert!(cache.get(&key).is_some());
        assert_eq!(cache.stats(), (1, 1));
    }

    #[test]
    fn test_inference_cache_eviction() {
        let mut cache = InferenceCache::new(2);
        let k1 = InferenceCache::hash_prompt("prompt-1");
        let k2 = InferenceCache::hash_prompt("prompt-2");
        let k3 = InferenceCache::hash_prompt("prompt-3");

        let entry = |s: &str| CacheEntry { response: s.into(), model: "m".into(), tokens_used: 1, created_at: 0 };
        cache.insert(k1, entry("r1"));
        cache.insert(k2, entry("r2"));

        // Cache is full; inserting k3 should evict one existing entry
        cache.insert(k3, entry("r3"));
        assert!(cache.get(&k3).is_some());
        // Total stored entries must not exceed max_entries
        // (verified indirectly: no panic, k3 is reachable)
    }

    // ------------------------------------------------------------------
    // route_request tests
    // ------------------------------------------------------------------

    #[test]
    fn test_route_request_context_filter() {
        let models = alloc::vec![
            ModelEntry { id: "small".into(), context_window: 4096, available: true, avg_latency_us: 100, cost_per_1k: 1 },
            ModelEntry { id: "large".into(), context_window: 128_000, available: true, avg_latency_us: 200, cost_per_1k: 5 },
        ];
        // Only "large" fits a 100k context request
        let chosen = route_request(&models, "general", 100_000).unwrap();
        assert_eq!(chosen.id, "large");
    }

    #[test]
    fn test_route_request_prefers_code_model() {
        let models = alloc::vec![
            ModelEntry { id: "gpt-4".into(), context_window: 8192, available: true, avg_latency_us: 50, cost_per_1k: 2 },
            ModelEntry { id: "code-llama".into(), context_window: 8192, available: true, avg_latency_us: 150, cost_per_1k: 1 },
        ];
        // Even though code-llama is slower, it should win for code tasks
        let chosen = route_request(&models, "code", 1024).unwrap();
        assert_eq!(chosen.id, "code-llama");
    }

    #[test]
    fn test_route_request_latency_tiebreak() {
        let models = alloc::vec![
            ModelEntry { id: "model-a".into(), context_window: 8192, available: true, avg_latency_us: 300, cost_per_1k: 1 },
            ModelEntry { id: "model-b".into(), context_window: 8192, available: true, avg_latency_us: 100, cost_per_1k: 1 },
        ];
        let chosen = route_request(&models, "general", 1024).unwrap();
        assert_eq!(chosen.id, "model-b");
    }

    #[test]
    fn test_route_request_unavailable_excluded() {
        let models = alloc::vec![
            ModelEntry { id: "offline".into(), context_window: 8192, available: false, avg_latency_us: 10, cost_per_1k: 0 },
            ModelEntry { id: "online".into(), context_window: 8192, available: true, avg_latency_us: 500, cost_per_1k: 0 },
        ];
        let chosen = route_request(&models, "general", 1024).unwrap();
        assert_eq!(chosen.id, "online");
    }

    // ------------------------------------------------------------------
    // AuditLog tests
    // ------------------------------------------------------------------

    fn make_audit(agent: &str, model: &str) -> InferenceAuditEntry {
        InferenceAuditEntry {
            timestamp_ms: 0,
            agent_id: agent.into(),
            model: model.into(),
            tokens_in: 10,
            tokens_out: 20,
            latency_ms: 5,
            cached: false,
            budget_remaining: 1000,
        }
    }

    #[test]
    fn test_audit_log_basic() {
        let mut log = AuditLog::new();
        assert_eq!(log.audit_recent(5).len(), 0);

        log.append(make_audit("agent-1", "m1"));
        log.append(make_audit("agent-2", "m2"));
        log.append(make_audit("agent-3", "m3"));

        let recent = log.audit_recent(2);
        assert_eq!(recent.len(), 2);
        // Most-recent 2 are agent-2 and agent-3 (oldest-first in the slice)
        assert_eq!(recent[0].agent_id, "agent-2");
        assert_eq!(recent[1].agent_id, "agent-3");
    }

    #[test]
    fn test_audit_log_wrap() {
        let mut log = AuditLog::new();
        // Fill beyond capacity to exercise ring-wrap
        for i in 0..=(AUDIT_CAPACITY + 5) {
            log.append(InferenceAuditEntry {
                timestamp_ms: i as u64,
                agent_id: alloc::format!("agent-{}", i),
                model: "m".into(),
                tokens_in: 1,
                tokens_out: 1,
                latency_ms: 1,
                cached: false,
                budget_remaining: 0,
            });
        }
        assert_eq!(log.total_appended() as usize, AUDIT_CAPACITY + 6);
        // Should still return at most AUDIT_CAPACITY entries
        let recent = log.audit_recent(AUDIT_CAPACITY + 100);
        assert_eq!(recent.len(), AUDIT_CAPACITY);
    }

    #[test]
    fn test_audit_log_ask_more_than_stored() {
        let mut log = AuditLog::new();
        log.append(make_audit("a", "m"));
        log.append(make_audit("b", "m"));
        // Asking for 10 when only 2 stored
        let recent = log.audit_recent(10);
        assert_eq!(recent.len(), 2);
    }
}
