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
    /// Total registrations
    pub total_registered: u64,
    /// Total invocations
    pub total_invocations: u64,
}

impl ToolRegistry {
    pub fn new() -> Self {
        Self {
            tools: BTreeMap::new(),
            next_badge: 1,
            access_grants: BTreeMap::new(),
            total_registered: 0,
            total_invocations: 0,
        }
    }
    
    /// Register a tool
    pub fn register(&mut self, mut tool: ToolDef) -> Result<u64, ToolStatus> {
        // Check for duplicates from same provider
        if let Some(existing) = self.tools.get(&tool.name) {
            if existing.provider == tool.provider {
                return Err(ToolStatus::ProviderError("Already registered".into()));
            }
            // Different provider — namespace it
            // e.g., "search" becomes "search@agent_xyz"
        }
        
        let badge = self.next_badge;
        self.next_badge += 1;
        tool.badge = badge;
        tool.available = true;
        
        self.tools.insert(tool.name.clone(), tool);
        self.total_registered += 1;
        
        Ok(badge)
    }
    
    /// Unregister a tool (only the provider can do this)
    pub fn unregister(&mut self, name: &str, provider: &[u8; 32]) -> Result<(), ToolStatus> {
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
    
    /// Grant an agent access to a tool
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
    
    /// Invoke a tool
    pub fn invoke(&mut self, call: &ToolCall) -> ToolResult {
        self.total_invocations += 1;
        
        // Check existence and access before taking mutable borrow
        if !self.tools.contains_key(&call.tool_name) {
            return ToolResult {
                status: ToolStatus::NotFound,
                output: Vec::new(),
                exec_time_us: 0,
            };
        }
        if !self.check_access(call.caller_badge, &call.tool_name) {
            return ToolResult {
                status: ToolStatus::AccessDenied,
                output: Vec::new(),
                exec_time_us: 0,
            };
        }

        // Now take the mutable borrow
        let tool = self.tools.get_mut(&call.tool_name).unwrap();
        
        // Check availability
        if !tool.available {
            return ToolResult {
                status: ToolStatus::ProviderError("Tool unavailable".into()),
                output: Vec::new(),
                exec_time_us: 0,
            };
        }
        
        tool.call_count += 1;
        
        // In the real implementation, this would:
        // 1. PPC to the provider PD's channel
        // 2. Pass the input data via shared memory
        // 3. Wait for the reply (with timeout)
        // 4. Return the result
        //
        // For now, return a placeholder
        ToolResult {
            status: ToolStatus::Ok,
            output: Vec::new(), // Would contain provider's response
            exec_time_us: 0,
        }
    }
    
    /// List all tools (MCP-compatible tool listing)
    pub fn list_tools(&self, caller_badge: u64) -> Vec<&ToolDef> {
        self.tools.values()
            .filter(|t| t.available && self.check_access(caller_badge, &t.name))
            .collect()
    }
    
    /// Search tools by keyword in name or description
    pub fn search(&self, query: &str, caller_badge: u64) -> Vec<&ToolDef> {
        let query_lower = query.to_lowercase();
        self.tools.values()
            .filter(|t| {
                t.available && 
                self.check_access(caller_badge, &t.name) &&
                (t.name.to_lowercase().contains(&query_lower) ||
                 t.description.to_lowercase().contains(&query_lower) ||
                 t.tags.iter().any(|tag| tag.to_lowercase().contains(&query_lower)))
            })
            .collect()
    }
    
    /// Get tool statistics
    pub fn stats(&self) -> RegistryStats {
        let available = self.tools.values().filter(|t| t.available).count() as u64;
        let total_calls: u64 = self.tools.values().map(|t| t.call_count).sum();
        
        RegistryStats {
            total_registered: self.total_registered,
            currently_available: available,
            total_invocations: total_calls,
            unique_providers: self.count_unique_providers(),
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

/// Registry statistics
#[derive(Debug)]
pub struct RegistryStats {
    pub total_registered: u64,
    pub currently_available: u64,
    pub total_invocations: u64,
    pub unique_providers: u64,
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
            name: name.into(),
            description: format!("Tool: {}", name),
            input_schema: "{}".into(),
            output_schema: "{}".into(),
            provider,
            provider_channel: 0,
            badge: 0,
            tags: vec!["test".into()],
            call_count: 0,
            total_latency_us: 0,
            available: true,
        }
    }
    
    #[test]
    fn test_register_and_list() {
        let mut reg = ToolRegistry::new();
        let agent = [0x01u8; 32];
        
        reg.register(make_tool("greet", agent)).unwrap();
        reg.register(make_tool("search", agent)).unwrap();
        
        // System badge (0) can see all
        let tools = reg.list_tools(0);
        assert_eq!(tools.len(), 2);
    }
    
    #[test]
    fn test_access_control() {
        let mut reg = ToolRegistry::new();
        let agent = [0x01u8; 32];
        
        reg.register(make_tool("secret_tool", agent)).unwrap();
        
        // Agent badge 42 has no access
        let call = ToolCall {
            tool_name: "secret_tool".into(),
            caller: [0x02u8; 32],
            caller_badge: 42,
            input: Vec::new(),
            timeout_us: 1000,
        };
        
        let result = reg.invoke(&call);
        assert_eq!(result.status, ToolStatus::AccessDenied);
        
        // Grant access
        reg.grant_access(42, "secret_tool".into());
        let result = reg.invoke(&call);
        assert_eq!(result.status, ToolStatus::Ok);
    }
    
    #[test]
    fn test_search() {
        let mut reg = ToolRegistry::new();
        let agent = [0x01u8; 32];
        
        let mut web_tool = make_tool("web_search", agent);
        web_tool.description = "Search the web for information".into();
        web_tool.tags = vec!["web".into(), "search".into()];
        reg.register(web_tool).unwrap();
        
        let mut calc_tool = make_tool("calculator", agent);
        calc_tool.description = "Perform mathematical calculations".into();
        calc_tool.tags = vec!["math".into()];
        reg.register(calc_tool).unwrap();
        
        let results = reg.search("web", 0);
        assert_eq!(results.len(), 1);
        assert_eq!(results[0].name, "web_search");
        
        let results = reg.search("math", 0);
        assert_eq!(results.len(), 1);
        assert_eq!(results[0].name, "calculator");
    }
}
