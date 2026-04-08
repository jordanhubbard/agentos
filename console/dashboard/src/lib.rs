mod xterm_bindings;

use leptos::*;
use wasm_bindgen::prelude::*;

// ── PD slot definitions ───────────────────────────────────────────────────────

pub struct PdSlot {
    pub id: usize,
    pub name: &'static str,
    pub display: &'static str,
}

pub const PD_SLOTS: &[PdSlot] = &[
    PdSlot { id: 0,  name: "controller",  display: "controller" },
    PdSlot { id: 1,  name: "event_bus",   display: "event_bus" },
    PdSlot { id: 2,  name: "init_agent",  display: "init_agent" },
    PdSlot { id: 3,  name: "agentfs",     display: "agentfs" },
    PdSlot { id: 4,  name: "vibe_engine", display: "vibe_engine" },
    PdSlot { id: 5,  name: "worker_0",    display: "WASM Worker 0" },
    PdSlot { id: 6,  name: "worker_1",    display: "WASM Worker 1" },
    PdSlot { id: 7,  name: "worker_2",    display: "WASM Worker 2" },
    PdSlot { id: 8,  name: "worker_3",    display: "WASM Worker 3" },
    PdSlot { id: 9,  name: "swap_slot_0", display: "WASM Swap 0" },
    PdSlot { id: 10, name: "swap_slot_1", display: "WASM Swap 1" },
    PdSlot { id: 11, name: "swap_slot_2", display: "WASM Swap 2" },
    PdSlot { id: 12, name: "swap_slot_3", display: "WASM Swap 3" },
    PdSlot { id: 13, name: "console_mux", display: "console_mux" },
    PdSlot { id: 14, name: "linux_vmm",   display: "Linux VM (Buildroot)" },
    PdSlot { id: 15, name: "fault_hndlr", display: "fault_hndlr" },
];

// ── Tab / slot config ─────────────────────────────────────────────────────────

pub struct TabSlot {
    pub tab: &'static str,
    pub slot: usize,
    pub color: &'static str,
}

pub const TAB_SLOTS: &[TabSlot] = &[
    TabSlot { tab: "audit",    slot: 5,  color: "#e3b341" },
    TabSlot { tab: "timeline", slot: 10, color: "#a5d6ff" },
    TabSlot { tab: "metrics",  slot: 8,  color: "#7ee787" },
];

pub const SCROLLBACK_LIMIT: usize = 600;

// ── WebSocket status ──────────────────────────────────────────────────────────

#[derive(Clone, PartialEq)]
pub enum WsStatus {
    Connecting,
    Connected,
    Disconnected,
    Error,
}

// ── Message types ─────────────────────────────────────────────────────────────

#[derive(Clone, Debug, serde::Deserialize)]
#[serde(untagged)]
pub enum WsMessage {
    LogLine {
        slot: usize,
        line: String,
    },
    Event {
        event: String,
        slot: Option<usize>,
        name: Option<String>,
        data: Option<serde_json::Value>,
        msg: Option<String>,
    },
}

// ── ProfilerSnapshot types ────────────────────────────────────────────────────

#[derive(Clone, serde::Deserialize)]
struct ProfilerSnapshot {
    ts: u64,
    slots: Vec<ProfilerSlot>,
}

#[derive(Clone, serde::Deserialize)]
struct ProfilerSlot {
    id: usize,
    name: String,
    cpu_pct: u32,
    mem_kb: u32,
    ticks: u32,
    frames: Vec<Frame>,
}

#[derive(Clone, serde::Deserialize)]
struct Frame {
    #[serde(rename = "fn")]
    fn_name: String,
    ticks: u32,
    depth: u32,
}

// ── Agent type ────────────────────────────────────────────────────────────────

#[derive(Clone, serde::Deserialize)]
struct Agent {
    id: String,
    #[serde(rename = "type")]
    agent_type: String,
    slot: i32,
    name: String,
    status: String,
    lines: usize,
    note: Option<String>,
}

// ── Helper: current time string ───────────────────────────────────────────────

fn now_ts() -> String {
    let perf = web_sys::window()
        .and_then(|w| w.performance())
        .map(|p| p.now())
        .unwrap_or(0.0);
    let secs = (perf / 1000.0) as u64;
    let ms   = (perf % 1000.0) as u32;
    format!("{}.{:03}", secs, ms)
}

// ── WebSocket URL ─────────────────────────────────────────────────────────────

fn ws_url() -> String {
    let loc  = web_sys::window().unwrap().location();
    let host = loc.host().unwrap_or_else(|_| "localhost:8080".to_string());
    format!("ws://{}/", host)
}

// ── connect_ws ────────────────────────────────────────────────────────────────

fn connect_ws(
    set_status: WriteSignal<WsStatus>,
    set_label:  WriteSignal<String>,
    audit_lines:    RwSignal<Vec<String>>,
    timeline_lines: RwSignal<Vec<String>>,
    metrics_lines:  RwSignal<Vec<String>>,
    current_tab: ReadSignal<String>,
    retry: u32,
) {
    use gloo_net::websocket::futures::WebSocket;
    use futures::StreamExt;

    let url = ws_url();
    let ws  = match WebSocket::open(&url) {
        Ok(ws) => ws,
        Err(_) => {
            set_status.set(WsStatus::Error);
            set_label.set("connection failed".to_string());
            schedule_reconnect(
                set_status, set_label,
                audit_lines, timeline_lines, metrics_lines,
                current_tab, retry,
            );
            return;
        }
    };

    let (mut write, mut read) = ws.split();

    // Subscribe to the current tab's slot immediately after opening
    let tab           = current_tab.get_untracked();
    let subscribe_slot: Option<usize> =
        TAB_SLOTS.iter().find(|ts| ts.tab == tab).map(|ts| ts.slot);

    wasm_bindgen_futures::spawn_local(async move {
        use gloo_net::websocket::Message;
        if let Some(slot) = subscribe_slot {
            let msg = format!(r#"{{"action":"subscribe","slot":{}}}"#, slot);
            let _ = futures::SinkExt::send(&mut write, Message::Text(msg)).await;
        }
        // hold write half alive for the duration of the task
        let _ = write;
    });

    set_status.set(WsStatus::Connected);
    set_label.set("connected".to_string());

    wasm_bindgen_futures::spawn_local(async move {
        while let Some(msg) = read.next().await {
            match msg {
                Ok(gloo_net::websocket::Message::Text(text)) => {
                    if let Ok(parsed) = serde_json::from_str::<WsMessage>(&text) {
                        match parsed {
                            WsMessage::LogLine { slot, line } => {
                                let ts    = now_ts();
                                let entry = format!("[{}] {}", ts, line);
                                if let Some(tab_slot) = TAB_SLOTS.iter().find(|ts| ts.slot == slot) {
                                    let sig = match tab_slot.tab {
                                        "audit"    => audit_lines,
                                        "timeline" => timeline_lines,
                                        "metrics"  => metrics_lines,
                                        _          => continue,
                                    };
                                    sig.update(|v| {
                                        v.push(entry);
                                        if v.len() > SCROLLBACK_LIMIT {
                                            let n = v.len() - SCROLLBACK_LIMIT;
                                            v.drain(0..n);
                                        }
                                    });
                                }
                            }
                            WsMessage::Event { msg, .. } => {
                                if let Some(m) = msg {
                                    let ts    = now_ts();
                                    let entry = format!("[{}] {}", ts, m);
                                    audit_lines.update(|v| {
                                        v.push(entry);
                                        if v.len() > SCROLLBACK_LIMIT {
                                            let n = v.len() - SCROLLBACK_LIMIT;
                                            v.drain(0..n);
                                        }
                                    });
                                }
                            }
                        }
                    }
                }
                Ok(_) => {}
                Err(_) => {
                    set_status.set(WsStatus::Disconnected);
                    set_label.set("disconnected".to_string());
                    break;
                }
            }
        }
        set_status.set(WsStatus::Disconnected);
        set_label.set("reconnecting…".to_string());
        schedule_reconnect(
            set_status, set_label,
            audit_lines, timeline_lines, metrics_lines,
            current_tab, retry + 1,
        );
    });
}

fn schedule_reconnect(
    set_status: WriteSignal<WsStatus>,
    set_label:  WriteSignal<String>,
    audit_lines:    RwSignal<Vec<String>>,
    timeline_lines: RwSignal<Vec<String>>,
    metrics_lines:  RwSignal<Vec<String>>,
    current_tab: ReadSignal<String>,
    retry: u32,
) {
    if retry > 10 {
        set_status.set(WsStatus::Error);
        set_label.set("gave up reconnecting".to_string());
        return;
    }
    let delay_ms = (1000u32 * (1u32 << retry.min(4))).min(30_000);
    let timeout  = gloo_timers::callback::Timeout::new(delay_ms, move || {
        connect_ws(
            set_status, set_label,
            audit_lines, timeline_lines, metrics_lines,
            current_tab, retry,
        );
    });
    timeout.forget();
}

// ── App ───────────────────────────────────────────────────────────────────────

#[component]
pub fn App() -> impl IntoView {
    let (current_tab,  set_current_tab) = create_signal(String::from("console"));
    let (ws_status,    set_ws_status)   = create_signal(WsStatus::Connecting);
    let (ws_label,     set_ws_label)    = create_signal(String::from("connecting…"));

    let audit_lines    = create_rw_signal(Vec::<String>::new());
    let timeline_lines = create_rw_signal(Vec::<String>::new());
    let metrics_lines  = create_rw_signal(Vec::<String>::new());

    connect_ws(
        set_ws_status, set_ws_label,
        audit_lines, timeline_lines, metrics_lines,
        current_tab, 0,
    );

    // Re-subscribe when the active tab changes
    create_effect(move |_| {
        let tab = current_tab.get();
        if let Some(tab_slot) = TAB_SLOTS.iter().find(|ts| ts.tab == tab) {
            let slot = tab_slot.slot;
            wasm_bindgen_futures::spawn_local(async move {
                let _ = gloo_net::http::Request::post("/api/agentos/subscribe")
                    .json(&serde_json::json!({"slot": slot}))
                    .unwrap()
                    .send()
                    .await;
            });
        }
    });

    view! {
        <div class="app">
            <Header ws_status=ws_status ws_label=ws_label />
            <div class="tab-bar">
                <TabBar current_tab=current_tab set_current_tab=set_current_tab />
            </div>
            <div class="main-content">
                <Show when=move || current_tab.get() == "console">
                    <ConsoleTab />
                </Show>
                <Show when=move || current_tab.get() == "audit">
                    <LogPanel lines=audit_lines color="#e3b341" placeholder="No audit events yet." />
                </Show>
                <Show when=move || current_tab.get() == "timeline">
                    <LogPanel lines=timeline_lines color="#a5d6ff" placeholder="No timeline events yet." />
                </Show>
                <Show when=move || current_tab.get() == "metrics">
                    <LogPanel lines=metrics_lines color="#7ee787" placeholder="No metrics yet." />
                </Show>
                <Show when=move || current_tab.get() == "profiler">
                    <ProfilerTab />
                </Show>
                <Show when=move || current_tab.get() == "agents">
                    <AgentsTab />
                </Show>
            </div>
        </div>
    }
}

// ── Header ────────────────────────────────────────────────────────────────────

#[component]
fn Header(
    ws_status: ReadSignal<WsStatus>,
    ws_label:  ReadSignal<String>,
) -> impl IntoView {
    view! {
        <header>
            <div class="logo">
                <span class="logo-icon">"◈"</span>
                <h1>"agentOS Console"</h1>
            </div>
            <div class="ws-status">
                <div class=move || match ws_status.get() {
                    WsStatus::Connected    => "ws-dot connected",
                    WsStatus::Error        => "ws-dot error",
                    _                      => "ws-dot",
                } />
                <span id="ws-label">{ws_label}</span>
            </div>
        </header>
    }
}

// ── TabBar ────────────────────────────────────────────────────────────────────

#[component]
fn TabBar(
    current_tab:    ReadSignal<String>,
    set_current_tab: WriteSignal<String>,
) -> impl IntoView {
    let tabs = ["console", "audit", "timeline", "metrics", "profiler", "agents"];
    view! {
        <>
            {tabs.iter().map(|&tab| {
                let tab_str  = tab.to_string();
                let tab_str2 = tab_str.clone();
                view! {
                    <button
                        class=move || if current_tab.get() == tab_str2 {
                            "tab-btn active"
                        } else {
                            "tab-btn"
                        }
                        on:click=move |_| set_current_tab.set(tab_str.clone())
                    >
                        {tab}
                    </button>
                }
            }).collect_view()}
        </>
    }
}

// ── LogPanel ──────────────────────────────────────────────────────────────────

#[component]
fn LogPanel(
    lines:       RwSignal<Vec<String>>,
    color:       &'static str,
    placeholder: &'static str,
) -> impl IntoView {
    let log_ref = create_node_ref::<leptos::html::Div>();

    // Auto-scroll to bottom whenever lines change
    create_effect(move |_| {
        let _ = lines.get(); // subscribe
        if let Some(el) = log_ref.get() {
            let el: &web_sys::HtmlElement = el.as_ref();
            el.set_scroll_top(el.scroll_height());
        }
    });

    view! {
        <div class="tab-panel active" style="display:flex;flex-direction:column;gap:12px;">
            <div class="log-area" node_ref=log_ref>
                {move || {
                    let current_lines = lines.get();
                    if current_lines.is_empty() {
                        view! { <div class="empty-state">{placeholder}</div> }.into_view()
                    } else {
                        current_lines.into_iter().map(|raw_line| {
                            let (ts_part, val_part) = if raw_line.starts_with('[') {
                                if let Some(end) = raw_line.find("] ") {
                                    (raw_line[1..end].to_string(), raw_line[end+2..].to_string())
                                } else {
                                    (String::new(), raw_line.clone())
                                }
                            } else {
                                (String::new(), raw_line)
                            };
                            view! {
                                <div class="log-line">
                                    <span class="ts">{ts_part}</span>
                                    <span class="val" style=format!("color:{}", color)>{val_part}</span>
                                </div>
                            }
                        }).collect_view().into_view()
                    }
                }}
            </div>
        </div>
    }
}

// ── ConsoleTab ────────────────────────────────────────────────────────────────

#[component]
fn ConsoleTab() -> impl IntoView {
    let (tiles,        set_tiles)        = create_signal(Vec::<(usize, String)>::new());
    let (picker_open,  set_picker_open)  = create_signal(false);

    let open_picker  = move |_| set_picker_open.set(true);
    let close_picker = move |_| set_picker_open.set(false);

    // Pre-bind each closure — RSX parser misparses `each=move ||` as a prop named `move`
    let tiles_each = move || tiles.get().into_iter().enumerate().collect::<Vec<_>>();

    view! {
        <div id="tab-console" class="tab-panel active">
            <div id="console-grid" class="console-grid">
                <For
                    each=tiles_each
                    key=|(i, _)| *i
                    children=move |item: (usize, (usize, String))| {
                        let (idx, (slot_id, name)) = item;
                        let title    = format!("slot {} \u{2014} {}", slot_id, name);
                        let term_url = format!("terminal ws://{{host}}/terminal/{}", slot_id);
                        view! {
                            <div class="console-tile">
                                <div class="console-tile-chrome">
                                    <span class="console-tile-title">{title}</span>
                                    <button
                                        class="console-tile-close"
                                        on:click=move |_| {
                                            set_tiles.update(|v| { v.remove(idx); });
                                        }
                                    >"×"</button>
                                </div>
                                <div class="console-tile-term">
                                    <span style="color:var(--dim);font-family:var(--mono);font-size:11px;">
                                        {term_url}
                                    </span>
                                </div>
                            </div>
                        }
                    }
                />
                <button
                    id="console-add-tile"
                    class="console-add-tile"
                    on:click=open_picker
                >
                    "＋ Add Session"
                </button>
            </div>

            <Show when=move || picker_open.get()>
                <div
                    id="slot-picker-overlay"
                    on:click=close_picker
                >
                    <div
                        class="slot-picker-dialog"
                        on:click=|e| e.stop_propagation()
                    >
                        <h3>"Select a PD Slot"</h3>
                        {PD_SLOTS.iter().map(|slot| {
                            let slot_id  = slot.id;
                            let display  = slot.display.to_string();
                            let display2 = display.clone();
                            view! {
                                <button
                                    class="slot-picker-btn"
                                    on:click=move |_| {
                                        set_tiles.update(|v| v.push((slot_id, display2.clone())));
                                        set_picker_open.set(false);
                                    }
                                >
                                    {format!("{} — {}", slot_id, display)}
                                </button>
                            }
                        }).collect_view()}
                        <button class="slot-picker-cancel" on:click=close_picker>
                            "Cancel"
                        </button>
                    </div>
                </div>
            </Show>
        </div>
    }
}

// ── ProfilerTab ───────────────────────────────────────────────────────────────

#[component]
fn ProfilerTab() -> impl IntoView {
    let (snapshot,  set_snapshot) = create_signal(Option::<ProfilerSnapshot>::None);
    let (loading,   set_loading)  = create_signal(false);

    let fetch_snapshot = move |_| {
        set_loading.set(true);
        wasm_bindgen_futures::spawn_local(async move {
            if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/profiler/snapshot")
                .send()
                .await
            {
                if let Ok(snap) = resp.json::<ProfilerSnapshot>().await {
                    set_snapshot.set(Some(snap));
                }
            }
            set_loading.set(false);
        });
    };

    view! {
        <div id="tab-profiler" class="tab-panel active">
            <div class="profiler-header">
                <button class="btn primary" on:click=fetch_snapshot>
                    {move || if loading.get() { "Loading…" } else { "Snapshot" }}
                </button>
                <span class="profiler-ts">
                    {move || snapshot.get().as_ref().map(|s| format!("ts: {}", s.ts)).unwrap_or_default()}
                </span>
            </div>

            {move || snapshot.get().map(|snap| {
                let slots_for_table = snap.slots.clone();
                let slots_for_flame = snap.slots.clone();
                view! {
                    <>
                        <div class="table-wrapper">
                            <table class="slot-table">
                                <thead>
                                    <tr>
                                        <th>"Slot"</th>
                                        <th>"Name"</th>
                                        <th>"CPU %"</th>
                                        <th>"Mem KB"</th>
                                        <th>"Ticks"</th>
                                    </tr>
                                </thead>
                                <tbody>
                                    {slots_for_table.into_iter().map(|s| {
                                        let cpu       = s.cpu_pct;
                                        let bar_class = if cpu >= 90 { "cpu-bar-fill critical" }
                                                        else if cpu >= 60 { "cpu-bar-fill hot" }
                                                        else { "cpu-bar-fill" };
                                        view! {
                                            <tr>
                                                <td>{s.id}</td>
                                                <td>{s.name.clone()}</td>
                                                <td>
                                                    <div class="cpu-bar">
                                                        <div class="cpu-bar-track">
                                                            <div
                                                                class=bar_class
                                                                style=format!("width:{}%", cpu.min(100))
                                                            />
                                                        </div>
                                                        {format!("{}%", cpu)}
                                                    </div>
                                                </td>
                                                <td>{s.mem_kb}</td>
                                                <td>{s.ticks}</td>
                                            </tr>
                                        }
                                    }).collect_view()}
                                </tbody>
                            </table>
                        </div>

                        <span class="section-title">"Flame Graphs"</span>
                        <div class="flame-graphs-container">
                            {slots_for_flame.into_iter()
                                .filter(|s| !s.frames.is_empty())
                                .map(|s| view! {
                                    <div class="flame-section">
                                        <div class="flame-header">
                                            <span class="slot-name">{s.name.clone()}</span>
                                            <span class="slot-meta">{format!("{}% CPU", s.cpu_pct)}</span>
                                        </div>
                                        <div class="flame-body">
                                            <FlameGraph frames=s.frames.clone() />
                                        </div>
                                    </div>
                                })
                                .collect_view()}
                        </div>
                    </>
                }
            })}
        </div>
    }
}

// ── FlameGraph ────────────────────────────────────────────────────────────────

#[component]
fn FlameGraph(frames: Vec<Frame>) -> impl IntoView {
    const ROW_H: u32 = 20;
    const WIDTH: u32 = 700;
    const PAD:   u32 = 2;

    if frames.is_empty() {
        return view! { <svg /> }.into_view();
    }

    let max_depth   = frames.iter().map(|f| f.depth).max().unwrap_or(0);
    let total_ticks: u32 = frames.iter().map(|f| f.ticks).sum();
    let svg_h = (max_depth + 1) * (ROW_H + PAD) + PAD;

    view! {
        <svg
            class="flame-graph"
            width=WIDTH
            height=svg_h
            viewBox=format!("0 0 {} {}", WIDTH, svg_h)
        >
            {frames.into_iter().map(|f| {
                let bar_w  = if total_ticks > 0 {
                    ((f.ticks as f64 / total_ticks as f64) * WIDTH as f64) as u32
                } else { 0 };
                let y     = f.depth * (ROW_H + PAD) + PAD;
                let label = format!("{} ({}t)", f.fn_name, f.ticks);
                view! {
                    <g>
                        <rect
                            x="0"
                            y=y
                            width=bar_w
                            height=ROW_H
                            rx="2"
                            fill="var(--accent-bg)"
                            opacity="0.8"
                        />
                        <text
                            x="4"
                            y=format!("{}", y + ROW_H - 6)
                            fill="var(--text)"
                            font-size="10"
                            font-family="monospace"
                        >
                            {label}
                        </text>
                    </g>
                }
            }).collect_view()}
        </svg>
    }.into_view()
}

// ── AgentsTab ─────────────────────────────────────────────────────────────────

#[component]
fn AgentsTab() -> impl IntoView {
    let (agents,  set_agents)  = create_signal(Vec::<Agent>::new());
    let (loading, set_loading) = create_signal(false);

    // Fetch on mount
    create_effect(move |_| {
        set_loading.set(true);
        wasm_bindgen_futures::spawn_local(async move {
            if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/agents")
                .send()
                .await
            {
                if let Ok(list) = resp.json::<Vec<Agent>>().await {
                    set_agents.set(list);
                }
            }
            set_loading.set(false);
        });
    });

    let refresh = move |_| {
        set_loading.set(true);
        wasm_bindgen_futures::spawn_local(async move {
            if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/agents")
                .send()
                .await
            {
                if let Ok(list) = resp.json::<Vec<Agent>>().await {
                    set_agents.set(list);
                }
            }
            set_loading.set(false);
        });
    };

    // Pre-bind each closure — RSX parser misparses `each=move ||` as a prop named `move`
    let agents_each = move || agents.get();

    view! {
        <div id="tab-agents" class="tab-panel active">
            <div class="agents-toolbar">
                <button class="btn primary" on:click=refresh>
                    {move || if loading.get() { "Loading…" } else { "↻ Refresh" }}
                </button>
            </div>
            <div id="agents-list">
                <Show
                    when=move || !agents.get().is_empty()
                    fallback=|| view! {
                        <div class="empty-state">"No agents found."</div>
                    }
                >
                    <For
                        each=agents_each
                        key=|a: &Agent| a.id.clone()
                        children=move |agent: Agent| {
                            let card_class  = format!("agent-card status-{}", agent.status);
                            let badge_class = format!("agent-badge badge-{}", agent.status);
                            let meta = format!(
                                "type: {}  |  slot: {}  |  lines: {}",
                                agent.agent_type, agent.slot, agent.lines
                            );
                            let note = agent.note.clone();
                            view! {
                                <div class=card_class>
                                    <div class="agent-name">{agent.name.clone()}</div>
                                    <div class="agent-actions">
                                        <span class=badge_class>{agent.status.clone()}</span>
                                    </div>
                                    <div class="agent-meta">{meta}</div>
                                    {note.map(|n| view! {
                                        <div class="agent-note">{n}</div>
                                    })}
                                </div>
                            }
                        }
                    />
                </Show>
            </div>
        </div>
    }
}

// ── WASM entry point ──────────────────────────────────────────────────────────

#[wasm_bindgen(start)]
pub fn main() {
    console_error_panic_hook::set_once();
    leptos::mount_to_body(App);
}
