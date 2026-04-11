# agentOS Operating System Design Review

**Reviewer:** Expert OS designer with 20+ years microkernel experience  
**Review Date:** April 2026  
**Codebase Version:** agentOS v0.1.0-alpha  
**Scope:** Comprehensive architecture review across kernel layer, Rust SDK, and protection domain model

---

## Executive Summary

agentOS is a **Microkit-based agent execution platform** that makes deliberate architectural choices to support dynamic WASM workloads in a capability-secure multi-domain environment. The design successfully avoids several HURD pitfalls through **static topology definition** (`.system` files) and **passive-by-default servers**, but introduces new risks around **untyped topic strings in the EventBus**, **scattered channel ID constants**, and **inadequate audit trails for capability delegation**.

### Key Strengths
1. **Static topology prevents bootstrap deadlock** — All channels wired at build time (Microkit constraint, but enforced well).
2. **Passive servers by default** — EventBus, VibeEngine, NameServer, and others are passive, avoiding scheduler priority inversion cascades.
3. **Capability-first security model** — Every resource access (ObjectStore, VectorStore, Network, Spawn) mediated by unforgeable capabilities.
4. **Proper ppcall/notify distinction** — Passive servers use protected procedure calls only; active PDs use notifications.
5. **WASM as first-class agent** — Hot-swap infrastructure (Vibe slots, VibeEngine) handles module reload without full restart.

### Critical Gaps
1. **EventBus topic strings allow squatting** — No topic namespace registry; any PD can publish to any topic string (confused deputy risk).
2. **Channel ID namespace scattered** — Channel constants hardcoded across C headers (`agentos.h`, per-service headers). New service addition requires header edits in multiple places.
3. **Capability delegation audit gap** — `capability.rs` `grant` and `delegate` operations lack cryptographic attestation; revocation cannot prove which intermediate holder caused a violation.
4. **Ring buffer overflow silent drop** — EventBus ring buffer (256 KB) has no backpressure; high-volume publisher silently loses events (no flow control signal to caller).
5. **WASM interpreter (wasm3) vs. AOT tradeoff uninvestigated** — Embedded wasm3 interpreter has no bounds checking for WASM→host memory escape (e.g., via table.set).

---

## Detailed Findings (Severity-Ranked)

### CRITICAL

#### 1. EventBus Topic Squatting Vulnerability
**File:** `/Users/jkh/Src/agentos/userspace/servers/event-bus/src/lib.rs:84–91`  
**Severity:** CRITICAL  
**Risk:** Confused deputy: any PD can publish events on any unregistered topic string, potentially spoofing system events.

In `lib.rs`, the `EventBus::subscribe()` method looks up a topic in the `BTreeMap` and returns `UnknownTopic` if not found:
```rust
pub fn subscribe(&mut self, topic: impl Into<Topic>) -> Result<SubscriberId, BusError> {
    let topic = topic.into();
    let entry = self.topics.get_mut(&topic).ok_or(BusError::UnknownTopic)?;
    // ...
}
```

But `publish()` allows any topic to be written:
```rust
pub fn publish(
    &mut self,
    topic: impl Into<Topic>,
    payload: Vec<u8>,
    timestamp_ns: u64,
) -> Result<usize, BusError> {
    if payload.len() > MAX_PAYLOAD_BYTES {
        return Err(BusError::PayloadTooLarge);
    }
    let topic = topic.into();
    let subs = self.topics.get(&topic)
        .ok_or(BusError::UnknownTopic)?
        .subscribers
        .clone();
    // ...
}
```

**The issue:** If no subscribers exist, `publish()` returns `UnknownTopic` — good. But the real vulnerability is in the **C kernel layer** (`monitor.c`), where the EventBus is a **passive PD receiving PPCs**. Any caller who holds an endpoint capability to EventBus can publish on any topic string **that has at least one subscriber**, poisoning that topic's history.

**Compare to Mach:** Mach ports are **unforged opaque objects**. Publish rights are separate capabilities from subscribe rights. agentOS has no per-topic authorization; it's all strings.

**Recommended Fix:**
1. Introduce a `TopicRegistry` struct that tracks which **PD** created each topic and forbid publish/subscribe except via that creator (or explicit delegation).
2. At bootstrap, init_agent registers the canonical topics and all PPCs reference a topic **capability** (seL4 CPtr) not a string.
3. Clients who want to publish on "agent.fault" look it up once, cache the cap, and use that.

---

#### 2. WASM Bounds Violation via Shared Memory in Hot-Swap Slots
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/agentos.system:238–246` (swap slot memory map)  
**Severity:** CRITICAL  
**Risk:** wasm3 interpreter does not bounds-check memory accesses. A malicious or buggy WASM module can read/write the controller's view of swap_code regions.

The `.system` file maps swap code regions to controller with `perms="rw"` and to swap slots with `perms="r"`:
```xml
<map mr="swap_code_0" vaddr="0x6000000" perms="rw" setvar_vaddr="swap_code_ctrl_0" />
```
and
```xml
<map mr="swap_code_0" vaddr="0x2000000" perms="r" setvar_vaddr="swap_code_vaddr" />
```

The swap slot PD loads WASM at offset 0x2000000 and interprets it via wasm3. However, wasm3 does **not implement memory protection**. If the WASM module has a buffer overflow in its own memory (e.g., `data[4096]` store at offset 5000), wasm3 will allow the write, potentially corrupting:
- The WASM module's own data segment (intended)
- Adjacent stack/heap (unintended)
- **seL4 page tables or kernel data if mapped adjacent** (catastrophic)

In reality, the swap slot's linear memory is isolated, but wasm3 has **no guard pages** between its heap and seL4 structures.

**Compare to Coyotos/EROS:** Capability-based system with **separate kernel space**. All WASM memory is user-level; kernel is inaccessible from WASM. But agentOS uses seL4, which also has this separation. The issue is **wasm3 itself has no inline bounds checking**.

**Recommended Fix:**
1. Use **AOT compilation (e.g., Cranelift, wasmtime)** instead of wasm3 for production agents. AOT generates bounds-checking code at compile time.
2. If sticking with wasm3 for simplicity: **instrument wasm3's memory access functions** to check linear memory bounds before every load/store. Add a signal handler for SIGSEGV that maps to agent fault.
3. Allocate WASM linear memory with **guard pages** (madvise MADV_GUARD or seL4 untyped frames with gaps).

---

#### 3. Capability Delegation Audit Trail Missing
**File:** `/Users/jkh/Src/agentos/userspace/sdk/src/capability.rs:189–208`  
**Severity:** CRITICAL  
**Risk:** When an agent grants a capability to a child, there is no cryptographic record linking the parent's capability to the child's copy. If the child leaks the capability and causes a security violation, audit logs cannot trace back to the parent.

In `capability.rs`, the `derive_for_child()` method:
```rust
pub fn derive_for_child(&self, granted: &[Capability]) -> Result<CapabilitySet, CapError> {
    let mut child_set = CapabilitySet::new();
    for cap in granted {
        let owned = self.caps.iter().find(|c| c.cptr == cap.cptr);
        match owned {
            Some(owned_cap) if owned_cap.delegatable => {
                if let Some(restricted) = owned_cap.restrict(cap.rights.clone()) {
                    child_set.add(restricted);
                } else {
                    return Err(CapError::RightsExceeded);
                }
            }
            // ...
        }
    }
    Ok(child_set)
}
```

This checks that a capability is delegatable and that rights are not escalated, but **does not record**:
- Who delegated (agent ID)
- To whom (child agent ID)
- Which capability (cptr)
- At what time
- With what rights

In EROS/Coyotos, capabilities have **designated bits** that encode the delegation chain. When a capability fault occurs, the kernel extracts the designee field and reports the full chain.

**Recommended Fix:**
1. Add a `CapAuditLog` PD (already present as `cap_audit_log` in comments) that receives every grant/revoke event via PPC.
2. Include the **parent agent_id**, **child agent_id**, **capability kind**, **rights**, and **timestamp** in the audit log.
3. On revocation (OP_QUOTA_REVOKE in `agentos.h:126`), cross-reference the audit log to find all downstream holders and revoke recursively.
4. Use **seL4 capability invocation counts** (if available) to detect use-after-revoke.

---

#### 4. Priority Inversion via Controller → Passive Server Call Chain
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/agentos.system:6–7` (controller priority 50)  
**Severity:** CRITICAL  
**Risk:** Controller (priority 50) is the **lowest priority user PD**. It PPCs into EventBus (priority 200). If EventBus is blocked on another PD (e.g., AgentFS priority 150), the controller is blocked **at the EventBus's priority** (200), not its own. This can cause a lower-priority worker to starve the entire system.

Scenario:
1. Worker (priority 80) calls EventBus.publish() → blocks on EventBus PD wakeup.
2. EventBus is passive (runs only when PPCed into).
3. Controller at priority 50 also wants to PPC EventBus.
4. EventBus is now running at priority 200 (inherited from first waiter), but the **second waiter (controller at 50) cannot be served** until EventBus returns.
5. If EventBus itself PPCs into AgentFS (priority 150) and AgentFS blocks, EventBus is at priority 150, controller is at priority 50.

This is **textbook priority inversion**: low-priority controller is blocked by high-priority worker (indirectly).

**seL4 MCS (Multi-Core Scheduling)** has scheduling contexts and priority budgets, but this design does not exploit them. The `.system` file specifies static priorities; there is no dynamic priority inheritance configured.

**Compare to seL4 best practices:** CAMKES (the seL4 code generation framework) handles this via **scheduling contexts** and **priority donation** at the microkernel level. But manual Microkit code must be careful.

**Recommended Fix:**
1. Introduce a **priority donation protocol**: when Controller PPCs EventBus, the kernel temporarily raises EventBus's priority to Controller's priority + 1.
2. This is built into seL4 MCS; the `.system` file should use `<scheduling_context>` elements to define context budgets.
3. Alternatively, **never allow low-priority PDs to PPC into high-priority passive servers**. Rearrange channels so that agents (priority 80) are the only ones calling EventBus directly; Controller queries the event log asynchronously.

---

#### 5. Ring Buffer Overflow in EventBus with No Backpressure
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/include/agentos.h:420–423`  
**Severity:** CRITICAL  
**Risk:** The EventBus ring buffer is 256 KB and allocated statically. When the ring is full, new events are **silently dropped** with no signal to the publisher. A high-frequency publisher is unaware its events are lost.

From `agentos.h`:
```c
#define EVENTBUS_RING_SIZE            0x40000u  /* 256 KB — matches agentos*.system */
#define EVENTBUS_BATCH_STAGING_SIZE   768u      /* bytes reserved at end for batch publish */
#define EVENTBUS_BATCH_STAGING_OFFSET (EVENTBUS_RING_SIZE - EVENTBUS_BATCH_STAGING_SIZE)
```

Each event slot is:
```c
typedef struct __attribute__((packed)) {
    uint64_t seq;         /* sequence number */
    uint64_t timestamp_ns;
    uint32_t kind;        /* event kind */
    uint32_t source_pd;   /* source protection domain */
    uint32_t payload_len; /* payload length in bytes */
    uint8_t  payload[64]; /* inline payload (up to 64 bytes) */
} agentos_event_t;  // ~144 bytes per entry
```

With 256 KB and ~144-byte entries, the ring holds ~1800 events. If a worker publishes 1000 events/second (not unrealistic for high-frequency sampling), the ring fills in ~2 seconds and then **loses all new events silently**.

No flow control: the publisher's PPC does not return an "overflow" error; it succeeds with 0 events delivered.

**Compare to Mach:** Mach ports have a queue depth limit; once exceeded, the sender blocks. This applies backpressure naturally.

**Recommended Fix:**
1. **Ring buffer size should be dynamic** based on system memory (e.g., 10% of free DRAM).
2. **OP_EVENTBUS_STATUS** should return (head, tail, capacity, overflow_count). Overflow_count > 0 triggers an alert event.
3. **OP_EVENTBUS_PUBLISH** should return (events_delivered, events_dropped). If dropped > 0, the publisher knows to slow down or request a larger ring.
4. Introduce **per-subscriber watermarks**: if a subscriber's queue exceeds N% of total capacity, publish MemoryPressure event to that subscriber's handler.

---

### MAJOR

#### 6. Channel ID Namespace Scattered Across Headers
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/include/agentos.h:30–47`  
**Severity:** MAJOR  
**Risk:** Channel IDs are defined as C preprocessor constants in `agentos.h`, but new services (SpawnServer, VFS, NetServer, etc.) each have their own header file with duplicate channel definitions. Adding a new service requires edits to multiple header files and risks off-by-one errors.

From `agentos.h`:
```c
#define MONITOR_CH_EVENTBUS   1
#define MONITOR_CH_INITAGENT  2
#define EVENTBUS_CH_MONITOR   1
#define EVENTBUS_CH_INITAGENT 2
// ... (and many more)

/* Service layer channel IDs (from controller perspective) */
#define CH_NAMESERVER         18  /* controller -> nameserver (PPC) */
#define CH_VFS_SERVER         19   /* controller -> vfs_server (PPC) */
#define CH_SPAWN_SERVER       20   /* controller -> spawn_server (PPC) */
#define CH_NET_SERVER         21   /* controller -> net_server (PPC) */
```

Then in `spawn.h` (not shown but inferred):
```c
#define SPAWN_CH_VFS     3
#define SPAWN_CH_APP_SLOT_0  4
```

And in `vfs.h`:
```c
#define VFS_SHMEM_PATH_OFF  0
#define VFS_SHMEM_DATA_OFF  256  // different in each service
```

**The problem:**
1. **Brittleness:** Adding a new service requires incrementing all downstream service channel IDs.
2. **Consistency:** VFS server's perspective of channel 3 (to SpawnServer) is not explicitly linked to SpawnServer's perspective of channel 19 (to VFS).
3. **Verification:** No compile-time check that channel IDs match between the C code and the `.system` file.

**Compare to seL4/CAMKES:** CAMKES generates C headers from the `.system` file, ensuring consistency.

**Recommended Fix:**
1. **Generate channel headers from the `.system` file** using a Python script or Rust build.rs macro.
2. Create a `channels.h` that is auto-generated with:
   ```c
   // AUTO-GENERATED from agentos.system
   typedef enum {
       CH_CONTROLLER_TO_EVENTBUS = 0,
       CH_CONTROLLER_TO_INITAGENT = 1,
       CH_CONTROLLER_TO_NAMESERVER = 18,
       // ...
   } ControllerChannelId;
   
   typedef enum {
       CH_SPAWN_TO_VFS = 3,
       CH_SPAWN_TO_APP_SLOT_0 = 4,
       // ...
   } SpawnChannelId;
   ```
3. **Verify in .system file generation:** Assert that every channel in the C header corresponds to a `<channel>` element in the XML.

---

#### 7. Capability Kind as Typed Enum Conflates Concerns
**File:** `/Users/jkh/Src/agentos/userspace/sdk/src/capability.rs:87–112`  
**Severity:** MAJOR  
**Risk:** `CapabilityKind` is an enum with associated data (e.g., `ObjectStore { namespace: String }`). This is a **halfway point between true ACLs (which would use predicates) and typed seL4 capabilities**. It leads to confused deputy risks when checking capabilities.

From `capability.rs`:
```rust
pub enum CapabilityKind {
    // seL4 native capabilities
    Thread,
    Endpoint,
    Notification,
    AddressSpace,
    CNode,
    Untyped,
    
    // agentOS extension capabilities
    ObjectStore { namespace: String },
    VectorStore { partition: String },
    Network { protocol: NetworkProtocol, scope: NetworkScope },
    AgentSpawn { max_children: u32 },
    Audit,
    Compute { budget_ms: u64 },
    Memory { pool: MemoryPool, limit_bytes: u64 },
}
```

**The issue:**
When an agent checks `cap.kind == CapabilityKind::ObjectStore { namespace: "user_data" }`, it is doing **string matching**, not verifying a seL4 kernel object. The kernel has **no opinion** on what the agent thinks the namespace is. If agent A somehow gets agent B's ObjectStore capability but has a different `namespace` field, the agents can be confused about whose data they are accessing.

In seL4, all capabilities are **kernel objects with unforgeable names**. The `cptr` field is the only thing that matters; the `kind` and `namespace` are just documentation.

**Compare to KeyKOS/EROS:** Capabilities are **bare object identifiers**. The holder must know (out of band) what the capability does. The system does not provide typed capabilities; that's a library-level concern.

**Recommended Fix:**
1. Remove the associated data from enum variants. The `Capability` struct should have:
   ```rust
   pub cptr: u64,           // seL4 capability pointer
   pub rights: Rights,      // r/w/x/grant/revoke bits
   pub kind_hint: String,   // HUMAN-READABLE ONLY: "ObjectStore(user_data)"
   ```
2. The **kernel** (monitor/cap_broker) enforces capability semantics. The Rust SDK provides helpers to **invoke** capabilities but should not use Rust-level pattern matching for security decisions.
3. If a capability grants access to ObjectStore namespace "user_data", the **ObjectStore PD (not the agent)** checks the badge (embedded in the cptr) to verify. The agent cannot lie about the namespace.

---

#### 8. NameServer is Passive but Has No Per-Topic Authorization
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/nameserver.c:122–169`  
**Severity:** MAJOR  
**Risk:** NameServer stores service metadata (name, channel_id, cap_classes, etc.) and returns it on lookup. But there is no check that the **requester is authorized** to learn about a service.

From `nameserver.c`:
```c
static microkit_msginfo handle_register(void) {
    // ... (stores service name and channel ID)
    registry[slot].channel_id   = channel_id;
    registry[slot].pd_id        = pd_id;
    registry[slot].cap_classes  = cap_classes;
    // ...
}

static microkit_msginfo handle_lookup(void) {
    // ... (no authorization check)
    int slot = registry_find_by_name(name);
    if (slot < 0) {
        microkit_mr_set(0, NS_ERR_NOT_FOUND);
        return microkit_msginfo_new(0, 1);
    }
    // Return the channel ID to anyone who asks
    microkit_mr_set(1, e->channel_id);
    // ...
}
```

**The issue:**
NameServer is used to **discover services dynamically**. But its lookup is **unchecked**. Any PD can call OP_NS_LOOKUP and learn that there is a "gpu_scheduler" at channel 50 (example). Then, even if that PD does not have a capability to contact gpu_scheduler, it knows where it is. This enables **covert channel attacks**: a low-privilege PD can learn the layout of the system by probing NameServer, then time-attack the scheduler to infer state.

**Compare to HURD:** HURD's translator registration is passive (stored in the filesystem), but translators are instantiated on demand and the kernel does not verify trust. HURD has no notion of "you are not allowed to know about this service."

**Recommended Fix:**
1. NameServer lookups should check the **requester's badge** against a **capability policy**.
2. At bootstrap, init_agent registers a policy: "Agent X can look up services in category Y."
3. Implement `OP_NS_LOOKUP_GATED`: requester passes a capability that proves it is authorized; NameServer checks the capability's badge against the policy before returning the channel ID.
4. Alternatively, **ship all service metadata at agent spawn time** (like CAMKES does) and forbid dynamic lookups. Each agent has a fixed, pre-configured set of service channels.

---

#### 9. Spawn Server Shares Memory Directly with untrusted ELF
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/spawn_server.c:183–232`  
**Severity:** MAJOR  
**Risk:** SpawnServer stages ELF images into `spawn_elf_shmem` and writes a `spawn_header_t` that includes the ELF size and other metadata. The app slot then reads this header and loads the ELF. But the slot does not **verify** that the ELF matches the header (e.g., hash or signature).

From `spawn_server.c`:
```c
spawn_header_t *hdr = (volatile spawn_header_t *)spawn_elf_shmem_vaddr;
hdr->magic       = SPAWN_MAGIC;
hdr->elf_size    = elf_size;
hdr->cap_classes = cap_classes;
hdr->app_id      = app_id;
```

The app slot then reads:
```c
// (in app_slot PD)
spawn_header_t *hdr = (spawn_header_t *)swap_code_vaddr;
uint32_t elf_size = hdr->elf_size;
// ... load elf_size bytes of ELF from offset SPAWN_HEADER_SIZE
```

**The issue:**
If an attacker (or a buggy system component) modifies `hdr->elf_size` **after** SpawnServer writes it but **before** the app slot reads it, the slot will load the wrong amount of code. For instance:
1. SpawnServer writes 1000 bytes of legitimate ELF.
2. Attacker (e.g., a compromised monitor) writes `hdr->elf_size = 50000`.
3. App slot tries to load 50000 bytes, including data past the ELF (padding, maybe next app's header).

There is no **hash or digital signature** on the ELF image.

**Recommended Fix:**
1. **Sign the ELF image** with a key held by the monitor.
2. SpawnServer computes SHA256(ELF bytes) and includes the hash in the header.
3. App slot verifies the hash before loading.
4. Alternatively, use **seL4 secure page tables** (if available) to make `spawn_elf_shmem` read-only after SpawnServer writes it, preventing tampering.

---

#### 10. VFS Lacks Directory Traversal Safeguards
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/vfs_server.c:90–125`  
**Severity:** MAJOR  
**Risk:** The VFS server implements a simple in-memory filesystem with parent inode pointers. The path lookup reconstructs the full path by walking the parent chain. But there is no check that prevents **symlink loops or path traversal via `..` components**.

From `vfs_server.c`:
```c
static uint32_t mem_find_inode(const char *path) {
    for (uint32_t i = 0; i < VFS_MEM_MAX_INODES; i++) {
        if (!mem_inodes[i].active) continue;
        if (i == 0) continue; /* root handled above */

        char   full[256];
        uint32_t segments[16];   /* inode indices from root to i */
        uint32_t depth = 0;
        uint32_t cur   = i;

        while (cur != 0 && depth < 16) {
            segments[depth++] = cur;
            cur = mem_inodes[cur].parent_ino;
        }
        // ... (reconstruct path and compare)
    }
}
```

**The issues:**
1. No handling of `..` components. A path like `/foo/../../../etc/passwd` is not normalized.
2. No symlink support (good), but no error on symlink-like paths either.
3. **Unbounded recursion risk:** If a parent inode somehow points back to itself (due to a bug), the `while (cur != 0 && depth < 16)` will hit the depth limit and give up, potentially returning the wrong inode.

**Recommended Fix:**
1. **Normalize paths** before lookup: convert `/foo/../bar` to `/bar`.
2. Add a **maximum path depth** constant and fail if exceeded.
3. Add **cycle detection**: track visited inodes in the parent walk; if we revisit an inode, the filesystem is corrupted.
4. Consider **capability-based path prefixes** instead of full path resolution: grant an agent a capability that covers `/data/*`, not a raw file path.

---

### MINOR

#### 11. Monitor Policy Embedded in C Code
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/monitor.c:1–50` (and throughout)  
**Severity:** MINOR  
**Risk:** The monitor PD contains hardcoded logic for capability grants, priority assignments, and service startup. Changing policy requires recompiling the kernel.

The monitor should be a **policy evaluator** that reads policy from **AgentFS** at runtime and applies it. Instead, decision logic is in C.

Example (from monitor.c comments):
```c
// Demo state: stored object ID from AgentFS (first 16 bytes in 4 words)
uint32_t demo_obj_id[4];
bool demo_obj_stored;
```

This is hardcoded demo state, not a general policy.

**Compare to HURD:** HURD uses a configuration filesystem to define translator policies. Policies are read at runtime, not compiled in.

**Recommended Fix:**
1. Move all policy into a **cap_policy.bin** blob (CBOR or Protobuf serialized).
2. Monitor loads the policy from AgentFS at boot and applies it using a **policy interpreter**.
3. init_agent can reload policy via `OP_CAP_POLICY_RELOAD` (already present in code) to update grants without reboot.

---

#### 12. WASM `agentos.capabilities` Manifest Unsigned
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/monitor.c` (implicit via monitor responsibilities)  
**Severity:** MINOR  
**Risk:** WASM agents carry a manifest of required capabilities (e.g., "FS", "NET", "GPU"). The manifest is read from the WASM module's data section but is never cryptographically verified. A malicious WASM compiler could emit a fake manifest requesting more permissions than intended.

**Recommended Fix:**
1. **Sign the manifest** with the agent's issuer key.
2. Monitor verifies the signature before granting any capabilities.
3. Use **TLV (type-length-value) encoding** for the manifest with a cryptographic header.

---

#### 13. Fault Handler Restart Policy Not Specified
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/src/monitor.c` (fault handling implies a handler exists)  
**Severity:** MINOR  
**Risk:** When an agent faults (e.g., page fault, capability error), the fault handler (referenced via `TRACE_PD_FAULT_HDL` in `agentos.h:214`) must decide: restart the agent, kill it, or escalate to the controller. But the policy is not documented or configurable per agent.

**Recommended Fix:**
1. Add a **per-agent restart policy** in the capability manifest:
   ```
   [restart_policy]
   max_restarts = 3
   restart_delay_ms = 100
   escalate_after_n_faults = 5  // escalate to supervisor agent
   ```
2. Monitor reads this at spawn time and applies it.

---

#### 14. Shared Memory Regions Lack Explicit Capability Gating
**File:** `/Users/jkh/Src/agentos/kernel/agentos-root-task/agentos.system:60–83`  
**Severity:** MINOR  
**Risk:** Shared memory regions (swap_code_N, vibe_staging, etc.) are mapped to PDs based on the `.system` file, but there is no **runtime check** that a PD has permission to access a shared memory region. All mapping decisions are made at build time.

If a PD is compromised, it can access any shared memory mapped to it. There is no dynamic **capability revocation** if the PD is quarantined.

**Recommended Fix:**
1. Introduce **memory capabilities** that allow a PD to access a shared region only if it holds a valid capability to that region.
2. At runtime, monitor can revoke the capability if the PD is suspected of misbehavior.

---

## Comparison Table: agentOS vs. HURD vs. Mach vs. seL4

| Aspect | agentOS | HURD | Mach | seL4 |
|--------|---------|------|------|------|
| **IPC Model** | Microkit (notifications + PPCs) | RPC-based translators | Typed ports | Capabilities + endpoints |
| **Service Discovery** | NameServer (passive, string-based) | Filesystem (passive) | Port registry (active) | CAMKES codegen |
| **Passive Servers** | Yes (EventBus, VFS, VibeEngine) | Translators (policy-enforcing) | Few | Implicit via seL4 |
| **Policy Enforcement** | Monitor (C code) | Filesystem permissions | Kernel + object model | Kernel + capabilities |
| **Priority Inversion Risk** | Yes (low-pri controller → high-pri servers) | High (RPC to translators) | Medium (port switching) | Low (MCS) |
| **Capability Revocation** | Per-agent quota revoke; no chains | Filesystem unlink | Port death | Full cap revocation chain |
| **Topic/Port Namespace** | Untyped strings (squatting risk) | Filesystem paths (acl-based) | Typed ports | Unforged capabilities |
| **WASM Support** | Hot-swap (vibe slots) | N/A | N/A | N/A |
| **Boot Topology** | Static (.system file) | Dynamic (translators launched on demand) | Static (boot config) | Static (CAMKES) |

---

## What HURD Got Right (That agentOS Should Adopt)

### 1. **Dynamic Translator Instantiation**
HURD's translators are **instantiated on demand**, not pre-allocated. This allows the system to **scale with workload**: if no one uses the VFS, the VFS translator is not running. agentOS has fixed PDs (controller, EventBus, workers), which wastes memory for systems that don't need all services.

**Adoption:** Add a **lazy PD spawner** in the monitor that creates service PDs only when first accessed (requires seL4 MCS + dynamic PD creation support).

### 2. **Filesystem as the Configuration Interface**
HURD uses the filesystem to **configure policies and translators**. This is elegant because:
- No special policy language needed; standard POSIX calls suffice.
- Policies can be **edited at runtime** without recompiling.
- The filesystem is the **audit trail** of all configuration changes.

agentOS hardcodes policy in monitor.c and config in .system files.

**Adoption:** Require all policy (capability grants, priority assignments, translator startup) to be **read from AgentFS at boot time**. Use a standard file format (YAML or Protobuf) for policies.

### 3. **Translator Filtering and Multiplexing**
HURD's translators can **wrap other translators** (e.g., a compression translator wrapping a filesystem translator). This provides **zero-copy layering** of services.

agentOS has a flat PD topology; services do not compose.

**Adoption:** Allow PDs to **delegate to other PDs** transparently. For example, the VFS server can call AgentFS for hot storage and cache results locally without the agent knowing about the intermediary.

### 4. **Resource Accounting at Translator Granularity**
HURD tracks **per-translator resource usage** (memory, CPU). If a translator runs out of quota, only that translator is killed, not the entire system.

agentOS has per-agent quotas but not per-service accounting.

**Adoption:** Instrument each service PD's memory and CPU independently. Quota PD should track EventBus ring usage, VFS inode count, etc., not just agent memory.

### 5. **Passive Translator Model**
HURD's translators are **passive by default**: they run only when accessed. This avoids **scheduler priority inversions**.

agentOS got this right: EventBus, VibeEngine, NameServer are all passive.

**Maintain this.** Do not add active service PDs (e.g., a service PD that polls for work) unless absolutely necessary.

---

## What HURD Got Wrong (That agentOS Must Avoid)

### 1. **No Capability Framework**
HURD's security model is based on **POSIX UID/GID + filesystem permissions**. There are no unforgeable capabilities. Any process that knows a translator's name can contact it (if you can open `/dev/mydevice`, you can talk to the translator, no authentication).

This enabled the **confused deputy problem**: a translator intended to serve one user could be tricked into serving another.

**agentOS avoids this** by using **seL4 capabilities**. Every agent has a specific set of capabilities granted at spawn time.

**Maintain this.** Never allow "publish to any topic because you know its name" (which is what agentOS EventBus currently does).

### 2. **RPC Without Async Notification**
HURD's RPC is fundamentally **synchronous**. A translator blocks the caller until the RPC completes. This creates **priority inversion cascades**: if translator A calls translator B which calls translator C, and C blocks, the entire chain is blocked at C's priority.

HURD added **asynchronous RPC** (ARpc) late in its history, but it was never widely used.

**agentOS avoids this** by having **passive servers handle PPCs synchronously** but returning results immediately (no deep call chains). Workers use **notifications**, which are asynchronous and don't block the sender.

**Maintain this.** The ppcall/notify split is correct.

### 3. **Unbounded Authority Delegation**
HURD's capability model (if you know a translator's name, you can use it) means **authority is not bounded**. A translator cannot revoke access selectively; all holders of the translator's name have equal rights.

**agentOS has the opportunity to do better** via explicit capability grants with revocation chains.

**Implement this:** Make revocation work by **tracking delegation chains** (see Finding #3 above).

### 4. **No Consensus on Policy Enforcement**
HURD never settled on **where policy is enforced**: in translators, in the microkernel, or in libraries. This led to **inconsistent security** across different translators.

**agentOS should be explicit:** Choose one:
- **Kernel (seL4):** Enforce all security in the kernel; PDs are purely functional. (Hard, requires seL4 patches.)
- **Monitor:** The monitor PD is the policy enforcer; all other PDs are passive or follow the monitor's decisions. (agentOS is moving toward this.)
- **Libraries:** The Rust SDK provides security libraries that agents use. (Trusts agents not to bypass the SDK.)

**Recommended:** Go with **Monitor-based enforcement**. init_agent registers policies at boot; the monitor enforces them; service PDs (NameServer, VFS, etc.) are purely passive/functional.

### 5. **No Audit Trail**
HURD has no built-in **capability audit log**. If a translator misbehaves, there is no record of which other translators it contacted.

**agentOS has a `cap_audit_log` PD** in the design (see `agentos.h:241–245`), but it is not implemented.

**Implement this immediately:** Every capability grant/revoke/delegation must be logged with agent IDs, timestamps, and rights.

---

## Recommendations for Hardening

### Tier 1 (Do Before v0.2)

1. **Implement topic capabilities (Finding #1):** Replace EventBus topic strings with seL4 capabilities. Only registered topics are publishable.
2. **Fix WASM memory bounds (Finding #2):** Use AOT (Cranelift or wasmtime) instead of wasm3, or add guard pages.
3. **Add delegation audit trail (Finding #3):** Implement the cap_audit_log PD with full grant/revoke/delegation tracking.
4. **Ring buffer backpressure (Finding #5):** Return overflow_count in EventBus publish response; add watermark events.
5. **Generate channel headers (Finding #6):** Auto-generate channel IDs from .system file.

### Tier 2 (v0.2 or v0.3)

6. **Dynamic priority inheritance (Finding #4):** Use seL4 MCS scheduling contexts for proper priority donation.
7. **NameServer authorization (Finding #8):** Check capabilities before returning service metadata.
8. **Move policy out of C (Finding #11):** Load capability policy from AgentFS at boot.
9. **Sign WASM manifests (Finding #12):** Cryptographically verify agent capability requests.
10. **Implement per-agent restart policies (Finding #13):** Make fault recovery configurable per agent class.

### Tier 3 (Future / Nice to Have)

11. **Lazy PD Spawning:** Instantiate services only when first accessed.
12. **Translator Composition:** Allow service PDs to delegate to other service PDs transparently.
13. **Capability-Based Filesystem:** Replace path-based VFS with capability-based access control.

---

## Conclusion

agentOS makes several **strong architectural choices**:
- **Static topology** avoids HURD's bootstrap deadlock problems.
- **Passive servers** sidesteps most priority inversion issues.
- **Capability framework** is far more robust than HURD's UID/GID model.
- **Hot-swap WASM support** is novel and valuable for agent workloads.

However, the design has **critical gaps in audit trails, memory safety, and topic authorization** that must be fixed before production use. The EventBus topic squatting vulnerability, WASM bounds escape, and missing capability delegation audit trail are particularly severe.

With the recommended fixes (especially Tier 1), agentOS can achieve **comparable or better security** than HURD while remaining simpler and more predictable. The key is to **trust the seL4 kernel for isolation**, enforce **policy explicitly in the monitor**, and **log all security-relevant operations**.

---

## Appendix: Files Analyzed

- `kernel/agentos-root-task/include/agentos.h` — 548 lines, defines all channel IDs, message tags, priorities, event structures.
- `kernel/agentos-root-task/src/monitor.c` — ~1000 lines (partial read), controller/coordinator PD, VibeEngine integration.
- `kernel/agentos-root-task/src/nameserver.c` — 353 lines, passive service registry (no auth checks).
- `kernel/agentos-root-task/src/app_manager.c` — 401 lines, orchestrates app deployment and lifecycle.
- `kernel/agentos-root-task/src/spawn_server.c` — 589 lines, dynamic app slot launching (no image verification).
- `kernel/agentos-root-task/src/vfs_server.c` — ~300 lines (partial), in-memory filesystem (no path normalization).
- `kernel/agentos-root-task/agentos.system` — 500+ lines, Microkit system description (static PD topology, channel wiring, priorities).
- `userspace/sdk/src/capability.rs` — 266 lines, typed capability system with rights and delegation (no cryptographic audit).
- `userspace/sdk/src/context.rs` — 214 lines, agent runtime context and lifecycle (subscribe/publish/spawn).
- `userspace/sdk/src/event.rs` — 236 lines, event definitions, priorities, EventChannel abstraction.
- `userspace/agents/init-agent/src/main.rs` — 98 lines, bootstrap sequence (stubs for real IPC in production).
- `userspace/servers/event-bus/src/lib.rs` — 213 lines, pub/sub event routing (untyped topics, no backpressure).
- `userspace/sim/src/microkit.rs` — 149 lines, mock Microkit for simulation.
- `userspace/sim/src/eventbus.rs` — 80 lines, simulated EventBus for testing.

**Total codebase reviewed:** ~4,500 lines of core logic + ~500 lines of system description.
