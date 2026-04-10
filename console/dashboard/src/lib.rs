mod docs;
mod images;
mod topology;
mod xterm_bindings;

use std::rc::Rc;
use std::cell::RefCell;

use leptos::*;
use wasm_bindgen::prelude::*;
use wasm_bindgen::JsCast;

use topology::{
    NodeMetrics, NodeStatus, TopologyMini, TopologyPanel,
    NUM_EDGES, edges_for_slot,
};
use images::ImagesPanel;
use docs::DocsPanel;

// ── PD slot definitions ───────────────────────────────────────────────────────

pub struct PdSlot {
    pub id:      usize,
    pub name:    &'static str,
    pub display: &'static str,
}

pub const PD_SLOTS: &[PdSlot] = &[
    PdSlot { id:  0, name: "controller",  display: "controller"             },
    PdSlot { id:  1, name: "event_bus",   display: "event_bus"              },
    PdSlot { id:  2, name: "init_agent",  display: "init_agent"             },
    PdSlot { id:  3, name: "agentfs",     display: "agentfs"                },
    PdSlot { id:  4, name: "vibe_engine", display: "vibe_engine"            },
    PdSlot { id:  5, name: "worker_0",    display: "WASM Worker 0"          },
    PdSlot { id:  6, name: "worker_1",    display: "WASM Worker 1"          },
    PdSlot { id:  7, name: "worker_2",    display: "WASM Worker 2"          },
    PdSlot { id:  8, name: "worker_3",    display: "WASM Worker 3"          },
    PdSlot { id:  9, name: "swap_slot_0", display: "WASM Swap 0"            },
    PdSlot { id: 10, name: "swap_slot_1", display: "WASM Swap 1"            },
    PdSlot { id: 11, name: "swap_slot_2", display: "WASM Swap 2"            },
    PdSlot { id: 12, name: "swap_slot_3", display: "WASM Swap 3"            },
    PdSlot { id: 13, name: "console_mux", display: "console_mux"            },
    PdSlot { id: 14, name: "linux_vmm",   display: "Linux VM (Buildroot)"   },
    PdSlot { id: 15, name: "fault_hndlr", display: "fault_hndlr"            },
];

pub const SCROLLBACK_LIMIT: usize = 600;

// ── WebSocket types ───────────────────────────────────────────────────────────

#[derive(Clone, PartialEq)]
pub enum WsStatus { Connecting, Connected, Disconnected, Error }

#[derive(Clone, Debug, serde::Deserialize)]
#[serde(untagged)]
pub enum WsMessage {
    LogLine { slot: usize, line: String },
    Event {
        event: String, slot: Option<usize>, name: Option<String>,
        data: Option<serde_json::Value>, msg: Option<String>,
    },
}

// ── Profiler types ────────────────────────────────────────────────────────────

#[derive(Clone, serde::Deserialize)]
struct ProfilerSnapshot { ts: u64, slots: Vec<ProfilerSlot> }

#[derive(Clone, serde::Deserialize)]
struct ProfilerSlot {
    id: usize, name: String, cpu_pct: u32, mem_kb: u32, ticks: u32,
    frames: Vec<Frame>,
}

#[derive(Clone, serde::Deserialize)]
struct Frame {
    #[serde(rename = "fn")] fn_name: String,
    ticks: u32, depth: u32,
}

// ── Agent type ────────────────────────────────────────────────────────────────

#[derive(Clone, serde::Deserialize)]
struct Agent {
    id: String, #[serde(rename = "type")] agent_type: String,
    slot: i32, name: String, status: String, lines: usize,
    note: Option<String>,
}

// ── Log-panel tab config ──────────────────────────────────────────────────────

struct LogTabSlot { tab: &'static str, slot: usize, color: &'static str }

const LOG_TABS: &[LogTabSlot] = &[
    LogTabSlot { tab: "audit",    slot: 5,  color: "#e3b341" },
    LogTabSlot { tab: "timeline", slot: 10, color: "#a5d6ff" },
    LogTabSlot { tab: "metrics",  slot: 8,  color: "#7ee787" },
];

// ── Helpers ───────────────────────────────────────────────────────────────────

fn now_ts() -> String {
    let p = web_sys::window().and_then(|w| w.performance()).map(|p| p.now()).unwrap_or(0.0);
    format!("{}.{:03}", (p / 1000.0) as u64, (p % 1000.0) as u32)
}

fn ws_url(path: &str) -> String {
    let loc  = web_sys::window().unwrap().location();
    let host = loc.host().unwrap_or_else(|_| "localhost:8080".to_string());
    format!("ws://{}{}", host, path)
}

fn now_perf() -> f64 {
    web_sys::window().and_then(|w| w.performance()).map(|p| p.now()).unwrap_or(0.0)
}

// ── Main log WebSocket ────────────────────────────────────────────────────────

fn connect_ws(
    set_status:     WriteSignal<WsStatus>,
    set_label:      WriteSignal<String>,
    audit_lines:    RwSignal<Vec<String>>,
    timeline_lines: RwSignal<Vec<String>>,
    metrics_lines:  RwSignal<Vec<String>>,
    topo_metrics:   RwSignal<Vec<NodeMetrics>>,
    active_edges:   RwSignal<Vec<bool>>,
    retry: u32,
) {
    use gloo_net::websocket::futures::WebSocket;
    use futures::StreamExt;

    let url = ws_url("/ws");
    let ws  = match WebSocket::open(&url) {
        Ok(ws) => ws,
        Err(_) => {
            set_status.set(WsStatus::Error);
            set_label.set("connection failed".to_string());
            schedule_reconnect(set_status, set_label, audit_lines, timeline_lines,
                               metrics_lines, topo_metrics, active_edges, retry);
            return;
        }
    };

    let (_write, mut read) = ws.split();
    set_status.set(WsStatus::Connected);
    set_label.set("connected".to_string());

    wasm_bindgen_futures::spawn_local(async move {
        while let Some(msg) = read.next().await {
            if let Ok(gloo_net::websocket::Message::Text(text)) = msg {
                if let Ok(parsed) = serde_json::from_str::<WsMessage>(&text) {
                    match parsed {
                        WsMessage::LogLine { slot, line } => {
                            // Log panels
                            let ts    = now_ts();
                            let entry = format!("[{}] {}", ts, line);
                            if let Some(ls) = LOG_TABS.iter().find(|ls| ls.slot == slot) {
                                let sig = match ls.tab {
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
                            // Topology edge animation
                            let indices = edges_for_slot(slot);
                            if !indices.is_empty() {
                                active_edges.update(|v| {
                                    for &i in &indices { if i < v.len() { v[i] = true; } }
                                });
                                let ae = active_edges;
                                let timeout = gloo_timers::callback::Timeout::new(500, move || {
                                    ae.update(|v| {
                                        for &i in &indices { if i < v.len() { v[i] = false; } }
                                    });
                                });
                                timeout.forget();
                            }
                            // Mark node status as Ready
                            if slot < topology::NUM_NODES {
                                topo_metrics.update(|v| {
                                    if let Some(m) = v.get_mut(slot) {
                                        if m.status == NodeStatus::Unknown {
                                            m.status = NodeStatus::Ready;
                                        }
                                    }
                                });
                            }
                        }
                        WsMessage::Event { msg, .. } => {
                            if let Some(m) = msg {
                                let entry = format!("[{}] {}", now_ts(), m);
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
            } else if msg.is_err() {
                set_status.set(WsStatus::Disconnected);
                set_label.set("reconnecting…".to_string());
                break;
            }
        }
        set_status.set(WsStatus::Disconnected);
        set_label.set("reconnecting…".to_string());
        schedule_reconnect(set_status, set_label, audit_lines, timeline_lines,
                           metrics_lines, topo_metrics, active_edges, retry + 1);
    });
}

fn schedule_reconnect(
    set_status:     WriteSignal<WsStatus>,
    set_label:      WriteSignal<String>,
    audit_lines:    RwSignal<Vec<String>>,
    timeline_lines: RwSignal<Vec<String>>,
    metrics_lines:  RwSignal<Vec<String>>,
    topo_metrics:   RwSignal<Vec<NodeMetrics>>,
    active_edges:   RwSignal<Vec<bool>>,
    retry: u32,
) {
    if retry > 10 {
        set_status.set(WsStatus::Error);
        set_label.set("gave up reconnecting".to_string());
        return;
    }
    let delay_ms = (1000u32 * (1u32 << retry.min(4))).min(30_000);
    let t = gloo_timers::callback::Timeout::new(delay_ms, move || {
        connect_ws(set_status, set_label, audit_lines, timeline_lines,
                   metrics_lines, topo_metrics, active_edges, retry);
    });
    t.forget();
}

// ── App ───────────────────────────────────────────────────────────────────────

#[component]
pub fn App() -> impl IntoView {
    let (current_panel, set_panel) = create_signal(String::from("topology"));
    let (ws_status,  set_ws_status) = create_signal(WsStatus::Connecting);
    let (ws_label,   set_ws_label)  = create_signal(String::from("connecting…"));
    let (sidebar_open, set_sidebar) = create_signal(true);

    let audit_lines    = create_rw_signal(Vec::<String>::new());
    let timeline_lines = create_rw_signal(Vec::<String>::new());
    let metrics_lines  = create_rw_signal(Vec::<String>::new());

    // Topology state
    let topo_metrics: RwSignal<Vec<NodeMetrics>> =
        create_rw_signal(vec![NodeMetrics::default(); topology::NUM_NODES]);
    let active_edges: RwSignal<Vec<bool>> =
        create_rw_signal(vec![false; NUM_EDGES]);

    // Console tile state — lifted to App so topology clicks can open terminals
    let (tiles, set_tiles) = create_signal(Vec::<(usize, String)>::new());

    connect_ws(
        set_ws_status, set_ws_label,
        audit_lines, timeline_lines, metrics_lines,
        topo_metrics, active_edges, 0,
    );

    // Periodic profiler fetch → update topo_metrics CPU/mem
    {
        let tm = topo_metrics;
        let interval = gloo_timers::callback::Interval::new(2_000, move || {
            let tm2 = tm;
            wasm_bindgen_futures::spawn_local(async move {
                if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/profiler/snapshot")
                    .send().await
                {
                    if let Ok(snap) = resp.json::<ProfilerSnapshot>().await {
                        tm2.update(|v| {
                            for s in &snap.slots {
                                if s.id < v.len() {
                                    v[s.id].cpu_pct = s.cpu_pct;
                                    v[s.id].mem_kb  = s.mem_kb as u64;
                                    if v[s.id].status == NodeStatus::Unknown {
                                        v[s.id].status = NodeStatus::Ready;
                                    }
                                }
                            }
                        });
                    }
                }
            });
        });
        interval.forget();
    }

    let sp = set_panel;
    let st = set_tiles;

    view! {
        <div class="app">
            <AppHeader ws_status=ws_status ws_label=ws_label
                       sidebar_open=sidebar_open set_sidebar=set_sidebar />
            <div class="workspace">
                // ── Sidebar ────────────────────────────────────────────────
                <aside class=move || if sidebar_open.get() { "sidebar open" } else { "sidebar" }>
                    <Show when=move || sidebar_open.get()>
                        <TopologyMini
                            metrics=topo_metrics
                            active_edges=active_edges
                            set_panel=sp
                            set_tiles=st
                        />
                    </Show>

                    <nav class="sidebar-nav">
                        {[
                            ("topology", "⬡", "Topology"),
                            ("console",  "⬜", "Console"),
                            ("profiler", "◎", "Profiler"),
                            ("agents",   "◈", "Agents"),
                            ("images",   "⬇", "Images"),
                            ("docs",     "⁇", "Docs"),
                        ].iter().map(|(id, icon, label)| {
                            let id_str    = *id;
                            let icon_str  = *icon;
                            let label_str = *label;
                            let sp2 = sp;
                            view! {
                                <button
                                    class=move || if current_panel.get() == id_str {
                                        "sidebar-nav-btn active"
                                    } else {
                                        "sidebar-nav-btn"
                                    }
                                    title=label_str
                                    on:click=move |_| sp2.set(id_str.to_string())
                                >
                                    <span class="nav-icon">{icon_str}</span>
                                    <Show when=move || sidebar_open.get()>
                                        <span class="nav-label">{label_str}</span>
                                    </Show>
                                </button>
                            }
                        }).collect_view()}
                    </nav>

                    <Show when=move || sidebar_open.get()>
                        <SidebarStats topo_metrics=topo_metrics ws_status=ws_status />
                    </Show>
                </aside>

                // ── Main panel area ────────────────────────────────────────
                <main class="panel-area">
                    <Show when=move || current_panel.get() == "topology">
                        <TopologyPanel
                            metrics=topo_metrics
                            active_edges=active_edges
                            set_panel=sp
                            set_tiles=st
                        />
                    </Show>
                    <Show when=move || current_panel.get() == "console">
                        <ConsoleTab tiles=tiles set_tiles=set_tiles />
                    </Show>
                    <Show when=move || current_panel.get() == "profiler">
                        <ProfilerTab />
                    </Show>
                    <Show when=move || current_panel.get() == "agents">
                        <AgentsTab />
                    </Show>
                    <Show when=move || current_panel.get() == "images">
                        <ImagesPanel />
                    </Show>
                    <Show when=move || current_panel.get() == "docs">
                        <DocsPanel />
                    </Show>
                    // Log sub-panels under "console"
                    <Show when=move || current_panel.get() == "audit">
                        <LogPanel lines=audit_lines color="#e3b341" placeholder="No audit events." />
                    </Show>
                    <Show when=move || current_panel.get() == "timeline">
                        <LogPanel lines=timeline_lines color="#a5d6ff" placeholder="No timeline events." />
                    </Show>
                    <Show when=move || current_panel.get() == "metrics">
                        <LogPanel lines=metrics_lines color="#7ee787" placeholder="No metrics yet." />
                    </Show>
                </main>
            </div>
        </div>
    }
}

// ── AppHeader ─────────────────────────────────────────────────────────────────

#[component]
fn AppHeader(
    ws_status:    ReadSignal<WsStatus>,
    ws_label:     ReadSignal<String>,
    sidebar_open: ReadSignal<bool>,
    set_sidebar:  WriteSignal<bool>,
) -> impl IntoView {
    view! {
        <header>
            <button
                class="sidebar-toggle"
                on:click=move |_| set_sidebar.update(|b| *b = !*b)
                title="Toggle sidebar"
            >
                {move || if sidebar_open.get() { "◁" } else { "▷" }}
            </button>
            <div class="logo">
                <span class="logo-icon">"◈"</span>
                <h1>"agentOS Console"</h1>
            </div>
            <div class="ws-status">
                <div class=move || match ws_status.get() {
                    WsStatus::Connected => "ws-dot connected",
                    WsStatus::Error     => "ws-dot error",
                    _                   => "ws-dot",
                } />
                <span id="ws-label">{ws_label}</span>
            </div>
        </header>
    }
}

// ── SidebarStats ──────────────────────────────────────────────────────────────

#[component]
fn SidebarStats(
    topo_metrics: RwSignal<Vec<NodeMetrics>>,
    ws_status:    ReadSignal<WsStatus>,
) -> impl IntoView {
    let avg_cpu = move || {
        let m = topo_metrics.get();
        let active: Vec<_> = m.iter().filter(|n| n.cpu_pct > 0).collect();
        if active.is_empty() { return 0u32; }
        active.iter().map(|n| n.cpu_pct).sum::<u32>() / active.len() as u32
    };
    let active_slots = move || {
        topo_metrics.get().iter().filter(|n| n.status == NodeStatus::Ready).count()
    };
    let ws_ok = move || ws_status.get() == WsStatus::Connected;

    view! {
        <div class="sidebar-stats">
            <div class="sidebar-stat">
                <span class="stat-label">"avg cpu"</span>
                <span class="stat-value">{move || format!("{}%", avg_cpu())}</span>
            </div>
            <div class="sidebar-stat">
                <span class="stat-label">"active PDs"</span>
                <span class="stat-value">{move || active_slots().to_string()}</span>
            </div>
            <div class="sidebar-stat">
                <span class="stat-label">"serial"</span>
                <span class=move || if ws_ok() { "stat-value ok" } else { "stat-value err" }>
                    {move || if ws_ok() { "live" } else { "off" }}
                </span>
            </div>
        </div>
    }
}

// ── ConsoleTab ────────────────────────────────────────────────────────────────

#[component]
fn ConsoleTab(
    tiles:     ReadSignal<Vec<(usize, String)>>,
    set_tiles: WriteSignal<Vec<(usize, String)>>,
) -> impl IntoView {
    let (picker_open, set_picker_open) = create_signal(false);
    let (log_tab, set_log_tab) = create_signal("audit");

    let audit_lines    = create_rw_signal(Vec::<String>::new());
    let timeline_lines = create_rw_signal(Vec::<String>::new());
    let metrics_lines  = create_rw_signal(Vec::<String>::new());

    let tiles_each  = move || tiles.get();
    let open_picker = move |_| set_picker_open.set(true);

    view! {
        <div class="console-panel">
            // Terminal grid
            <div id="console-grid" class="console-grid">
                <For
                    each=tiles_each
                    key=|(slot_id, _)| *slot_id
                    children=move |(slot_id, name): (usize, String)| {
                        let st = set_tiles;
                        view! {
                            <TerminalTile
                                slot_id=slot_id
                                name=name.clone()
                                on_close=Callback::new(move |_: ()| {
                                    st.update(|v| v.retain(|(id, _)| *id != slot_id));
                                })
                            />
                        }
                    }
                />
                <button id="console-add-tile" class="console-add-tile" on:click=open_picker>
                    "＋ Add Terminal"
                </button>
            </div>

            // Log sub-tabs
            <div class="console-logs">
                <div class="log-subtabs">
                    {["audit", "timeline", "metrics"].iter().map(|&tab| {
                        view! {
                            <button
                                class=move || if log_tab.get() == tab { "log-subtab active" } else { "log-subtab" }
                                on:click=move |_| set_log_tab.set(tab)
                            >{tab}</button>
                        }
                    }).collect_view()}
                </div>
                <Show when=move || log_tab.get() == "audit">
                    <LogPanel lines=audit_lines color="#e3b341" placeholder="No audit events." />
                </Show>
                <Show when=move || log_tab.get() == "timeline">
                    <LogPanel lines=timeline_lines color="#a5d6ff" placeholder="No timeline events." />
                </Show>
                <Show when=move || log_tab.get() == "metrics">
                    <LogPanel lines=metrics_lines color="#7ee787" placeholder="No metrics yet." />
                </Show>
            </div>

            // Slot picker dialog
            <Show when=move || picker_open.get()>
                <div id="slot-picker-overlay" on:click=move |_| set_picker_open.set(false)>
                    <div class="slot-picker-dialog" on:click=|e| e.stop_propagation()>
                        <h3>"Select a PD Slot"</h3>
                        {PD_SLOTS.iter().map(|slot| {
                            let slot_id  = slot.id;
                            let display  = slot.display.to_string();
                            let display2 = display.clone();
                            view! {
                                <button
                                    class="slot-picker-btn"
                                    on:click=move |_| {
                                        set_tiles.update(|v| {
                                            if !v.iter().any(|(id, _)| *id == slot_id) {
                                                v.push((slot_id, display2.clone()));
                                            }
                                        });
                                        set_picker_open.set(false);
                                    }
                                >
                                    {format!("{} — {}", slot_id, display)}
                                </button>
                            }
                        }).collect_view()}
                        <button class="slot-picker-cancel" on:click=move |_| set_picker_open.set(false)>
                            "Cancel"
                        </button>
                    </div>
                </div>
            </Show>
        </div>
    }
}

// ── TerminalTile ──────────────────────────────────────────────────────────────

#[component]
fn TerminalTile(slot_id: usize, name: String, on_close: Callback<()>) -> impl IntoView {
    let term_ref  = create_node_ref::<leptos::html::Div>();
    let term_cell: Rc<RefCell<Option<xterm_bindings::XTerminal>>> = Rc::new(RefCell::new(None));
    let term_cell_cleanup = term_cell.clone();

    create_effect(move |initialized: Option<bool>| -> bool {
        if initialized.unwrap_or(false) { return true; }
        let container = match term_ref.get() { Some(c) => c, None => return false };

        let opts = js_sys::Object::new();
        let _ = js_sys::Reflect::set(&opts, &JsValue::from("fontSize"),    &JsValue::from_f64(12.0));
        let _ = js_sys::Reflect::set(&opts, &JsValue::from("fontFamily"),  &JsValue::from("'Berkeley Mono','Fira Code',monospace"));
        let _ = js_sys::Reflect::set(&opts, &JsValue::from("cursorBlink"), &JsValue::TRUE);
        let _ = js_sys::Reflect::set(&opts, &JsValue::from("scrollback"),  &JsValue::from_f64(2000.0));
        let theme = js_sys::Object::new();
        let _ = js_sys::Reflect::set(&theme, &JsValue::from("background"), &JsValue::from("#050607"));
        let _ = js_sys::Reflect::set(&theme, &JsValue::from("foreground"), &JsValue::from("#f7f8f8"));
        let _ = js_sys::Reflect::set(&theme, &JsValue::from("cursor"),     &JsValue::from("#7170ff"));
        let _ = js_sys::Reflect::set(&opts,  &JsValue::from("theme"),      &theme.into());

        let term = xterm_bindings::XTerminal::new(&opts.into());
        term.open(container.as_ref());

        let (tx, mut rx) = futures::channel::mpsc::unbounded::<String>();
        let tx2 = tx.clone();
        let on_data_cb = Closure::wrap(Box::new(move |data: String| {
            let _ = tx2.unbounded_send(data);
        }) as Box<dyn Fn(String)>);
        term.on_data(on_data_cb.as_ref().unchecked_ref::<JsValue>());
        on_data_cb.forget();
        *term_cell.borrow_mut() = Some(term);

        let host = web_sys::window().unwrap().location().host()
            .unwrap_or_else(|_| "localhost:8080".to_string());
        let ws_url = format!("ws://{}/terminal/{}", host, slot_id);

        use gloo_net::websocket::futures::WebSocket;
        use futures::StreamExt;

        if let Ok(ws) = WebSocket::open(&ws_url) {
            let (mut write, mut read) = ws.split();
            wasm_bindgen_futures::spawn_local(async move {
                use futures::SinkExt;
                while let Some(msg) = rx.next().await {
                    if write.send(gloo_net::websocket::Message::Text(msg)).await.is_err() { break; }
                }
            });
            let tc = term_cell.clone();
            wasm_bindgen_futures::spawn_local(async move {
                while let Some(Ok(gloo_net::websocket::Message::Text(text))) = read.next().await {
                    if let Some(t) = tc.borrow().as_ref() { t.write(&text); }
                }
            });
        } else if let Some(t) = term_cell.borrow().as_ref() {
            t.write("\r\n\x1b[31m[terminal] ws connection failed\x1b[0m\r\n");
        }
        true
    });

    on_cleanup(move || {
        if let Some(t) = term_cell_cleanup.borrow_mut().take() { t.dispose(); }
    });

    view! {
        <div class="console-tile">
            <div class="console-tile-chrome">
                <span class="console-tile-title">{format!("slot {} \u{2014} {}", slot_id, name)}</span>
                <button class="console-tile-close" on:click=move |_| on_close.call(())>"×"</button>
            </div>
            <div class="console-tile-term" node_ref=term_ref />
        </div>
    }
}

// ── LogPanel ──────────────────────────────────────────────────────────────────

#[component]
fn LogPanel(lines: RwSignal<Vec<String>>, color: &'static str, placeholder: &'static str) -> impl IntoView {
    let log_ref = create_node_ref::<leptos::html::Div>();
    create_effect(move |_| {
        let _ = lines.get();
        if let Some(el) = log_ref.get() {
            let el: &web_sys::HtmlElement = el.as_ref();
            el.set_scroll_top(el.scroll_height());
        }
    });
    view! {
        <div class="log-area" node_ref=log_ref>
            {move || {
                let current = lines.get();
                if current.is_empty() {
                    view! { <div class="empty-state">{placeholder}</div> }.into_view()
                } else {
                    current.into_iter().map(|raw| {
                        let (ts, val) = if raw.starts_with('[') {
                            if let Some(end) = raw.find("] ") {
                                (raw[1..end].to_string(), raw[end+2..].to_string())
                            } else { (String::new(), raw.clone()) }
                        } else { (String::new(), raw) };
                        view! {
                            <div class="log-line">
                                <span class="ts">{ts}</span>
                                <span class="val" style=format!("color:{}", color)>{val}</span>
                            </div>
                        }
                    }).collect_view().into_view()
                }
            }}
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
            if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/profiler/snapshot").send().await {
                if let Ok(snap) = resp.json::<ProfilerSnapshot>().await { set_snapshot.set(Some(snap)); }
            }
            set_loading.set(false);
        });
    };

    view! {
        <div class="tab-panel active">
            <div class="profiler-header">
                <button class="btn primary" on:click=fetch_snapshot>
                    {move || if loading.get() { "Loading…" } else { "Snapshot" }}
                </button>
                <span class="profiler-ts">
                    {move || snapshot.get().as_ref().map(|s| format!("ts: {}", s.ts)).unwrap_or_default()}
                </span>
            </div>
            {move || snapshot.get().map(|snap| {
                let for_table = snap.slots.clone();
                let for_flame = snap.slots.clone();
                view! {
                    <>
                        <div class="table-wrapper">
                            <table class="slot-table">
                                <thead><tr>
                                    <th>"Slot"</th><th>"Name"</th>
                                    <th>"CPU %"</th><th>"Mem KB"</th><th>"Ticks"</th>
                                </tr></thead>
                                <tbody>
                                    {for_table.into_iter().map(|s| {
                                        let cpu = s.cpu_pct;
                                        let cls = if cpu >= 90 { "cpu-bar-fill critical" }
                                                  else if cpu >= 60 { "cpu-bar-fill hot" }
                                                  else { "cpu-bar-fill" };
                                        view! {
                                            <tr>
                                                <td>{s.id}</td><td>{s.name}</td>
                                                <td>
                                                    <div class="cpu-bar">
                                                        <div class="cpu-bar-track">
                                                            <div class=cls style=format!("width:{}%", cpu.min(100)) />
                                                        </div>
                                                        {format!("{}%", cpu)}
                                                    </div>
                                                </td>
                                                <td>{s.mem_kb}</td><td>{s.ticks}</td>
                                            </tr>
                                        }
                                    }).collect_view()}
                                </tbody>
                            </table>
                        </div>
                        <span class="section-title">"Flame Graphs"</span>
                        <div class="flame-graphs-container">
                            {for_flame.into_iter().filter(|s| !s.frames.is_empty()).map(|s| view! {
                                <div class="flame-section">
                                    <div class="flame-header">
                                        <span class="slot-name">{s.name.clone()}</span>
                                        <span class="slot-meta">{format!("{}% CPU", s.cpu_pct)}</span>
                                    </div>
                                    <div class="flame-body">
                                        <FlameGraph frames=s.frames />
                                    </div>
                                </div>
                            }).collect_view()}
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
    if frames.is_empty() { return view! { <svg /> }.into_view(); }
    let max_depth   = frames.iter().map(|f| f.depth).max().unwrap_or(0);
    let total_ticks: u32 = frames.iter().map(|f| f.ticks).sum();
    let svg_h = (max_depth + 1) * (ROW_H + PAD) + PAD;
    view! {
        <svg class="flame-graph" width=WIDTH height=svg_h viewBox=format!("0 0 {} {}", WIDTH, svg_h)>
            {frames.into_iter().map(|f| {
                let bar_w = if total_ticks > 0 {
                    ((f.ticks as f64 / total_ticks as f64) * WIDTH as f64) as u32
                } else { 0 };
                let y = f.depth * (ROW_H + PAD) + PAD;
                view! {
                    <g>
                        <rect x="0" y=y width=bar_w height=ROW_H rx="2"
                              fill="var(--accent-bg)" opacity="0.8" />
                        <text x="4" y=format!("{}", y + ROW_H - 6)
                              fill="var(--text)" font-size="10" font-family="monospace">
                            {format!("{} ({}t)", f.fn_name, f.ticks)}
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

    let fetch = move || {
        set_loading.set(true);
        wasm_bindgen_futures::spawn_local(async move {
            if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/agents").send().await {
                if let Ok(list) = resp.json::<Vec<Agent>>().await { set_agents.set(list); }
            }
            set_loading.set(false);
        });
    };

    create_effect(move |_| { fetch(); });

    view! {
        <div class="tab-panel active">
            <div class="agents-toolbar">
                <button class="btn primary" on:click=move |_| fetch()>
                    {move || if loading.get() { "Loading…" } else { "↻ Refresh" }}
                </button>
            </div>
            <div id="agents-list">
                <Show
                    when=move || !agents.get().is_empty()
                    fallback=|| view! { <div class="empty-state">"No agents found."</div> }
                >
                    <For
                        each=move || agents.get()
                        key=|a: &Agent| a.id.clone()
                        children=move |agent: Agent| {
                            let card_cls  = format!("agent-card status-{}", agent.status);
                            let badge_cls = format!("agent-badge badge-{}", agent.status);
                            let meta = format!("type: {}  |  slot: {}  |  lines: {}",
                                               agent.agent_type, agent.slot, agent.lines);
                            let note = agent.note.clone();
                            view! {
                                <div class=card_cls>
                                    <div class="agent-name">{agent.name}</div>
                                    <div class="agent-actions">
                                        <span class=badge_cls>{agent.status}</span>
                                    </div>
                                    <div class="agent-meta">{meta}</div>
                                    {note.map(|n| view! { <div class="agent-note">{n}</div> })}
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
