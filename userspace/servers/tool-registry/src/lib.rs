//! Tool Registry — capability-gated tool registration and dispatch for agentOS
//!
//! Agents register callable tools with typed schemas.
//! Other agents invoke tools through capability-gated dispatch.
//! MCP-compatible interface for model-tool interaction.
//!
//! ## How It Works
//!
//! 1. Agent A registers a tool (name, description, input/output schema, handler PD channel)
//! 2. ToolRegistry creates a ToolCap and stores the registration
//! 3. Agent B discovers the tool (query by name, description, or semantic search)
//! 4. Agent B calls the tool via PPC to ToolRegistry
//! 5. ToolRegistry validates Agent B's ToolCap, routes to Agent A's handler
//! 6. Agent A processes the call, returns result through ToolRegistry
//!
//! This is the MCP layer for agentOS — but with hardware-enforced capability gating.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec::Vec;

// ── Capacity limits ───────────────────────────────────────────────────────────

const MAX_TOOLS: usize = 256;
const MAX_INVOCATION_LOG: usize = 512;

// ============================================================================
// Tool Definition
// ============================================================================

/// JSON Schema (simplified for no_std — just a string representation)
pub type Schema = String;

/// A registered tool
#[derive(Debug, Clone)]
pub struct ToolDef {
    /// Tool name (unique per provider)
    pub name: String,
    /// Human/agent-readable description
    pub description: String,
    /// Input parameter schema (JSON Schema)
    pub input_schema: Schema,
    /// Output schema (JSON Schema)
    pub output_schema: Schema,
    /// Agent that provides this tool
    pub provider: [u8; 32],
    /// Microkit channel to the provider PD
    pub provider_channel: u32,
    /// Capability badge for this tool
    pub badge: u64,
    /// Tags for discovery
    pub tags: Vec<String>,
    /// Invocation count (stats)
    pub call_count: u64,
    /// Total latency in microseconds (stats)
    pub total_latency_us: u64,
    /// Whether this tool is currently available
    pub available: bool,
}

/// Tool invocation request
#[derive(Debug, Clone)]
pub struct ToolCall {
    /// Which tool to call
    pub tool_name: String,
    /// Caller's agent ID
    pub caller: [u8; 32],
    /// Caller's capability badge
    pub caller_badge: u64,
    /// Input arguments (CBOR-encoded)
    pub input: Vec<u8>,
    /// Timeout in microseconds
    pub timeout_us: u64,
}

/// Tool invocation result
#[derive(Debug, Clone)]
pub struct ToolResult {
    /// Success or error code
    pub status: ToolStatus,
    /// Output data (CBOR-encoded)
    pub output: Vec<u8>,
    /// Execution time in microseconds
    pub exec_time_us: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ToolStatus {
    Ok,
    NotFound,
    AccessDenied,
    Timeout,
    ProviderError(String),
    InvalidInput(String),
    /// Registry at capacity
    RegistryFull,
}

// ============================================================================
// Invocation Log
// ============================================================================

/// One entry in the invocation log
#[derive(Debug, Clone)]
pub struct InvocationEntry {
    /// Caller-supplied timestamp in milliseconds (0 if unavailable)
    pub timestamp_ms: u64,
    /// Tool name
    pub tool_id: String,
    /// Provider agent ID bytes
    pub agent_id: [u8; 32],
    /// Execution latency in microseconds
    pub latency_us: u64,
    /// Whether the invocation succeeded
    pub success: bool,
}

/// Ring-buffer invocation log (last `MAX_INVOCATION_LOG` entries)
struct InvocationLog {
    entries: Vec<InvocationEntry>,
    head:    usize,
}

impl InvocationLog {
    fn new() -> Self {
        Self { entries: Vec::new(), head: 0 }
    }

    fn push(&mut self, entry: InvocationEntry) {
        if self.entries.len() < MAX_INVOCATION_LOG {
            self.entries.push(entry);
        } else {
            self.entries[self.head] = entry;
            self.head = (self.head + 1) % MAX_INVOCATION_LOG;
        }
    }

    /// Return the last `n` entries in chronological order (most-recent last).
    fn recent(&self, n: usize) -> &[InvocationEntry] {
        let len = self.entries.len();
        if len == 0 { return &[]; }
        let want  = n.min(len);
        let start = len.saturating_sub(want);
        &self.entries[start..]
    }

    fn len(&self) -> usize {
        self.entries.len()
    }
}

// ============================================================================
// Tool Registry
// ============================================================================

/// The tool registry service
pub struct ToolRegistry {
    /// All registered tools (name -> ToolDef)
    tools: BTreeMap<String, ToolDef>,
    /// Badge counter for generating unique tool badges
    next_badge: u64,
    /// Access control: which agent badges can call which tools
    /// (badge -> vec of tool names)
    access_grants: BTreeMap<u64, Vec<String>>,
    /// Total registrations ever (including re-registrations)
    pub total_registered: u64,
    /// Total invocations ever
    pub total_invocations: u64,
    /// Invocation log (last 512 entries)
    invocation_log: InvocationLog,
}

impl ToolRegistry {
    pub fn new() -> Self {
        Self {
            tools:            BTreeMap::new(),
            next_badge:       1,
            access_grants:    BTreeMap::new(),
            total_registered: 0,
            total_invocations: 0,
            invocation_log:   InvocationLog::new(),
        }
    }

    // ── Registration ──────────────────────────────────────────────────────────

    /// Register a tool.  Returns the capability badge assigned to the tool.
    ///
    /// Returns `Err(ToolStatus::RegistryFull)` if the registry already holds
    /// `MAX_TOOLS` distinct tools.
    pub fn register(&mut self, mut tool: ToolDef) -> Result<u64, ToolStatus> {
        // Check for duplicate from same provider
        if let Some(existing) = self.tools.get(&tool.name) {
            if existing.provider == tool.provider {
                return Err(ToolStatus::ProviderError("Already registered".into()));
            }
            // Different provider — allowed (provider is namespaced implicitly via badge)
        }

        if self.tools.len() >= MAX_TOOLS && !self.tools.contains_key(&tool.name) {
            return Err(ToolStatus::RegistryFull);
        }

        let badge = self.next_badge;
        self.next_badge += 1;
        tool.badge     = badge;
        tool.available = true;

        self.tools.insert(tool.name.clone(), tool);
        self.total_registered += 1;

        Ok(badge)
    }

    /// Unregister a tool.  Only the original provider may remove it.
    pub fn deregister(&mut self, name: &str, provider: &[u8; 32]) -> Result<(), ToolStatus> {
        if let Some(tool) = self.tools.get(name) {
            if &tool.provider != provider {
                return Err(ToolStatus::AccessDenied);
            }
        } else {
            return Err(ToolStatus::NotFound);
        }
        self.tools.remove(name);
        Ok(())
    }

    /// Alias for `deregister` (kept for backwards-compat with the old name).
    pub fn unregister(&mut self, name: &str, provider: &[u8; 32]) -> Result<(), ToolStatus> {
        self.deregister(name, provider)
    }

    // ── Access control ────────────────────────────────────────────────────────

    /// Grant an agent badge access to a tool (or "*" for all tools)
    pub fn grant_access(&mut self, agent_badge: u64, tool_name: String) {
        self.access_grants
            .entry(agent_badge)
            .or_insert_with(Vec::new)
            .push(tool_name);
    }

    /// Check if an agent has access to a tool
    fn check_access(&self, caller_badge: u64, tool_name: &str) -> bool {
        // Badge 0 = system/init — always has access
        if caller_badge == 0 {
            return true;
        }
        self.access_grants
            .get(&caller_badge)
            .map(|tools| tools.iter().any(|t| t == tool_name || t == "*"))
            .unwrap_or(false)
    }

    // ── Invocation ────────────────────────────────────────────────────────────

    /// Invoke a tool.
    ///
    /// `timestamp_ms` is a caller-supplied monotonic timestamp used for the
    /// invocation log; pass 0 if not available.
    pub fn invoke(&mut self, call: &ToolCall, timestamp_ms: u64) -> ToolResult {
        self.total_invocations += 1;

        if !self.tools.contains_key(&call.tool_name) {
            self.invocation_log.push(InvocationEntry {
                timestamp_ms,
                tool_id:    call.tool_name.clone(),
                agent_id:   call.caller,
                latency_us: 0,
                success:    false,
            });
            return ToolResult {
                status:       ToolStatus::NotFound,
                output:       Vec::new(),
                exec_time_us: 0,
            };
        }

        if !self.check_access(call.caller_badge, &call.tool_name) {
            self.invocation_log.push(InvocationEntry {
                timestamp_ms,
                tool_id:    call.tool_name.clone(),
                agent_id:   call.caller,
                latency_us: 0,
                success:    false,
            });
            return ToolResult {
                status:       ToolStatus::AccessDenied,
                output:       Vec::new(),
                exec_time_us: 0,
            };
        }

        let tool = self.tools.get_mut(&call.tool_name).unwrap();

        if !tool.available {
            self.invocation_log.push(InvocationEntry {
                timestamp_ms,
                tool_id:    call.tool_name.clone(),
                agent_id:   call.caller,
                latency_us: 0,
                success:    false,
            });
            return ToolResult {
                status:       ToolStatus::ProviderError("Tool unavailable".into()),
                output:       Vec::new(),
                exec_time_us: 0,
            };
        }

        tool.call_count += 1;

        // In the real implementation this would PPC to the provider PD's
        // channel, pass input via shared memory, await the reply, and return
        // the result.  For now we return an empty success payload.
        let exec_time_us: u64 = 0;
        tool.total_latency_us = tool.total_latency_us.saturating_add(exec_time_us);

        self.invocation_log.push(InvocationEntry {
            timestamp_ms,
            tool_id:    call.tool_name.clone(),
            agent_id:   call.caller,
            latency_us: exec_time_us,
            success:    true,
        });

        ToolResult {
            status:       ToolStatus::Ok,
            output:       Vec::new(),
            exec_time_us,
        }
    }

    // ── Listing & search ──────────────────────────────────────────────────────

    /// List all tools visible to `caller_badge` (MCP-compatible tool listing)
    pub fn list(&self, caller_badge: u64) -> Vec<&ToolDef> {
        self.tools.values()
            .filter(|t| t.available && self.check_access(caller_badge, &t.name))
            .collect()
    }

    /// Alias for `list` (kept for call-site compatibility)
    pub fn list_tools(&self, caller_badge: u64) -> Vec<&ToolDef> {
        self.list(caller_badge)
    }

    /// Search tools by keyword in name, description, or tags
    pub fn search(&self, query: &str, caller_badge: u64) -> Vec<&ToolDef> {
        let query_lower = query.to_lowercase();
        self.tools.values()
            .filter(|t| {
                t.available
                    && self.check_access(caller_badge, &t.name)
                    && (t.name.to_lowercase().contains(&query_lower)
                        || t.description.to_lowercase().contains(&query_lower)
                        || t.tags.iter().any(|tag| tag.to_lowercase().contains(&query_lower)))
            })
            .collect()
    }

    // ── Invocation log ────────────────────────────────────────────────────────

    /// Return the last `n` invocation log entries (chronological order).
    pub fn invocation_log_recent(&self, n: usize) -> &[InvocationEntry] {
        self.invocation_log.recent(n)
    }

    /// Total number of invocation log entries (capped at MAX_INVOCATION_LOG)
    pub fn invocation_log_len(&self) -> usize {
        self.invocation_log.len()
    }

    // ── Statistics ────────────────────────────────────────────────────────────

    /// Get aggregate registry statistics
    pub fn stats(&self) -> RegistryStats {
        let available    = self.tools.values().filter(|t| t.available).count() as u64;
        let total_calls: u64 = self.tools.values().map(|t| t.call_count).sum();
        RegistryStats {
            total_registered:    self.total_registered,
            currently_available: available,
            total_invocations:   total_calls,
            unique_providers:    self.count_unique_providers(),
        }
    }

    fn count_unique_providers(&self) -> u64 {
        let mut seen = Vec::new();
        for tool in self.tools.values() {
            if !seen.contains(&tool.provider) {
                seen.push(tool.provider);
            }
        }
        seen.len() as u64
    }
}

impl Default for ToolRegistry {
    fn default() -> Self {
        Self::new()
    }
}

/// Registry statistics
#[derive(Debug)]
pub struct RegistryStats {
    pub total_registered:    u64,
    pub currently_available: u64,
    pub total_invocations:   u64,
    pub unique_providers:    u64,
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::{format, vec};

    fn make_tool(name: &str, provider: [u8; 32]) -> ToolDef {
        ToolDef {
            name:             name.into(),
            description:      format!("Tool: {}", name),
            input_schema:     "{}".into(),
            output_schema:    "{}".into(),
            provider,
            provider_channel: 0,
            badge:            0,
            tags:             vec!["test".into()],
            call_count:       0,
            total_latency_us: 0,
            available:        true,
        }
    }

    fn sys_call(tool_name: &str) -> ToolCall {
        ToolCall {
            tool_name:  tool_name.into(),
            caller:     [0u8; 32],
            caller_badge: 0, // system — always allowed
            input:      Vec::new(),
            timeout_us: 1_000,
        }
    }

    // ── register / list ──────────────────────────────────────────────────────

    #[test]
    fn test_register_and_list() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];

        reg.register(make_tool("greet",  agent)).unwrap();
        reg.register(make_tool("search", agent)).unwrap();

        let tools = reg.list(0);
        assert_eq!(tools.len(), 2);
    }

    #[test]
    fn test_register_duplicate_same_provider() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        reg.register(make_tool("dup", agent)).unwrap();
        let err = reg.register(make_tool("dup", agent)).unwrap_err();
        assert_eq!(err, ToolStatus::ProviderError("Already registered".into()));
    }

    #[test]
    fn test_registry_full() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        for i in 0..MAX_TOOLS {
            reg.register(make_tool(&format!("tool-{}", i), agent)).unwrap();
        }
        let err = reg.register(make_tool("overflow", agent)).unwrap_err();
        assert_eq!(err, ToolStatus::RegistryFull);
    }

    // ── deregister ───────────────────────────────────────────────────────────

    #[test]
    fn test_deregister() {
        let mut reg   = ToolRegistry::new();
        let agent     = [0x01u8; 32];
        let other     = [0x02u8; 32];
        reg.register(make_tool("my-tool", agent)).unwrap();

        // Wrong provider is denied
        assert_eq!(reg.deregister("my-tool", &other), Err(ToolStatus::AccessDenied));
        // Correct provider succeeds
        assert_eq!(reg.deregister("my-tool", &agent), Ok(()));
        // Tool gone
        assert_eq!(reg.list(0).len(), 0);
    }

    #[test]
    fn test_deregister_not_found() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        assert_eq!(reg.deregister("ghost", &agent), Err(ToolStatus::NotFound));
    }

    // ── access control ────────────────────────────────────────────────────────

    #[test]
    fn test_access_control() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        reg.register(make_tool("secret_tool", agent)).unwrap();

        // Badge 42 has no access
        let call = ToolCall {
            tool_name:    "secret_tool".into(),
            caller:       [0x02u8; 32],
            caller_badge: 42,
            input:        Vec::new(),
            timeout_us:   1_000,
        };
        assert_eq!(reg.invoke(&call, 0).status, ToolStatus::AccessDenied);

        // Grant access and retry
        reg.grant_access(42, "secret_tool".into());
        assert_eq!(reg.invoke(&call, 0).status, ToolStatus::Ok);
    }

    #[test]
    fn test_wildcard_access() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        reg.register(make_tool("tool-a", agent)).unwrap();
        reg.register(make_tool("tool-b", agent)).unwrap();
        reg.grant_access(99, "*".into());

        for name in &["tool-a", "tool-b"] {
            let call = ToolCall {
                tool_name:    alloc::string::String::from(*name),
                caller:       [0x03u8; 32],
                caller_badge: 99,
                input:        Vec::new(),
                timeout_us:   1_000,
            };
            assert_eq!(reg.invoke(&call, 0).status, ToolStatus::Ok);
        }
    }

    // ── invoke not found ─────────────────────────────────────────────────────

    #[test]
    fn test_invoke_not_found() {
        let mut reg = ToolRegistry::new();
        let result  = reg.invoke(&sys_call("nonexistent"), 0);
        assert_eq!(result.status, ToolStatus::NotFound);
    }

    // ── invoke unavailable ───────────────────────────────────────────────────

    #[test]
    fn test_invoke_unavailable() {
        let mut reg  = ToolRegistry::new();
        let agent    = [0x01u8; 32];
        reg.register(make_tool("toggled", agent)).unwrap();
        reg.tools.get_mut("toggled").unwrap().available = false;
        let result = reg.invoke(&sys_call("toggled"), 0);
        assert!(matches!(result.status, ToolStatus::ProviderError(_)));
    }

    // ── search ────────────────────────────────────────────────────────────────

    #[test]
    fn test_search() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];

        let mut web = make_tool("web_search", agent);
        web.description = "Search the web for information".into();
        web.tags        = vec!["web".into(), "search".into()];
        reg.register(web).unwrap();

        let mut calc = make_tool("calculator", agent);
        calc.description = "Perform mathematical calculations".into();
        calc.tags        = vec!["math".into()];
        reg.register(calc).unwrap();

        let results = reg.search("web",  0);
        assert_eq!(results.len(), 1);
        assert_eq!(results[0].name, "web_search");

        let results = reg.search("math", 0);
        assert_eq!(results.len(), 1);
        assert_eq!(results[0].name, "calculator");
    }

    // ── invocation log ────────────────────────────────────────────────────────

    #[test]
    fn test_invocation_log_records_entries() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        reg.register(make_tool("ping", agent)).unwrap();

        reg.invoke(&sys_call("ping"),        100);
        reg.invoke(&sys_call("ping"),        200);
        reg.invoke(&sys_call("nonexistent"), 300);

        assert_eq!(reg.invocation_log_len(), 3);

        let entries = reg.invocation_log_recent(10);
        assert_eq!(entries.len(), 3);
        assert!(entries[0].success);
        assert!(entries[1].success);
        assert!(!entries[2].success);
    }

    #[test]
    fn test_invocation_log_recent_limited() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        reg.register(make_tool("p", agent)).unwrap();

        for ts in 0..20u64 {
            reg.invoke(&sys_call("p"), ts);
        }
        let recent = reg.invocation_log_recent(5);
        assert_eq!(recent.len(), 5);
    }

    #[test]
    fn test_invocation_log_wraps() {
        let mut reg = ToolRegistry::new();
        let agent   = [0x01u8; 32];
        reg.register(make_tool("q", agent)).unwrap();

        for ts in 0..(MAX_INVOCATION_LOG + 20) as u64 {
            reg.invoke(&sys_call("q"), ts);
        }
        assert_eq!(reg.invocation_log_len(), MAX_INVOCATION_LOG);
    }

    #[test]
    fn test_stats() {
        let mut reg   = ToolRegistry::new();
        let agent_a   = [0x01u8; 32];
        let agent_b   = [0x02u8; 32];
        reg.register(make_tool("t1", agent_a)).unwrap();
        reg.register(make_tool("t2", agent_b)).unwrap();

        reg.invoke(&sys_call("t1"), 0);
        reg.invoke(&sys_call("t2"), 0);

        let s = reg.stats();
        assert_eq!(s.currently_available, 2);
        assert_eq!(s.total_invocations,   2);
        assert_eq!(s.unique_providers,    2);
    }
}
