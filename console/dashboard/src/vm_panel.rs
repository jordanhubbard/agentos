//! VM management panel for agentOS dashboard.
//!
//! Communicates with the backend via:
//!   GET  /api/agentos/vms                            → list all VMs
//!   GET  /api/agentos/vms/:id/devices                → list devices for a VM
//!   POST /api/agentos/vms/:id/devices                → attach a new device
//!   DEL  /api/agentos/vms/:id/devices/:dev_id        → remove a device

use leptos::*;
use serde::{Deserialize, Serialize};

// ── Device info ───────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
pub struct VmDevice {
    pub id:    String,
    #[serde(rename = "type")]
    pub kind:  String,
    pub label: String,
    pub path:  Option<String>,
    pub port:  Option<u16>,
    pub live:  bool,
}

// ── VM info ───────────────────────────────────────────────────────────────────

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct VmInfo {
    pub id:           String,
    pub name:         String,
    pub state:        String,   // "running" | "stopped" | "preparing" | "error"
    pub ram_mb:       u32,
    pub ssh_port:     u16,
    pub ssh_user:     String,
    pub ssh_password: String,
    pub ssh_note:     String,
    pub devices:      Vec<VmDevice>,
}

// ── VmPanel component ─────────────────────────────────────────────────────────

#[component]
pub fn VmPanel(set_panel: WriteSignal<String>) -> impl IntoView {
    let (vms,           set_vms)           = create_signal(Vec::<VmInfo>::new());
    let (selected,      set_selected)      = create_signal(Option::<String>::None);
    // Device manager state
    let (show_dev_mgr,  set_show_dev_mgr)  = create_signal(false);
    let (dev_mgr_vm,    set_dev_mgr_vm)    = create_signal(String::new());
    let (new_dev_type,  set_new_dev_type)  = create_signal("disk".to_string());
    let (new_dev_label, set_new_dev_label) = create_signal(String::new());
    let (new_dev_size,  set_new_dev_size)  = create_signal(10u32);
    let (dev_error,     set_dev_error)     = create_signal(Option::<String>::None);

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

    // Poll every 3 seconds
    {
        let interval = gloo_timers::callback::Interval::new(3_000, move || {
            fetch_vms();
        });
        interval.forget();
    }

    // ── Device manager handlers ───────────────────────────────────────────────

    let open_dev_mgr = move |vm_id: String| {
        set_dev_mgr_vm.set(vm_id);
        set_show_dev_mgr.set(true);
        set_dev_error.set(None);
    };

    let close_dev_mgr = move |_| {
        set_show_dev_mgr.set(false);
        set_dev_error.set(None);
    };

    let add_device = move |_| {
        let vm_id = dev_mgr_vm.get_untracked();
        let kind  = new_dev_type.get_untracked();
        let label = new_dev_label.get_untracked();
        let size  = new_dev_size.get_untracked();
        let sdm   = set_show_dev_mgr;
        let sde   = set_dev_error;
        wasm_bindgen_futures::spawn_local(async move {
            let body = if kind == "disk" {
                serde_json::json!({
                    "type": "disk",
                    "label": if label.trim().is_empty() { format!("Disk {}GB", size) } else { label },
                    "size_gb": size,
                })
            } else {
                serde_json::json!({
                    "type": "nic",
                    "label": if label.trim().is_empty() { "Extra NIC".to_string() } else { label },
                })
            };
            let url  = format!("/api/agentos/vms/{}/devices", vm_id);
            let resp = gloo_net::http::Request::post(&url)
                .header("Content-Type", "application/json")
                .body(body.to_string())
                .expect("body")
                .send().await;
            match resp {
                Ok(r) if r.status() == 201 => {
                    sdm.set(false);
                    fetch_vms();
                }
                Ok(r) => {
                    let txt = r.text().await.unwrap_or_default();
                    sde.set(Some(format!("Failed: {}", txt)));
                }
                Err(e) => {
                    sde.set(Some(format!("Request error: {}", e)));
                }
            }
        });
    };

    let remove_device = move |vm_id: String, dev_id: String| {
        wasm_bindgen_futures::spawn_local(async move {
            let url = format!("/api/agentos/vms/{}/devices/{}", vm_id, dev_id);
            let _ = gloo_net::http::Request::delete(&url).send().await;
            fetch_vms();
        });
    };

    // ── State color helper ────────────────────────────────────────────────────

    let state_dot = |state: &str| -> &'static str {
        match state {
            "running"   => "vm-dot running",
            "preparing" => "vm-dot booting",
            "error"     => "vm-dot error",
            _           => "vm-dot stopped",
        }
    };

    let state_label = |state: &str| -> &'static str {
        match state {
            "running"   => "Running",
            "preparing" => "Preparing",
            "error"     => "Error",
            _           => "Stopped",
        }
    };

    // ── View ──────────────────────────────────────────────────────────────────

    view! {
        <div class="vm-panel">
            // Header
            <div class="vm-panel-header">
                <span class="vm-panel-title">"Virtual Machines"</span>
            </div>

            // VM list
            <div class="vm-list">
                {move || {
                    let list = vms.get();
                    if list.is_empty() {
                        view! {
                            <div class="vm-empty">
                                "Loading VMs…"
                            </div>
                        }.into_view()
                    } else {
                        list.into_iter().map(|vm| {
                            let vm_id      = vm.id.clone();
                            let vm_id2     = vm.id.clone();
                            let vm_id3     = vm.id.clone();
                            let dot_class  = state_dot(&vm.state).to_string();
                            let state_lbl  = state_label(&vm.state).to_string();
                            let name       = vm.name.clone();
                            let ssh_note   = vm.ssh_note.clone();
                            let is_running = vm.state == "running";
                            let is_sel     = {
                                let id = vm.id.clone();
                                move || selected.get().as_deref() == Some(&id)
                            };
                            let device_count = vm.devices.len();
                            let sp = set_panel;

                            view! {
                                <div
                                    class=move || if is_sel() { "vm-row selected" } else { "vm-row" }
                                    on:click={
                                        let id = vm_id3.clone();
                                        move |_| set_selected.set(Some(id.clone()))
                                    }
                                >
                                    <span class=dot_class.clone() />
                                    <div class="vm-info">
                                        <span class="vm-label">{name.clone()}</span>
                                        <span class="vm-state-badge">{state_lbl.clone()}</span>
                                        // SSH connection info
                                        {if is_running {
                                            view! {
                                                <span class="vm-ssh-note">{ssh_note.clone()}</span>
                                            }.into_view()
                                        } else {
                                            view! { <></> }.into_view()
                                        }}
                                    </div>
                                    <div class="vm-actions">
                                        // Device manager button
                                        <button
                                            class="btn-devices"
                                            title=format!("{} device(s) attached", device_count)
                                            on:click={
                                                let id = vm_id.clone();
                                                move |ev| {
                                                    ev.stop_propagation();
                                                    open_dev_mgr(id.clone());
                                                }
                                            }
                                        >
                                            {format!("Devices ({})", device_count)}
                                        </button>
                                        {if is_running {
                                            view! {
                                                <>
                                                    <button on:click={
                                                        move |ev| {
                                                            ev.stop_propagation();
                                                            sp.set("console".to_string());
                                                        }
                                                    }>"Console"</button>
                                                </>
                                            }.into_view()
                                        } else {
                                            // Spawn button for stopped VMs
                                            view! {
                                                <button on:click={
                                                    let id = vm_id2.clone();
                                                    move |ev| {
                                                        ev.stop_propagation();
                                                        let agent_type = match id.as_str() {
                                                            "freebsd" => "freebsd_vm",
                                                            "ubuntu"  => "linux_vm",
                                                            other     => other,
                                                        }.to_string();
                                                        wasm_bindgen_futures::spawn_local(async move {
                                                            let body = serde_json::json!({
                                                                "type": agent_type
                                                            });
                                                            let _ = gloo_net::http::Request::post(
                                                                "/api/agentos/agents/spawn"
                                                            )
                                                            .header("Content-Type", "application/json")
                                                            .body(body.to_string())
                                                            .expect("body")
                                                            .send().await;
                                                            fetch_vms();
                                                        });
                                                    }
                                                }>"Start"</button>
                                            }.into_view()
                                        }}
                                    </div>
                                </div>
                            }
                        }).collect_view()
                    }
                }}
            </div>

            // ── Device Manager Modal ──────────────────────────────────────────
            <Show when=move || show_dev_mgr.get()>
                <div class="vm-modal-backdrop">
                    <div class="vm-device-modal">
                        <div class="vm-modal-header">
                            <h3>"Device Manager — "
                                {move || dev_mgr_vm.get()}
                            </h3>
                            <button class="btn-close" on:click=close_dev_mgr>"✕"</button>
                        </div>

                        // Existing devices
                        <div class="device-list">
                            {move || {
                                let vm_id = dev_mgr_vm.get();
                                let list  = vms.get();
                                let vm = list.iter().find(|v| v.id == vm_id).cloned();
                                match vm {
                                    None => view! { <div>"VM not found"</div> }.into_view(),
                                    Some(vm) => vm.devices.into_iter().map(|dev| {
                                        let dev_id  = dev.id.clone();
                                        let vm_id2  = vm.id.clone();
                                        let is_boot = dev_id == "boot" || dev_id == "net0";
                                        let detail  = match dev.kind.as_str() {
                                            "disk" => dev.path.as_deref().unwrap_or("").to_string(),
                                            "nic"  => format!("port {}", dev.port.unwrap_or(0)),
                                            other  => other.to_string(),
                                        };
                                        let live_badge = if dev.live { " ●" } else { " ○" };
                                        view! {
                                            <div class="device-row">
                                                <span class="device-type-badge">{dev.kind.clone()}</span>
                                                <span class="device-label">{dev.label.clone()}</span>
                                                <span class="device-detail">{detail}</span>
                                                <span class="device-live" title=if dev.live { "live" } else { "queued" }>
                                                    {live_badge}
                                                </span>
                                                {if !is_boot {
                                                    let dv  = dev_id.clone();
                                                    let vid = vm_id2.clone();
                                                    let rd  = remove_device.clone();
                                                    view! {
                                                        <button
                                                            class="btn-remove"
                                                            on:click=move |_| rd(vid.clone(), dv.clone())
                                                        >"Remove"</button>
                                                    }.into_view()
                                                } else {
                                                    view! { <></> }.into_view()
                                                }}
                                            </div>
                                        }
                                    }).collect_view(),
                                }
                            }}
                        </div>

                        // Add new device form
                        <div class="device-add-form">
                            <h4>"Add Device"</h4>
                            <div class="device-form-row">
                                <label>
                                    "Type"
                                    <select on:change=move |ev| set_new_dev_type.set(event_target_value(&ev))>
                                        <option value="disk" selected=move || new_dev_type.get() == "disk">"Virtual Disk"</option>
                                        <option value="nic"  selected=move || new_dev_type.get() == "nic">"Network Interface"</option>
                                    </select>
                                </label>
                                <label>
                                    "Label (optional)"
                                    <input
                                        type="text"
                                        placeholder="e.g. Data disk"
                                        prop:value=move || new_dev_label.get()
                                        on:input=move |ev| set_new_dev_label.set(event_target_value(&ev))
                                    />
                                </label>
                                {move || if new_dev_type.get() == "disk" {
                                    view! {
                                        <label>
                                            "Size"
                                            <select on:change=move |ev| {
                                                if let Ok(v) = event_target_value(&ev).parse::<u32>() {
                                                    set_new_dev_size.set(v);
                                                }
                                            }>
                                                <option value="10"  selected=move || new_dev_size.get() == 10>"10 GB"</option>
                                                <option value="20"  selected=move || new_dev_size.get() == 20>"20 GB"</option>
                                                <option value="40"  selected=move || new_dev_size.get() == 40>"40 GB"</option>
                                                <option value="80"  selected=move || new_dev_size.get() == 80>"80 GB"</option>
                                                <option value="100" selected=move || new_dev_size.get() == 100>"100 GB"</option>
                                            </select>
                                        </label>
                                    }.into_view()
                                } else {
                                    view! { <></> }.into_view()
                                }}
                            </div>
                            {move || dev_error.get().map(|e| view! {
                                <span class="error-msg">{e}</span>
                            })}
                            <div class="device-form-actions">
                                <button class="btn-cancel" on:click=close_dev_mgr>"Cancel"</button>
                                <button class="btn-primary" on:click=add_device>"Add Device"</button>
                            </div>
                        </div>
                    </div>
                </div>
            </Show>
        </div>
    }
}
