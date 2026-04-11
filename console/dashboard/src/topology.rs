/// agentOS system topology: static PD graph + SVG rendering.
///
/// All node positions are hardcoded for a 1600×700 viewBox.
/// The same viewBox is used for both the full panel and the mini sidebar
/// (the sidebar just renders at smaller width/height with SVG scaling).

use leptos::*;

// ── Node definitions ──────────────────────────────────────────────────────────

pub struct TopoNode {
    pub id:      usize,
    pub name:    &'static str,
    pub display: &'static str,
}

pub const TOPO_NODES: &[TopoNode] = &[
    // Slots 0–15 mirror PD_SLOTS from pd-slots crate
    TopoNode { id:  0, name: "controller",  display: "controller"  },
    TopoNode { id:  1, name: "event_bus",   display: "event_bus"   },
    TopoNode { id:  2, name: "init_agent",  display: "init_agent"  },
    TopoNode { id:  3, name: "agentfs",     display: "agentfs"     },
    TopoNode { id:  4, name: "vibe_engine", display: "vibe_engine" },
    TopoNode { id:  5, name: "worker_0",    display: "worker_0"    },
    TopoNode { id:  6, name: "worker_1",    display: "worker_1"    },
    TopoNode { id:  7, name: "worker_2",    display: "worker_2"    },
    TopoNode { id:  8, name: "worker_3",    display: "worker_3"    },
    TopoNode { id:  9, name: "swap_slot_0", display: "swap_0"      },
    TopoNode { id: 10, name: "swap_slot_1", display: "swap_1"      },
    TopoNode { id: 11, name: "swap_slot_2", display: "swap_2"      },
    TopoNode { id: 12, name: "swap_slot_3", display: "swap_3"      },
    TopoNode { id: 13, name: "console_mux", display: "console_mux" },
    TopoNode { id: 14, name: "linux_vmm",   display: "linux_vmm"   },
    TopoNode { id: 15, name: "fault_hndlr", display: "fault_hndlr" },
    // Slots 16–23: kernel-space services (not in PD_SLOTS)
    TopoNode { id: 16, name: "nameserver",   display: "nameserver"   },
    TopoNode { id: 17, name: "vfs_server",   display: "vfs_server"   },
    TopoNode { id: 18, name: "spawn_server", display: "spawn_server" },
    TopoNode { id: 19, name: "net_server",   display: "net_server"   },
    TopoNode { id: 20, name: "virtio_blk",   display: "virtio_blk"   },
    TopoNode { id: 21, name: "app_manager",  display: "app_manager"  },
    TopoNode { id: 22, name: "http_svc",     display: "http_svc"     },
    TopoNode { id: 23, name: "capstore",     display: "capstore"     },
];

pub const NUM_NODES: usize = 24;

// ── Edge definitions ──────────────────────────────────────────────────────────

#[derive(Clone, Copy, PartialEq)]
pub enum EdgeKind {
    Ipc,       // solid white arrow
    SharedMem, // dashed yellow
    EventBus,  // purple
    Network,   // green dotted
}

pub struct TopoEdge {
    pub from:    usize,
    pub to:      usize,
    pub channel: u32,
    pub kind:    EdgeKind,
}

pub const TOPO_EDGES: &[TopoEdge] = &[
    // controller → services
    TopoEdge { from:  0, to: 16, channel: 18,  kind: EdgeKind::Ipc       }, //  0 nameserver
    TopoEdge { from:  0, to: 15, channel: 200, kind: EdgeKind::Ipc       }, //  1 fault_hndlr
    TopoEdge { from:  0, to:  1, channel:  0,  kind: EdgeKind::Ipc       }, //  2 event_bus
    TopoEdge { from:  0, to:  2, channel:  1,  kind: EdgeKind::Ipc       }, //  3 init_agent
    TopoEdge { from:  0, to: 23, channel: 50,  kind: EdgeKind::Ipc       }, //  4 capstore
    TopoEdge { from:  0, to:  3, channel:  5,  kind: EdgeKind::Ipc       }, //  5 agentfs
    TopoEdge { from:  0, to:  4, channel: 40,  kind: EdgeKind::Ipc       }, //  6 vibe_engine
    TopoEdge { from:  0, to: 17, channel: 19,  kind: EdgeKind::Ipc       }, //  7 vfs_server
    TopoEdge { from:  0, to: 18, channel: 20,  kind: EdgeKind::Ipc       }, //  8 spawn_server
    TopoEdge { from:  0, to: 19, channel: 21,  kind: EdgeKind::Ipc       }, //  9 net_server
    TopoEdge { from:  0, to: 20, channel: 22,  kind: EdgeKind::Ipc       }, // 10 virtio_blk
    TopoEdge { from:  0, to: 21, channel: 23,  kind: EdgeKind::Ipc       }, // 11 app_manager
    TopoEdge { from:  0, to: 22, channel: 24,  kind: EdgeKind::Ipc       }, // 12 http_svc
    TopoEdge { from:  0, to: 13, channel: 55,  kind: EdgeKind::Ipc       }, // 13 console_mux
    TopoEdge { from:  0, to: 14, channel: 60,  kind: EdgeKind::Network   }, // 14 linux_vmm (VMM)
    // app_manager → downstream services
    TopoEdge { from: 21, to: 18, channel: 20,  kind: EdgeKind::Ipc       }, // 15 → spawn_server
    TopoEdge { from: 21, to: 19, channel: 21,  kind: EdgeKind::Ipc       }, // 16 → net_server
    TopoEdge { from: 21, to: 22, channel: 24,  kind: EdgeKind::Ipc       }, // 17 → http_svc
    // shared memory paths
    TopoEdge { from: 18, to: 17, channel:  0,  kind: EdgeKind::SharedMem }, // 18 spawn ↔ vfs
    TopoEdge { from: 19, to: 20, channel:  0,  kind: EdgeKind::SharedMem }, // 19 net ↔ blk
    TopoEdge { from:  3, to: 17, channel:  0,  kind: EdgeKind::SharedMem }, // 20 agentfs ↔ vfs
    // event_bus → workers
    TopoEdge { from:  1, to:  5, channel: 30,  kind: EdgeKind::EventBus  }, // 21 worker_0
    TopoEdge { from:  1, to:  6, channel: 31,  kind: EdgeKind::EventBus  }, // 22 worker_1
    TopoEdge { from:  1, to:  7, channel: 32,  kind: EdgeKind::EventBus  }, // 23 worker_2
    TopoEdge { from:  1, to:  8, channel: 33,  kind: EdgeKind::EventBus  }, // 24 worker_3
    // vibe_engine → swap_slots
    TopoEdge { from:  4, to:  9, channel: 30,  kind: EdgeKind::Ipc       }, // 25 swap_0
    TopoEdge { from:  4, to: 10, channel: 31,  kind: EdgeKind::Ipc       }, // 26 swap_1
    TopoEdge { from:  4, to: 11, channel: 32,  kind: EdgeKind::Ipc       }, // 27 swap_2
    TopoEdge { from:  4, to: 12, channel: 33,  kind: EdgeKind::Ipc       }, // 28 swap_3
    // init_agent → event_bus (subscription)
    TopoEdge { from:  2, to:  1, channel:  0,  kind: EdgeKind::EventBus  }, // 29
];

pub const NUM_EDGES: usize = 30;

// ── Node positions (x, y) = top-left corner, full 1600×700 viewBox ──────────
// Node size: 128 wide × 38 tall.
pub const NODE_POS: [(f32, f32); NUM_NODES] = [
    (736.0,  20.0),  //  0 controller  — layer 0
    (588.0, 180.0),  //  1 event_bus   — layer 2
    (736.0, 180.0),  //  2 init_agent  — layer 2
    (514.0, 270.0),  //  3 agentfs     — layer 3
    (736.0, 270.0),  //  4 vibe_engine — layer 3
    (440.0, 530.0),  //  5 worker_0    — layer 6
    (588.0, 530.0),  //  6 worker_1    — layer 6
    (736.0, 530.0),  //  7 worker_2    — layer 6
    (884.0, 530.0),  //  8 worker_3    — layer 6
    (440.0, 620.0),  //  9 swap_slot_0 — layer 7
    (588.0, 620.0),  // 10 swap_slot_1 — layer 7
    (736.0, 620.0),  // 11 swap_slot_2 — layer 7
    (884.0, 620.0),  // 12 swap_slot_3 — layer 7
    (662.0, 440.0),  // 13 console_mux — layer 5
    (884.0, 440.0),  // 14 linux_vmm   — layer 5
    (884.0,  90.0),  // 15 fault_hndlr — layer 1
    (588.0,  90.0),  // 16 nameserver  — layer 1
    (884.0, 270.0),  // 17 vfs_server  — layer 3
    (440.0, 355.0),  // 18 spawn_server— layer 4
    (588.0, 355.0),  // 19 net_server  — layer 4
    (736.0, 355.0),  // 20 virtio_blk  — layer 4
    (958.0, 355.0),  // 21 app_manager — layer 4
    (514.0, 440.0),  // 22 http_svc    — layer 5
    (884.0, 180.0),  // 23 capstore    — layer 2
];

// ── Metrics ───────────────────────────────────────────────────────────────────

#[derive(Clone, Default)]
pub struct NodeMetrics {
    pub cpu_pct: u32,
    pub mem_kb:  u64,
    pub status:  NodeStatus,
}

#[derive(Clone, Copy, PartialEq, Default)]
pub enum NodeStatus {
    #[default]
    Unknown,
    Ready,
    Degraded,
    Offline,
}

impl NodeStatus {
    pub fn css_class(self) -> &'static str {
        match self {
            NodeStatus::Unknown  => "status-unknown",
            NodeStatus::Ready    => "status-ready",
            NodeStatus::Degraded => "status-degraded",
            NodeStatus::Offline  => "status-offline",
        }
    }
}

// ── SVG topology rendering ────────────────────────────────────────────────────

/// Full topology panel — large SVG with live metric overlays.
#[component]
pub fn TopologyPanel(
    metrics:      RwSignal<Vec<NodeMetrics>>,
    active_edges: RwSignal<Vec<bool>>,
    set_panel:    WriteSignal<String>,
    set_tiles:    WriteSignal<Vec<(usize, String)>>,
) -> impl IntoView {
    view! {
        <div class="topo-panel">
            <div class="topo-legend">
                <span class="legend-item ipc">"IPC"</span>
                <span class="legend-item shmem">"Shared Mem"</span>
                <span class="legend-item eventbus">"EventBus"</span>
                <span class="legend-item network">"Network"</span>
                <span class="legend-hint">"Click a node to open its terminal"</span>
            </div>
            <div class="topo-scroll">
                {topo_svg(metrics, active_edges, set_panel, set_tiles, false)}
            </div>
            <div class="topo-legend">
                <span class="legend-item"><span class="dot status-ready"/>"Ready"</span>
                <span class="legend-item"><span class="dot status-degraded"/>"Degraded"</span>
                <span class="legend-item"><span class="dot status-offline"/>"Offline"</span>
                <span class="legend-item"><span class="dot status-unknown"/>"Unknown"</span>
            </div>
        </div>
    }
}

/// Mini sidebar topology — same SVG scaled to fit 220px.
#[component]
pub fn TopologyMini(
    metrics:      RwSignal<Vec<NodeMetrics>>,
    active_edges: RwSignal<Vec<bool>>,
    set_panel:    WriteSignal<String>,
    set_tiles:    WriteSignal<Vec<(usize, String)>>,
) -> impl IntoView {
    view! {
        <div class="topo-mini">
            {topo_svg(metrics, active_edges, set_panel, set_tiles, true)}
        </div>
    }
}

fn topo_svg(
    metrics:      RwSignal<Vec<NodeMetrics>>,
    active_edges: RwSignal<Vec<bool>>,
    set_panel:    WriteSignal<String>,
    set_tiles:    WriteSignal<Vec<(usize, String)>>,
    mini:         bool,
) -> impl IntoView {
    let (w, h) = if mini { ("100%", "440") } else { ("100%", "700") };

    view! {
        <svg
            width=w
            height=h
            viewBox="0 0 1100 680"
            preserveAspectRatio="xMidYMid meet"
            class="topo-svg"
            xmlns="http://www.w3.org/2000/svg"
        >
            <defs>
                <marker id="arr-ipc" markerWidth="7" markerHeight="7" refX="6" refY="3.5" orient="auto">
                    <path d="M0,0 L0,7 L7,3.5 z" fill="rgba(255,255,255,0.35)" />
                </marker>
                <marker id="arr-ipc-active" markerWidth="7" markerHeight="7" refX="6" refY="3.5" orient="auto">
                    <path d="M0,0 L0,7 L7,3.5 z" fill="#7170ff" />
                </marker>
                <marker id="arr-eb" markerWidth="7" markerHeight="7" refX="6" refY="3.5" orient="auto">
                    <path d="M0,0 L0,7 L7,3.5 z" fill="rgba(113,112,255,0.5)" />
                </marker>
                <marker id="arr-net" markerWidth="7" markerHeight="7" refX="6" refY="3.5" orient="auto">
                    <path d="M0,0 L0,7 L7,3.5 z" fill="rgba(16,185,129,0.5)" />
                </marker>
            </defs>

            // ── Edges ─────────────────────────────────────────────────────────
            {TOPO_EDGES.iter().enumerate().map(|(i, edge)| {
                let (sx, sy) = NODE_POS[edge.from];
                let (tx, ty) = NODE_POS[edge.to];
                // line from bottom-center of source to top-center of target
                let x1 = sx + 64.0;
                let y1 = sy + 38.0;
                let x2 = tx + 64.0;
                let y2 = ty;
                // control point for slight curve
                let cx = (x1 + x2) / 2.0;
                let cy = (y1 + y2) / 2.0;
                let d = format!("M{:.0},{:.0} Q{:.0},{:.0} {:.0},{:.0}", x1, y1, cx, cy, x2, y2);
                let kind = edge.kind;
                let is_active = move || active_edges.get().get(i).copied().unwrap_or(false);
                let (stroke, dasharray, marker) = match kind {
                    EdgeKind::Ipc      => ("rgba(255,255,255,0.25)", "none",    "url(#arr-ipc)"),
                    EdgeKind::SharedMem=> ("rgba(232,169,79,0.4)",   "6,3",     "none"),
                    EdgeKind::EventBus => ("rgba(113,112,255,0.45)", "none",    "url(#arr-eb)"),
                    EdgeKind::Network  => ("rgba(16,185,129,0.45)",  "4,4",     "url(#arr-net)"),
                };
                view! {
                    <path
                        d=d
                        fill="none"
                        stroke=move || if is_active() { "#7170ff" } else { stroke }
                        stroke-width=move || if is_active() { "2.5" } else { "1.5" }
                        stroke-dasharray=dasharray
                        marker-end=move || if is_active() { "url(#arr-ipc-active)" } else { marker }
                        class=move || if is_active() { "topo-edge active" } else { "topo-edge" }
                    />
                }
            }).collect_view()}

            // ── Nodes ─────────────────────────────────────────────────────────
            {TOPO_NODES.iter().map(|node| {
                let id = node.id;
                let display = node.display;
                let m = move || metrics.get().get(id).cloned().unwrap_or_default();
                let status_cls = move || format!("topo-node {}", m().status.css_class());
                let cpu_w = move || (m().cpu_pct.min(100) as f32 / 100.0 * 106.0).max(0.0);
                let (nx, ny) = NODE_POS[id];

                let sp = set_panel;
                let st = set_tiles;
                let slot_name = node.name;

                let desc = node_description(node.name);
                view! {
                    <g
                        class=status_cls
                        transform=format!("translate({:.0},{:.0})", nx, ny)
                        style="cursor:pointer"
                        on:click=move |_| {
                            sp.set("console".to_string());
                            st.update(|v| {
                                if !v.iter().any(|(sid, _)| *sid == id) {
                                    v.push((id, slot_name.to_string()));
                                }
                            });
                        }
                    >
                        <title>{desc}</title>
                        <rect width="128" height="38" rx="5" class="topo-node-bg" />
                        // CPU bar
                        <rect
                            x="11" y="28"
                            width=move || format!("{:.0}", cpu_w())
                            height="5"
                            rx="2"
                            class="topo-cpu-fill"
                        />
                        <rect x="11" y="28" width="106" height="5" rx="2" class="topo-cpu-track" />
                        // Name
                        <text x="64" y="14" text-anchor="middle" class="topo-label">{display}</text>
                        // Metrics
                        <text x="64" y="26" text-anchor="middle" class="topo-meta">
                            {move || {
                                let met = m();
                                if met.cpu_pct > 0 || met.mem_kb > 0 {
                                    format!("{}%  {}K", met.cpu_pct, met.mem_kb)
                                } else {
                                    String::new()
                                }
                            }}
                        </text>
                        // Status dot (top-right corner)
                        <circle cx="120" cy="9" r="5" class="topo-status-dot" />
                    </g>
                }
            }).collect_view()}
        </svg>
    }
}

/// Return a plain-English description for a topology node by name.
fn node_description(name: &str) -> &'static str {
    match name {
        "controller"  => "Root protection domain — coordinates all other PDs",
        "event_bus"   => "Event Bus — routes publish/subscribe events between PDs",
        "init_agent"  => "Init Agent — bootstraps the system and starts services",
        "agentfs"     => "AgentFS — virtual filesystem for agent data",
        "vibe_engine" => "Vibe Engine — hot-swaps WASM agents into swap slots",
        "worker_0"    => "WASM Worker 0 — executes WASM agents",
        "worker_1"    => "WASM Worker 1 — executes WASM agents",
        "worker_2"    => "WASM Worker 2 — executes WASM agents",
        "worker_3"    => "WASM Worker 3 — executes WASM agents",
        "swap_slot_0" => "WASM Swap Slot 0 — hot-swap target for agent upgrades",
        "swap_slot_1" => "WASM Swap Slot 1 — hot-swap target for agent upgrades",
        "swap_slot_2" => "WASM Swap Slot 2 — hot-swap target for agent upgrades",
        "swap_slot_3" => "WASM Swap Slot 3 — hot-swap target for agent upgrades",
        "console_mux" => "Console Mux — multiplexes serial console streams",
        "linux_vmm"   => "Linux VM — runs Buildroot Linux guest",
        "fault_hndlr" => "Fault Handler — catches and reports PD faults",
        _             => "Protection Domain — isolated security boundary",
    }
}

/// Find all edge indices incident to a slot id (for animation).
pub fn edges_for_slot(slot: usize) -> Vec<usize> {
    TOPO_EDGES.iter().enumerate()
        .filter(|(_, e)| e.from == slot || e.to == slot)
        .map(|(i, _)| i)
        .collect()
}
