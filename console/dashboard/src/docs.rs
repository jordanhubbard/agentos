use leptos::*;

struct DocSection {
    id:      &'static str,
    title:   &'static str,
    content: &'static str,
}

const SECTIONS: &[DocSection] = &[
    DocSection {
        id: "architecture",
        title: "Architecture Overview",
        content: r#"
<h3>agentOS Architecture</h3>
<p>agentOS is a capability-based microkernel OS built on seL4 Microkit. Every service
runs in an isolated <strong>Protection Domain (PD)</strong> — a lightweight seL4 process with
a fixed capability set. No PD can access another's memory or invoke another's services
without an explicit capability grant.</p>

<h4>Key design principles</h4>
<ul>
  <li><strong>Capability-based isolation</strong> — all cross-PD communication is mediated by seL4 capabilities</li>
  <li><strong>WASM agents</strong> — userspace workloads run as signed .wasm modules in swap_slot PDs</li>
  <li><strong>Hot-swap</strong> — VibeEngine can atomically replace a WASM agent's code without reboot</li>
  <li><strong>Event-driven</strong> — EventBus provides topic-based pub/sub between PDs</li>
  <li><strong>Zero-copy IPC</strong> — shared memory regions bypass the IPC buffer for bulk data</li>
</ul>

<h4>Boot sequence</h4>
<ol>
  <li>controller (monitor) boots, patches <code>setvar_vaddr</code> globals via Microkit image</li>
  <li>Registers all services with NameServer</li>
  <li>Runs 5-step demo: AgentFS PUT/GET → EventBus pub/sub → Worker tasks → VibeEngine hot-swap → AppManager launch</li>
</ol>
"#,
    },
    DocSection {
        id: "pd-topology",
        title: "PD Topology",
        content: r#"
<h3>Protection Domain Topology</h3>
<p>The <strong>Topology</strong> tab shows the live PD graph. Each box is a PD; arrows are IPC channels.</p>

<h4>Core PDs</h4>
<table>
  <tr><th>PD</th><th>Role</th><th>Priority</th></tr>
  <tr><td>controller</td><td>Root task — boots system, orchestrates demo</td><td>254</td></tr>
  <tr><td>nameserver</td><td>Service registry (passive PD)</td><td>200</td></tr>
  <tr><td>event_bus</td><td>Topic pub/sub, ring-buffer backed</td><td>190</td></tr>
  <tr><td>init_agent</td><td>Userspace init, registers topics</td><td>150</td></tr>
  <tr><td>agentfs</td><td>Object store (key→value blob)</td><td>170</td></tr>
  <tr><td>vibe_engine</td><td>WASM validator + hot-swap coordinator</td><td>160</td></tr>
  <tr><td>vfs_server</td><td>Virtual filesystem (path-based)</td><td>155</td></tr>
  <tr><td>spawn_server</td><td>ELF loader + PD spawner</td><td>145</td></tr>
  <tr><td>app_manager</td><td>App lifecycle (launch/kill/status)</td><td>130</td></tr>
  <tr><td>worker_0..3</td><td>WASM worker pool (task dispatch)</td><td>120</td></tr>
  <tr><td>swap_slot_0..3</td><td>Hot-swappable WASM PDs</td><td>110</td></tr>
</table>

<h4>Edge types</h4>
<ul>
  <li><strong style="color:#fff">Solid white</strong> — Microkit PPC (Protected Procedure Call)</li>
  <li><strong style="color:#e8a94f">Dashed yellow</strong> — Shared memory region</li>
  <li><strong style="color:#7170ff">Purple</strong> — EventBus subscription</li>
  <li><strong style="color:#10b981">Green dotted</strong> — Network/VMM channel</li>
</ul>
"#,
    },
    DocSection {
        id: "ipc-reference",
        title: "IPC Reference",
        content: r#"
<h3>IPC Reference</h3>
<p>All cross-PD communication uses seL4 Microkit's IPC primitives.</p>

<h4>Message Registers</h4>
<p>Each PPC call carries up to 8 message registers (MR0–MR7). By convention:</p>
<ul>
  <li>MR0 — opcode (also set in msginfo label)</li>
  <li>MR1–MR7 — arguments / return values</li>
</ul>

<h4>Channel IDs (from controller)</h4>
<pre>
CH_EVENTBUS     = 0
CH_INITAGENT    = 1
CH_AGENTFS      = 5
CH_NAMESERVER   = 18
CH_VFS_SERVER   = 19
CH_SPAWN_SERVER = 20
CH_NET_SERVER   = 21
CH_VIRTIO_BLK   = 22
CH_APP_MANAGER  = 23
CH_HTTP_SVC     = 24
CH_VIBEENGINE   = 40
CH_GPUSCHED     = 50
</pre>

<h4>Common opcodes</h4>
<pre>
OP_NS_REGISTER      = 0xD0
OP_NS_LOOKUP        = 0xD1
OP_APP_LAUNCH       = 0xA0
OP_APP_KILL         = 0xA1
OP_APP_LIST         = 0xA2
OP_VFS_OPEN         = 0xC0
OP_VFS_READ         = 0xC1
OP_SPAWN_LAUNCH     = 0xB0
</pre>
"#,
    },
    DocSection {
        id: "wasm-agents",
        title: "WASM Agents",
        content: r#"
<h3>WASM Agent Development</h3>
<p>Userspace workloads are signed <code>.wasm</code> modules executed by swap_slot PDs via the wasm3 interpreter.</p>

<h4>Required exports</h4>
<pre>
init()                             -- called once at PD boot
handle_ppc(i64,i64,i64,i64,i64)  -- handles an incoming PPC
health_check() → i32              -- 0=healthy, non-zero=degraded
</pre>

<h4>Host imports (from "env" namespace)</h4>
<pre>
aos_log(ptr: i32, len: i32)            -- print to serial
aos_time_us() → i64                    -- boot time in µs
aos_mem_read(addr, buf, len) → i32     -- read shared memory
aos_mem_write(addr, buf, len) → i32    -- write shared memory
</pre>

<h4>Signing</h4>
<p>Agents must be signed with <code>sign-wasm</code> before deployment:</p>
<pre>sign-wasm agent.wasm --key-id 0000000000000001</pre>
<p>The signature covers the <code>agentos.capabilities</code> custom section (SHA-512).</p>

<h4>Capability section format</h4>
<p>Add an <code>agentos.capabilities</code> custom section to your WASM declaring
required capabilities as a comma-separated list:</p>
<pre>network,objectstore,compute</pre>
<p>The AppManager checks these against the system policy before launching.</p>
"#,
    },
];

#[component]
pub fn DocsPanel() -> impl IntoView {
    let (active, set_active) = create_signal(SECTIONS[0].id);

    view! {
        <div class="docs-panel">
            <nav class="docs-nav">
                {SECTIONS.iter().map(|sec| {
                    let sec_id = sec.id;
                    view! {
                        <button
                            class=move || if active.get() == sec_id {
                                "docs-nav-btn active"
                            } else {
                                "docs-nav-btn"
                            }
                            on:click=move |_| set_active.set(sec_id)
                        >
                            {sec.title}
                        </button>
                    }
                }).collect_view()}
            </nav>

            <div class="docs-content">
                {move || {
                    let id = active.get();
                    let section = SECTIONS.iter().find(|s| s.id == id)?;
                    let content = section.content;
                    Some(view! {
                        <div class="docs-body" inner_html=content />
                    })
                }}
            </div>
        </div>
    }
}
