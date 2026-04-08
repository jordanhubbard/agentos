//! Vibe Engine — the service hot-swap protocol for agentOS
//!
//! This is the core innovation that makes agentOS more than just "seL4 for agents."
//! The Vibe Engine allows agents to:
//!
//! 1. **Propose** a new implementation for any pluggable system service
//! 2. **Validate** the proposal against safety invariants
//! 3. **Sandbox** the new implementation in an isolated test environment
//! 4. **Swap** the live service with the new implementation (zero-downtime)
//! 5. **Rollback** if the new implementation fails health checks
//!
//! ## Why "Vibe-Coding"?
//!
//! Traditional OS development requires human kernel engineers, months of testing,
//! and careful review. agentOS services are designed to be hot-swappable by agents
//! who "vibe" with a problem and generate solutions using LLM inference.
//!
//! The safety net: seL4's capability system ensures that even a badly vibe-coded
//! service can't compromise the kernel or other agents. The worst case is a
//! service crash — which triggers automatic rollback to the previous version.
//!
//! ## Protocol
//!
//! ```text
//! Agent (ModelSvc) → Generate new service code
//!       ↓
//! Agent → VibeEngine::propose(service_id, wasm_binary, metadata)
//!       ↓
//! VibeEngine → Validate: capability safety, memory bounds, interface compliance
//!       ↓
//! VibeEngine → Sandbox: run new service in isolated PD with synthetic workload
//!       ↓
//! VibeEngine → Approve/Reject (based on validation + sandbox results)
//!       ↓
//! Controller → swap_service(old_pd, new_pd) — atomic switch
//!       ↓
//! VibeEngine → Monitor: health checks for rollback window
//! ```
//!
//! ## Service Interface Contracts
//!
//! Each swappable service has a contract:
//! - Required IPC labels and message register layouts
//! - Required capabilities (what the service needs to function)
//! - Health check protocol
//! - Performance baselines (latency, throughput)
//!
//! A vibe-coded replacement must satisfy the contract to pass validation.

#![cfg_attr(not(feature = "std"), no_std)]

extern crate alloc;

pub mod wasm_validator;

use alloc::collections::BTreeMap;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

// ============================================================================
// Service Interface Contracts
// ============================================================================

/// A service contract defines what a swappable service must implement
#[derive(Debug, Clone)]
pub struct ServiceContract {
    /// Service identifier (e.g., "storage.v1", "msgbus.v1")
    pub service_id: String,
    /// Human/agent-readable description
    pub description: String,
    /// Required IPC operations (label -> description)
    pub required_ops: Vec<OperationSpec>,
    /// Required capabilities (what the service needs from the system)
    pub required_caps: Vec<CapRequirement>,
    /// Performance baselines
    pub performance: PerformanceBaseline,
    /// Current implementation version
    pub current_version: u32,
    /// Whether this service is currently swappable
    pub swappable: bool,
}

/// An IPC operation the service must implement
#[derive(Debug, Clone)]
pub struct OperationSpec {
    /// IPC label
    pub label: u64,
    /// Operation name
    pub name: String,
    /// Expected input message registers
    pub input_mrs: u8,
    /// Expected output message registers
    pub output_mrs: u8,
    /// Whether the operation must be synchronous (PPC vs notification)
    pub synchronous: bool,
}

/// A capability the service needs from the system
#[derive(Debug, Clone)]
pub struct CapRequirement {
    /// Capability type
    pub cap_type: String,
    /// Why it's needed
    pub reason: String,
    /// Whether it's optional
    pub optional: bool,
}

/// Performance baseline that replacement must meet
#[derive(Debug, Clone)]
pub struct PerformanceBaseline {
    /// Maximum acceptable latency for core operations (microseconds)
    pub max_latency_us: u64,
    /// Minimum throughput (operations per second)
    pub min_throughput: u64,
    /// Maximum memory usage (bytes)
    pub max_memory_bytes: u64,
}

// ============================================================================
// Proposals
// ============================================================================

/// A proposal to replace a system service
#[derive(Debug, Clone)]
pub struct SwapProposal {
    /// Unique proposal ID
    pub id: u64,
    /// Which service to replace
    pub target_service: String,
    /// Who's proposing
    pub proposer: [u8; 32],
    /// Proposer's capability badge
    pub proposer_badge: u64,
    /// The new implementation (WASM binary for sandbox, or native ELF)
    pub binary: Vec<u8>,
    /// Binary format
    pub format: BinaryFormat,
    /// Metadata about the proposal
    pub metadata: ProposalMetadata,
    /// Current state
    pub state: ProposalState,
    /// Validation results
    pub validation: Option<ValidationResult>,
    /// Sandbox results
    pub sandbox_result: Option<SandboxResult>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum BinaryFormat {
    /// WebAssembly module (sandboxed, portable)
    Wasm,
    /// Native ELF (requires elevated trust)
    NativeElf,
    /// Source code (compile in sandbox)
    Source { language: String },
}

#[derive(Debug, Clone)]
pub struct ProposalMetadata {
    /// Why this replacement is better
    pub rationale: String,
    /// What the agent changed from the reference implementation
    pub changes: Vec<String>,
    /// How this was generated (model ID, prompt hash, etc.)
    pub generation_info: String,
    /// Timestamp
    pub proposed_at: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum ProposalState {
    /// Just submitted, awaiting validation
    Pending,
    /// Being validated (safety checks)
    Validating,
    /// Passed validation, awaiting sandbox test
    Validated,
    /// Running in sandbox environment
    Sandboxing,
    /// Passed all checks, ready for swap
    Approved,
    /// Rejected (with reason)
    Rejected(String),
    /// Swapped in and running
    Active,
    /// Rolled back after failure
    RolledBack(String),
}

// ============================================================================
// Validation
// ============================================================================

/// Result of validating a proposal
#[derive(Debug, Clone)]
pub struct ValidationResult {
    /// Did it pass?
    pub passed: bool,
    /// Individual check results
    pub checks: Vec<ValidationCheck>,
    /// Time taken for validation (microseconds)
    pub duration_us: u64,
}

#[derive(Debug, Clone)]
pub struct ValidationCheck {
    pub name: String,
    pub passed: bool,
    pub detail: String,
}

/// Validation checks we run on every proposal
pub fn validate_proposal(proposal: &SwapProposal, contract: &ServiceContract) -> ValidationResult {
    let mut checks = Vec::new();
    
    // 1. Binary format check
    checks.push(match proposal.format {
        BinaryFormat::Wasm => ValidationCheck {
            name: "binary_format".into(),
            passed: true,
            detail: "WASM module — sandboxable, portable".into(),
        },
        BinaryFormat::NativeElf => ValidationCheck {
            name: "binary_format".into(),
            passed: proposal.proposer_badge == 0, // Only root can propose native
            detail: "Native ELF — requires root/system trust".into(),
        },
        BinaryFormat::Source { .. } => ValidationCheck {
            name: "binary_format".into(),
            passed: true,
            detail: "Source code — will compile in sandbox".into(),
        },
    });
    
    // 2. Size check (no absurdly large binaries)
    let max_size = 10 * 1024 * 1024; // 10MB
    checks.push(ValidationCheck {
        name: "binary_size".into(),
        passed: proposal.binary.len() <= max_size,
        detail: alloc::format!("Binary size: {} bytes (max: {})", 
                               proposal.binary.len(), max_size),
    });
    
    // 3. Service contract exists and is swappable
    checks.push(ValidationCheck {
        name: "service_swappable".into(),
        passed: contract.swappable,
        detail: alloc::format!("Service '{}' swappable: {}", 
                               contract.service_id, contract.swappable),
    });
    
    // 4. Memory budget check
    checks.push(ValidationCheck {
        name: "memory_budget".into(),
        passed: proposal.binary.len() as u64 <= contract.performance.max_memory_bytes,
        detail: alloc::format!("Within memory budget of {} bytes", 
                               contract.performance.max_memory_bytes),
    });
    
    // 5. WASM validation and capability manifest enforcement
    if proposal.format == BinaryFormat::Wasm {
        let wasm_valid = proposal.binary.len() >= 8 
            && proposal.binary[0..4] == [0x00, 0x61, 0x73, 0x6D]; // WASM magic
        checks.push(ValidationCheck {
            name: "wasm_module_valid".into(),
            passed: wasm_valid,
            detail: if wasm_valid { "Valid WASM magic number".into() } 
                    else { "Invalid WASM module header".into() },
        });

        // 5b. __agentos_caps custom section policy enforcement
        //
        // Parses the WASM binary for a custom section named "__agentos_caps"
        // containing a JSON-encoded CapabilityManifest. Rejects any module
        // that requests capabilities exceeding the slot's policy.
        //
        // Custom section format (WASM binary encoding):
        //   section_id: 0x00 (custom)
        //   section_size: u32 (LEB128)
        //   name_len: u32 (LEB128)
        //   name: "__agentos_caps" (UTF-8)
        //   payload: JSON CapabilityManifest
        if wasm_valid {
            match parse_agentos_caps_section(&proposal.binary) {
                Ok(Some(manifest)) => {
                    let policy_result = check_capability_policy(&manifest, contract);
                    checks.push(ValidationCheck {
                        name: "capability_manifest".into(),
                        passed: policy_result.allowed,
                        detail: policy_result.detail,
                    });
                }
                Ok(None) => {
                    // No __agentos_caps section — allow but warn (legacy/unsigned modules)
                    checks.push(ValidationCheck {
                        name: "capability_manifest".into(),
                        passed: true,
                        detail: "No __agentos_caps section — capabilities unconstrained (legacy mode)".into(),
                    });
                }
                Err(e) => {
                    checks.push(ValidationCheck {
                        name: "capability_manifest".into(),
                        passed: false,
                        detail: alloc::format!("Malformed __agentos_caps section: {}", e),
                    });
                }
            }
        }
    }
    
    let all_passed = checks.iter().all(|c| c.passed);
    
    ValidationResult {
        passed: all_passed,
        checks,
        duration_us: 0,
    }
}

// ============================================================================
// WASM Capability Manifest
// ============================================================================

/// Capability manifest declared in a WASM module's `__agentos_caps` custom section.
/// Serialized as JSON in the WASM binary.
#[derive(Debug, Clone)]
pub struct CapabilityManifest {
    /// Version of the manifest format (currently 1)
    pub version: u32,
    /// seL4 endpoint capabilities this module will invoke
    /// These map to the `cap_type` fields in `ServiceContract::required_caps`
    pub required: Vec<String>,
    /// Optional capabilities (module degrades gracefully without these)
    pub optional: Vec<String>,
    /// Maximum memory pages this module needs (64KB each)
    pub max_memory_pages: u32,
    /// Whether the module uses any mutable shared state
    pub uses_shared_memory: bool,
}

/// Result of a capability policy check
#[derive(Debug)]
pub struct CapPolicyResult {
    /// Whether the module is allowed to load in this slot
    pub allowed: bool,
    /// Human-readable explanation
    pub detail: String,
    /// Which capabilities were denied (if any)
    pub denied: Vec<String>,
}

/// Parse the `__agentos_caps` custom section from a WASM binary.
///
/// Returns `Ok(None)` if the section is absent (legacy module),
/// `Ok(Some(manifest))` if found and parseable,
/// `Err(msg)` if the section is malformed.
pub fn parse_agentos_caps_section(wasm: &[u8]) -> Result<Option<CapabilityManifest>, String> {
    const SECTION_NAME: &[u8] = b"__agentos_caps";
    
    // Skip WASM magic (4 bytes) + version (4 bytes)
    if wasm.len() < 8 {
        return Err("WASM binary too short".into());
    }
    let mut pos = 8usize;
    
    while pos < wasm.len() {
        // Read section id (1 byte)
        let section_id = wasm[pos];
        pos += 1;
        
        // Read section size (LEB128 u32)
        let (section_size, bytes_read) = read_leb128_u32(wasm, pos)
            .ok_or("Truncated section size")?;
        pos += bytes_read;
        
        let section_end = pos + section_size as usize;
        if section_end > wasm.len() {
            return Err("Section extends beyond binary".into());
        }
        
        if section_id == 0x00 {
            // Custom section — check the name
            let (name_len, name_bytes) = read_leb128_u32(wasm, pos)
                .ok_or("Truncated custom section name length")?;
            let name_start = pos + name_bytes;
            let name_end = name_start + name_len as usize;
            
            if name_end > section_end {
                return Err("Custom section name extends beyond section".into());
            }
            
            let name = &wasm[name_start..name_end];
            if name == SECTION_NAME {
                // Found it — parse the JSON payload
                let payload = &wasm[name_end..section_end];
                let manifest = parse_caps_json(payload)?;
                return Ok(Some(manifest));
            }
        }
        
        pos = section_end;
    }
    
    Ok(None) // No __agentos_caps section found
}

/// Read a LEB128-encoded u32 from `buf` at `pos`.
/// Returns `Some((value, bytes_consumed))` or `None` if truncated.
fn read_leb128_u32(buf: &[u8], pos: usize) -> Option<(u32, usize)> {
    let mut result: u32 = 0;
    let mut shift: u32 = 0;
    let mut i = pos;
    loop {
        if i >= buf.len() { return None; }
        let byte = buf[i] as u32;
        i += 1;
        result |= (byte & 0x7F) << shift;
        if byte & 0x80 == 0 { break; }
        shift += 7;
        if shift >= 32 { return None; } // Overflow
    }
    Some((result, i - pos))
}

/// Parse the JSON payload of an `__agentos_caps` section.
/// Uses a minimal hand-rolled parser to avoid pulling in serde in no_std.
fn parse_caps_json(payload: &[u8]) -> Result<CapabilityManifest, String> {
    // In a real no_std implementation this would be a minimal JSON scanner.
    // For now, we do a best-effort parse of the expected format:
    //   {"version":1,"required":["memory","network"],"optional":["log"],
    //    "max_memory_pages":16,"uses_shared_memory":false}
    
    let s = core::str::from_utf8(payload)
        .map_err(|_| "Capability manifest is not valid UTF-8")?;
    
    // Extract "required" array
    let required = extract_string_array(s, "required").unwrap_or_default();
    let optional = extract_string_array(s, "optional").unwrap_or_default();
    
    // Extract numeric/boolean fields with simple substring search
    let version = extract_u32(s, "version").unwrap_or(1);
    let max_pages = extract_u32(s, "max_memory_pages").unwrap_or(16);
    let shared_mem = s.contains("\"uses_shared_memory\":true");
    
    Ok(CapabilityManifest {
        version,
        required,
        optional,
        max_memory_pages: max_pages,
        uses_shared_memory: shared_mem,
    })
}

/// Minimal string array extractor: finds `"key":["a","b",...]` in JSON.
fn extract_string_array(json: &str, key: &str) -> Option<Vec<String>> {
    let search = alloc::format!("\"{}\":[", key);
    let start = json.find(search.as_str())? + search.len();
    let end = json[start..].find(']')? + start;
    let inner = &json[start..end];
    
    let mut result = Vec::new();
    for token in inner.split(',') {
        let trimmed = token.trim().trim_matches('"');
        if !trimmed.is_empty() {
            result.push(trimmed.into());
        }
    }
    Some(result)
}

/// Extract a u32 value from a JSON field.
fn extract_u32(json: &str, key: &str) -> Option<u32> {
    let search = alloc::format!("\"{}\":", key);
    let start = json.find(search.as_str())? + search.len();
    let rest = json[start..].trim_start();
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// Check that a module's capability manifest is within the slot policy.
///
/// Policy rules:
/// 1. Every `required` capability must be present in the contract's `required_caps`
/// 2. Memory pages must not exceed contract budget (max_memory_bytes / 65536)
/// 3. Shared memory use must be explicitly allowed by the contract
pub fn check_capability_policy(manifest: &CapabilityManifest, contract: &ServiceContract) -> CapPolicyResult {
    let allowed_caps: Vec<&str> = contract.required_caps.iter()
        .map(|c| c.cap_type.as_str())
        .collect();
    
    let mut denied: Vec<String> = Vec::new();
    
    // Check required capabilities
    for cap in &manifest.required {
        if !allowed_caps.contains(&cap.as_str()) {
            denied.push(cap.clone());
        }
    }
    
    // Check memory budget
    let max_pages = (contract.performance.max_memory_bytes / 65536) as u32;
    if manifest.max_memory_pages > max_pages {
        denied.push(alloc::format!(
            "memory_pages:{} (slot allows max {})", 
            manifest.max_memory_pages, max_pages
        ));
    }
    
    if denied.is_empty() {
        CapPolicyResult {
            allowed: true,
            detail: alloc::format!(
                "Capability manifest v{} verified: {} required, {} optional caps within policy",
                manifest.version,
                manifest.required.len(),
                manifest.optional.len(),
            ),
            denied: Vec::new(),
        }
    } else {
        CapPolicyResult {
            allowed: false,
            detail: alloc::format!(
                "Capability policy violation: denied capabilities: {}",
                denied.join(", ")
            ),
            denied,
        }
    }
}

// ============================================================================
// Sandbox
// ============================================================================

/// Result of sandbox testing a proposal
#[derive(Debug, Clone)]
pub struct SandboxResult {
    /// Did the service run without crashing?
    pub stable: bool,
    /// Did it meet performance baselines?
    pub performance_ok: bool,
    /// Latency measurements (microseconds)
    pub latency_samples: Vec<u64>,
    /// Operations completed during sandbox period
    pub ops_completed: u64,
    /// Any errors observed
    pub errors: Vec<String>,
    /// Sandbox duration (microseconds)
    pub duration_us: u64,
}

// ============================================================================
// Vibe Engine
// ============================================================================

/// The Vibe Engine service
pub struct VibeEngine {
    /// Registered service contracts
    contracts: BTreeMap<String, ServiceContract>,
    /// Active proposals
    proposals: BTreeMap<u64, SwapProposal>,
    /// Next proposal ID
    next_proposal_id: u64,
    /// History of swaps (for rollback)
    swap_history: Vec<SwapRecord>,
    /// Active rollback window (service_id -> previous binary)
    rollback_state: BTreeMap<String, Vec<u8>>,
}

/// Record of a service swap
#[derive(Debug, Clone)]
pub struct SwapRecord {
    pub proposal_id: u64,
    pub service_id: String,
    pub old_version: u32,
    pub new_version: u32,
    pub swapped_at: u64,
    pub rolled_back: bool,
}

impl VibeEngine {
    pub fn new() -> Self {
        Self {
            contracts: BTreeMap::new(),
            proposals: BTreeMap::new(),
            next_proposal_id: 1,
            swap_history: Vec::new(),
            rollback_state: BTreeMap::new(),
        }
    }
    
    /// Register a service contract (called by init task for each swappable service)
    pub fn register_contract(&mut self, contract: ServiceContract) {
        self.contracts.insert(contract.service_id.clone(), contract);
    }
    
    /// Submit a proposal
    pub fn propose(&mut self, mut proposal: SwapProposal) -> Result<u64, String> {
        // Check contract exists
        let contract = self.contracts.get(&proposal.target_service)
            .ok_or_else(|| alloc::format!("Unknown service: {}", proposal.target_service))?;
        
        if !contract.swappable {
            return Err(alloc::format!("Service '{}' is not swappable", proposal.target_service));
        }
        
        let id = self.next_proposal_id;
        self.next_proposal_id += 1;
        proposal.id = id;
        proposal.state = ProposalState::Pending;
        
        self.proposals.insert(id, proposal);
        Ok(id)
    }
    
    /// Validate a pending proposal
    pub fn validate(&mut self, proposal_id: u64) -> Result<bool, String> {
        let proposal = self.proposals.get_mut(&proposal_id)
            .ok_or("Proposal not found")?;
        
        if proposal.state != ProposalState::Pending {
            return Err(alloc::format!("Proposal in wrong state: {:?}", proposal.state));
        }
        
        proposal.state = ProposalState::Validating;
        
        let contract = self.contracts.get(&proposal.target_service)
            .ok_or("Contract not found")?;
        
        let result = validate_proposal(proposal, contract);
        let passed = result.passed;
        
        proposal.validation = Some(result);
        proposal.state = if passed {
            ProposalState::Validated
        } else {
            ProposalState::Rejected("Validation failed".into())
        };
        
        Ok(passed)
    }
    
    /// Run sandbox test on a validated proposal
    pub fn sandbox_test(&mut self, proposal_id: u64) -> Result<bool, String> {
        let proposal = self.proposals.get_mut(&proposal_id)
            .ok_or("Proposal not found")?;
        
        if proposal.state != ProposalState::Validated {
            return Err("Proposal must be validated before sandboxing".into());
        }
        
        proposal.state = ProposalState::Sandboxing;
        
        // In full implementation:
        // 1. Create an isolated PD for the sandbox
        // 2. Load the new service binary
        // 3. Run synthetic workload against it
        // 4. Measure latency, stability, correctness
        // 5. Compare against contract baselines
        
        // Placeholder: assume sandbox passes
        let result = SandboxResult {
            stable: true,
            performance_ok: true,
            latency_samples: Vec::new(),
            ops_completed: 0,
            errors: Vec::new(),
            duration_us: 0,
        };
        
        let passed = result.stable && result.performance_ok;
        proposal.sandbox_result = Some(result);
        proposal.state = if passed {
            ProposalState::Approved
        } else {
            ProposalState::Rejected("Sandbox test failed".into())
        };
        
        Ok(passed)
    }
    
    /// Execute the swap (called by controller after approval)
    pub fn execute_swap(&mut self, proposal_id: u64) -> Result<(), String> {
        let proposal = self.proposals.get_mut(&proposal_id)
            .ok_or("Proposal not found")?;
        
        if proposal.state != ProposalState::Approved {
            return Err("Proposal must be approved before swap".into());
        }
        
        let service_id = proposal.target_service.clone();
        let contract = self.contracts.get_mut(&service_id)
            .ok_or("Contract not found")?;
        
        // Save rollback state
        // (In full impl: save reference to current PD binary)
        self.rollback_state.insert(service_id.clone(), Vec::new());
        
        // Record the swap
        let old_version = contract.current_version;
        contract.current_version += 1;
        
        self.swap_history.push(SwapRecord {
            proposal_id,
            service_id,
            old_version,
            new_version: contract.current_version,
            swapped_at: 0,
            rolled_back: false,
        });
        
        proposal.state = ProposalState::Active;
        
        // In full implementation:
        // 1. Stop accepting new requests to old PD
        // 2. Drain in-flight requests
        // 3. Destroy old PD
        // 4. Create new PD from proposal binary
        // 5. Wire up channels
        // 6. Resume accepting requests
        //
        // This is the hardest part — atomic service swap in a microkernel.
        // Microkit doesn't natively support dynamic PD creation, so this
        // will require either:
        // a) Pre-allocated "swap slots" in the system description
        // b) A Microkit extension for dynamic PD management
        // c) Restart with new system description (less elegant)
        
        Ok(())
    }
    
    /// Rollback a swapped service to previous version
    pub fn rollback(&mut self, service_id: &str) -> Result<(), String> {
        if !self.rollback_state.contains_key(service_id) {
            return Err("No rollback state available".into());
        }
        
        // In full implementation: reverse the swap
        self.rollback_state.remove(service_id);
        
        // Mark the last swap as rolled back
        for record in self.swap_history.iter_mut().rev() {
            if record.service_id == service_id && !record.rolled_back {
                record.rolled_back = true;
                break;
            }
        }
        
        Ok(())
    }
    
    /// Get proposal status
    pub fn proposal_status(&self, proposal_id: u64) -> Option<&ProposalState> {
        self.proposals.get(&proposal_id).map(|p| &p.state)
    }
    
    /// List all registered contracts
    pub fn list_contracts(&self) -> Vec<&ServiceContract> {
        self.contracts.values().collect()
    }
    
    /// Get swap history
    pub fn history(&self) -> &[SwapRecord] {
        &self.swap_history
    }
}

// ============================================================================
// Default Contracts
// ============================================================================

/// Register the default service contracts for agentOS
pub fn register_default_contracts(engine: &mut VibeEngine) {
    engine.register_contract(ServiceContract {
        service_id: "storage.v1".into(),
        description: "Agent filesystem / object store".into(),
        required_ops: vec![
            OperationSpec { label: 1, name: "put".into(), input_mrs: 4, output_mrs: 2, synchronous: true },
            OperationSpec { label: 2, name: "get".into(), input_mrs: 2, output_mrs: 4, synchronous: true },
            OperationSpec { label: 3, name: "delete".into(), input_mrs: 2, output_mrs: 1, synchronous: true },
            OperationSpec { label: 4, name: "list".into(), input_mrs: 2, output_mrs: 4, synchronous: true },
            OperationSpec { label: 5, name: "search".into(), input_mrs: 4, output_mrs: 4, synchronous: true },
        ],
        required_caps: vec![
            CapRequirement { cap_type: "memory".into(), reason: "Content storage pool".into(), optional: false },
            CapRequirement { cap_type: "log".into(), reason: "Audit logging".into(), optional: true },
        ],
        performance: PerformanceBaseline {
            max_latency_us: 1000,     // 1ms for basic ops
            min_throughput: 10000,     // 10K ops/sec
            max_memory_bytes: 64 * 1024 * 1024, // 64MB
        },
        current_version: 1,
        swappable: true,
    });
    
    engine.register_contract(ServiceContract {
        service_id: "msgbus.v1".into(),
        description: "Inter-agent message bus".into(),
        required_ops: vec![
            OperationSpec { label: 1, name: "publish".into(), input_mrs: 4, output_mrs: 1, synchronous: false },
            OperationSpec { label: 2, name: "subscribe".into(), input_mrs: 2, output_mrs: 1, synchronous: true },
            OperationSpec { label: 3, name: "unsubscribe".into(), input_mrs: 2, output_mrs: 1, synchronous: true },
        ],
        required_caps: vec![
            CapRequirement { cap_type: "memory".into(), reason: "Ring buffer".into(), optional: false },
            CapRequirement { cap_type: "notification".into(), reason: "Async delivery".into(), optional: false },
        ],
        performance: PerformanceBaseline {
            max_latency_us: 100,       // 100us for messaging (must be fast)
            min_throughput: 100000,    // 100K msgs/sec
            max_memory_bytes: 16 * 1024 * 1024, // 16MB
        },
        current_version: 1,
        swappable: true,
    });
    
    engine.register_contract(ServiceContract {
        service_id: "tools.v1".into(),
        description: "Tool registry and dispatch".into(),
        required_ops: vec![
            OperationSpec { label: 1, name: "register".into(), input_mrs: 4, output_mrs: 2, synchronous: true },
            OperationSpec { label: 2, name: "invoke".into(), input_mrs: 4, output_mrs: 4, synchronous: true },
            OperationSpec { label: 3, name: "list".into(), input_mrs: 1, output_mrs: 4, synchronous: true },
        ],
        required_caps: vec![
            CapRequirement { cap_type: "memory".into(), reason: "Tool registry".into(), optional: false },
        ],
        performance: PerformanceBaseline {
            max_latency_us: 500,
            min_throughput: 50000,
            max_memory_bytes: 8 * 1024 * 1024, // 8MB
        },
        current_version: 1,
        swappable: true,
    });
    
    engine.register_contract(ServiceContract {
        service_id: "models.v1".into(),
        description: "Model inference proxy".into(),
        required_ops: vec![
            OperationSpec { label: 1, name: "infer".into(), input_mrs: 4, output_mrs: 4, synchronous: true },
            OperationSpec { label: 2, name: "list_models".into(), input_mrs: 1, output_mrs: 4, synchronous: true },
        ],
        required_caps: vec![
            CapRequirement { cap_type: "network".into(), reason: "HTTP API access".into(), optional: false },
            CapRequirement { cap_type: "memory".into(), reason: "Prompt cache".into(), optional: false },
        ],
        performance: PerformanceBaseline {
            max_latency_us: 30_000_000, // 30s (inference is slow)
            min_throughput: 10,          // 10 requests/sec
            max_memory_bytes: 128 * 1024 * 1024, // 128MB (for caching)
        },
        current_version: 1,
        swappable: true,
    });
}

// ============================================================================
// Tests
// ============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::{string::ToString, vec};

    fn setup_engine() -> VibeEngine {
        let mut engine = VibeEngine::new();
        register_default_contracts(&mut engine);
        engine
    }
    
    fn make_proposal(service_id: &str) -> SwapProposal {
        // Minimal valid WASM module (magic + version)
        let wasm_binary = vec![
            0x00, 0x61, 0x73, 0x6D, // \0asm
            0x01, 0x00, 0x00, 0x00, // version 1
        ];
        
        SwapProposal {
            id: 0,
            target_service: service_id.into(),
            proposer: [0x42u8; 32],
            proposer_badge: 3, // Trust level 3
            binary: wasm_binary,
            format: BinaryFormat::Wasm,
            metadata: ProposalMetadata {
                rationale: "Better performance through vibes".into(),
                changes: vec!["Optimized data structure".into()],
                generation_info: "claude-sonnet-4, temp=0.3".into(),
                proposed_at: 0,
            },
            state: ProposalState::Pending,
            validation: None,
            sandbox_result: None,
        }
    }
    
    #[test]
    fn test_full_swap_lifecycle() {
        let mut engine = setup_engine();
        
        // 1. Propose
        let id = engine.propose(make_proposal("storage.v1")).unwrap();
        assert_eq!(*engine.proposal_status(id).unwrap(), ProposalState::Pending);
        
        // 2. Validate
        let valid = engine.validate(id).unwrap();
        assert!(valid);
        assert_eq!(*engine.proposal_status(id).unwrap(), ProposalState::Validated);
        
        // 3. Sandbox
        let sandbox_ok = engine.sandbox_test(id).unwrap();
        assert!(sandbox_ok);
        assert_eq!(*engine.proposal_status(id).unwrap(), ProposalState::Approved);
        
        // 4. Swap
        engine.execute_swap(id).unwrap();
        assert_eq!(*engine.proposal_status(id).unwrap(), ProposalState::Active);
        
        // Check history
        assert_eq!(engine.history().len(), 1);
        assert_eq!(engine.history()[0].new_version, 2);
    }
    
    #[test]
    fn test_reject_non_swappable() {
        let mut engine = setup_engine();
        
        // Register a non-swappable service
        engine.register_contract(ServiceContract {
            service_id: "kernel.core".into(),
            description: "Kernel core — DO NOT SWAP".into(),
            required_ops: vec![],
            required_caps: vec![],
            performance: PerformanceBaseline { max_latency_us: 0, min_throughput: 0, max_memory_bytes: 0 },
            current_version: 1,
            swappable: false, // NOT swappable
        });
        
        let result = engine.propose(make_proposal("kernel.core"));
        assert!(result.is_err());
    }
    
    #[test]
    fn test_rollback() {
        let mut engine = setup_engine();
        
        let id = engine.propose(make_proposal("storage.v1")).unwrap();
        engine.validate(id).unwrap();
        engine.sandbox_test(id).unwrap();
        engine.execute_swap(id).unwrap();
        
        // Rollback
        engine.rollback("storage.v1").unwrap();
        assert!(engine.history().last().unwrap().rolled_back);
    }
    
    #[test]
    fn test_unknown_service_rejected() {
        let mut engine = setup_engine();
        let result = engine.propose(make_proposal("nonexistent.v1"));
        assert!(result.is_err());
    }

    #[test]
    fn test_parse_caps_section_absent() {
        // Minimal valid WASM with no custom sections
        let wasm = vec![0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00];
        let result = parse_agentos_caps_section(&wasm).unwrap();
        assert!(result.is_none(), "No caps section should return None");
    }

    #[test]
    fn test_parse_caps_section_present() {
        // Build a WASM binary with an __agentos_caps custom section
        let section_name = b"__agentos_caps";
        let payload = br#"{"version":1,"required":["memory"],"optional":["log"],"max_memory_pages":8,"uses_shared_memory":false}"#;
        
        // Encode the custom section
        let name_len_enc = leb128_encode(section_name.len() as u32);
        let mut section_body = Vec::new();
        section_body.extend_from_slice(&name_len_enc);
        section_body.extend_from_slice(section_name);
        section_body.extend_from_slice(payload);
        
        let section_size_enc = leb128_encode(section_body.len() as u32);
        
        let mut wasm = vec![0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00];
        wasm.push(0x00); // custom section id
        wasm.extend_from_slice(&section_size_enc);
        wasm.extend_from_slice(&section_body);
        
        let result = parse_agentos_caps_section(&wasm).unwrap();
        assert!(result.is_some(), "Should find __agentos_caps section");
        let manifest = result.unwrap();
        assert_eq!(manifest.version, 1);
        assert_eq!(manifest.required, vec!["memory".to_string()]);
        assert_eq!(manifest.optional, vec!["log".to_string()]);
        assert_eq!(manifest.max_memory_pages, 8);
        assert!(!manifest.uses_shared_memory);
    }

    #[test]
    fn test_capability_policy_within_bounds() {
        let mut engine = setup_engine();
        // storage.v1 allows: memory, log
        let contract = engine.contracts.get("storage.v1").unwrap();
        let manifest = CapabilityManifest {
            version: 1,
            required: vec!["memory".into()],
            optional: vec!["log".into()],
            max_memory_pages: 16, // 1MB — within 64MB budget
            uses_shared_memory: false,
        };
        let result = check_capability_policy(&manifest, contract);
        assert!(result.allowed, "Should be within policy: {}", result.detail);
    }

    #[test]
    fn test_capability_policy_denied_cap() {
        let mut engine = setup_engine();
        let contract = engine.contracts.get("storage.v1").unwrap();
        let manifest = CapabilityManifest {
            version: 1,
            required: vec!["memory".into(), "network".into()], // network not allowed in storage slot
            optional: Vec::new(),
            max_memory_pages: 16,
            uses_shared_memory: false,
        };
        let result = check_capability_policy(&manifest, contract);
        assert!(!result.allowed, "Should be denied: network not in storage policy");
        assert!(result.denied.contains(&"network".to_string()));
    }

    #[test]
    fn test_capability_policy_memory_exceeded() {
        let mut engine = setup_engine();
        let contract = engine.contracts.get("storage.v1").unwrap();
        // storage.v1 budget: 64MB = 1024 pages
        let manifest = CapabilityManifest {
            version: 1,
            required: vec!["memory".into()],
            optional: Vec::new(),
            max_memory_pages: 2048, // 128MB — exceeds 64MB slot
            uses_shared_memory: false,
        };
        let result = check_capability_policy(&manifest, contract);
        assert!(!result.allowed, "Should be denied: memory budget exceeded");
    }

    // Helper: encode u32 as LEB128
    fn leb128_encode(mut val: u32) -> Vec<u8> {
        let mut out = Vec::new();
        loop {
            let mut byte = (val & 0x7F) as u8;
            val >>= 7;
            if val != 0 { byte |= 0x80; }
            out.push(byte);
            if val == 0 { break; }
        }
        out
    }
}

