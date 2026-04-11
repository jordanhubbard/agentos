//! VMware Fusion-style VM management panel for agentOS dashboard.
//!
//! Communicates with the backend via:
//!   GET  /api/agentos/vms                  → list all VMs
//!   POST /api/agentos/vms                  → create a new VM
//!   POST /api/agentos/vms/:slot_id/:action → perform action on a VM

use leptos::*;
use serde::{Deserialize, Serialize};

// ── VM state ──────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub enum VmState {
    Free,
    Booting,
    Running,
    Paused,
    Halted,
    Error,
}

impl VmState {
    pub fn label(&self) -> &'static str {
        match self {
            VmState::Free    => "Free",
            VmState::Booting => "Booting",
            VmState::Running => "Running",
            VmState::Paused  => "Paused",
            VmState::Halted  => "Halted",
            VmState::Error   => "Error",
        }
    }

    pub fn dot_class(&self) -> &'static str {
        match self {
            VmState::Running                  => "vm-dot running",
            VmState::Paused                   => "vm-dot paused",
            VmState::Booting                  => "vm-dot booting",
            VmState::Halted | VmState::Free   => "vm-dot stopped",
            VmState::Error                    => "vm-dot error",
        }
    }
}

// ── VmInfo ────────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct VmInfo {
    pub slot_id: u8,
    pub label:   String,
    pub state:   VmState,
    pub ram_mb:  u32,
}

// ── RAM label helper ──────────────────────────────────────────────────────────

fn ram_label(mb: u32) -> String {
    if mb >= 1024 && mb % 1024 == 0 {
        format!("{}GB", mb / 1024)
    } else {
        format!("{}MB", mb)
    }
}

// ── VmPanel component ─────────────────────────────────────────────────────────

#[component]
pub fn VmPanel(set_panel: WriteSignal<String>) -> impl IntoView {
    let (vms,          set_vms)          = create_signal(Vec::<VmInfo>::new());
    let (selected,     set_selected)     = create_signal(Option::<u8>::None);
    let (show_create,  set_show_create)  = create_signal(false);
    let (new_vm_name,  set_new_vm_name)  = create_signal(String::new());
    let (new_vm_ram,   set_new_vm_ram)   = create_signal(512u32);
    let (create_error, set_create_error) = create_signal(Option::<String>::None);

    // ── Fetch helper ──────────────────────────────────────────────────────────

    let fetch_vms = move || {
        let sv = set_vms;
        wasm_bindgen_futures::spawn_local(async move {
            if let Ok(resp) = gloo_net::http::Request::get("/api/agentos/vms")
                .send().await
            {
                if let Ok(list) = resp.json::<Vec<VmInfo>>().await {
                    sv.set(list);
                }
            }
        });
    };

    // Fetch on mount
    create_effect(move |_| { fetch_vms(); });

    // Poll every 2 seconds
    {
        let interval = gloo_timers::callback::Interval::new(2_000, move || {
            fetch_vms();
        });
        interval.forget();
    }

    // ── Action helper ─────────────────────────────────────────────────────────

    let do_action = move |slot_id: u8, action: &'static str| {
        let fv = move || fetch_vms();
        wasm_bindgen_futures::spawn_local(async move {
            let url = format!("/api/agentos/vms/{}/{}", slot_id, action);
            let _ = gloo_net::http::Request::post(&url).send().await;
            fv();
        });
    };

    // ── Create VM handler ─────────────────────────────────────────────────────

    let on_create = move |_| {
        let name = new_vm_name.get_untracked();
        let ram  = new_vm_ram.get_untracked();
        if name.trim().is_empty() {
            set_create_error.set(Some("Name is required.".to_string()));
            return;
        }
        set_create_error.set(None);
        let ssc = set_show_create;
        let snm = set_new_vm_name;
        wasm_bindgen_futures::spawn_local(async move {
            let body = serde_json::json!({ "label": name.trim(), "ram_mb": ram });
            let _ = gloo_net::http::Request::post("/api/agentos/vms")
                .header("Content-Type", "application/json")
                .body(body.to_string())
                .expect("body")
                .send().await;
            ssc.set(false);
            snm.set(String::new());
            fetch_vms();
        });
    };

    let on_cancel = move |_| {
        set_show_create.set(false);
        set_new_vm_name.set(String::new());
        set_create_error.set(None);
    };

    // ── View ──────────────────────────────────────────────────────────────────

    view! {
        <div class="vm-panel">
            // Header
            <div class="vm-panel-header">
                <span class="vm-panel-title">"Virtual Machines"</span>
                <button
                    class="btn-primary"
                    on:click=move |_| set_show_create.set(true)
                >
                    "+ New VM"
                </button>
            </div>

            // VM list
            <div class="vm-list">
                {move || {
                    let list = vms.get();
                    if list.is_empty() {
                        view! {
                            <div class="vm-empty">
                                "No VMs running. Click + New VM to create one."
                            </div>
                        }.into_view()
                    } else {
                        list.into_iter().map(|vm| {
                            let slot_id   = vm.slot_id;
                            let dot_class = vm.state.dot_class().to_string();
                            let state_lbl = vm.state.label().to_string();
                            let ram_str   = ram_label(vm.ram_mb);
                            let label     = vm.label.clone();
                            let is_sel    = move || selected.get() == Some(slot_id);
                            let vm_state  = vm.state.clone();

                            view! {
                                <div
                                    class=move || if is_sel() { "vm-row selected" } else { "vm-row" }
                                    on:click=move |_| set_selected.set(Some(slot_id))
                                >
                                    <span class=dot_class.clone() />
                                    <span class="vm-label">{label.clone()}</span>
                                    <span class="vm-state-badge">{state_lbl.clone()}</span>
                                    <span class="vm-ram">{ram_str.clone()}</span>
                                    <div class="vm-actions">
                                        {match vm_state {
                                            VmState::Halted => view! {
                                                <button on:click=move |_| do_action(slot_id, "start")>
                                                    "Start"
                                                </button>
                                            }.into_view(),
                                            VmState::Running => view! {
                                                <>
                                                    <button on:click=move |_| do_action(slot_id, "stop")>
                                                        "Stop"
                                                    </button>
                                                    <button on:click=move |_| do_action(slot_id, "pause")>
                                                        "Pause"
                                                    </button>
                                                    <button on:click=move |_| do_action(slot_id, "snapshot")>
                                                        "Snapshot"
                                                    </button>
                                                    <button on:click={
                                                        let sp = set_panel;
                                                        move |_| sp.set("console".to_string())
                                                    }>
                                                        "Console"
                                                    </button>
                                                </>
                                            }.into_view(),
                                            VmState::Paused => view! {
                                                <>
                                                    <button on:click=move |_| do_action(slot_id, "resume")>
                                                        "Resume"
                                                    </button>
                                                    <button on:click=move |_| do_action(slot_id, "stop")>
                                                        "Stop"
                                                    </button>
                                                </>
                                            }.into_view(),
                                            VmState::Booting => view! {
                                                <button disabled=true>"Booting…"</button>
                                            }.into_view(),
                                            _ => view! { <></> }.into_view(),
                                        }}
                                    </div>
                                </div>
                            }
                        }).collect_view()
                    }
                }}
            </div>

            // Create modal
            <Show when=move || show_create.get()>
                <div class="vm-create-modal">
                    <h3>"New Virtual Machine"</h3>
                    <div class="vm-create-form">
                        <label>
                            "Name"
                            <input
                                type="text"
                                placeholder="e.g. dev-vm-1"
                                prop:value=move || new_vm_name.get()
                                on:input=move |ev| {
                                    set_new_vm_name.set(
                                        event_target_value(&ev)
                                    );
                                }
                            />
                        </label>
                        <label>
                            "RAM"
                            <select
                                on:change=move |ev| {
                                    if let Ok(mb) = event_target_value(&ev).parse::<u32>() {
                                        set_new_vm_ram.set(mb);
                                    }
                                }
                            >
                                <option value="256"  selected=move || new_vm_ram.get() == 256>"256 MB"</option>
                                <option value="512"  selected=move || new_vm_ram.get() == 512>"512 MB"</option>
                                <option value="1024" selected=move || new_vm_ram.get() == 1024>"1 GB"</option>
                                <option value="2048" selected=move || new_vm_ram.get() == 2048>"2 GB"</option>
                                <option value="4096" selected=move || new_vm_ram.get() == 4096>"4 GB"</option>
                            </select>
                        </label>
                        {move || create_error.get().map(|e| view! {
                            <span style="color:#f44336;font-size:0.8rem;">{e}</span>
                        })}
                        <div class="vm-create-actions">
                            <button class="btn-cancel" on:click=on_cancel>"Cancel"</button>
                            <button class="btn-primary" on:click=on_create>"Create"</button>
                        </div>
                    </div>
                </div>
            </Show>
        </div>
    }
}
