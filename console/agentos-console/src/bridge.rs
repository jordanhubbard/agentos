//! Stub agentOS bridge HTTP server (port 8790).
//!
//! Implements all /api/agentos/* stub routes backed by the SerialCache.
//! Runs as a separate axum Router on a separate TcpListener, but in the
//! same process.

use std::sync::{Arc, Mutex};

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::{IntoResponse, Response},
    routing::{get, post},
    Json, Router,
};
use serde::Deserialize;
use serde_json::{json, Value};

use pd_slots::{MAX_SLOTS, SLOT_NAMES};

use crate::freebsd::{assets_ready, FreeBsdPhase, SharedFreeBsdState};
use crate::serial::SerialCache;

pub type SharedSerial = Arc<Mutex<SerialCache>>;
pub type SharedInjectTx = Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<Vec<u8>>>>>;

#[derive(Clone)]
pub struct BridgeState {
    pub serial:        SharedSerial,
    pub freebsd:       SharedFreeBsdState,
    pub _serial_path:  String,
    pub guest_img_dir: String,
    pub freebsd_ver:   String,
    /// Channel to request a serial-log re-parse from background task
    pub parse_tx:      tokio::sync::watch::Sender<()>,
    /// Channel to inject bytes into QEMU serial socket
    pub inject_tx:     SharedInjectTx,
}

pub fn build_router(state: BridgeState) -> Router {
    Router::new()
        .route("/api/agentos/agents",                 get(get_agents))
        .route("/api/agentos/agents/freebsd/status",  get(get_freebsd_status))
        .route("/api/agentos/agents/spawn",            post(post_spawn))
        .route("/api/agentos/slots",                   get(get_slots))
        .route("/api/agentos/console/status",          get(get_console_status))
        .route("/api/agentos/console/attach/:slot",    post(post_attach))
        .route("/api/agentos/console/inject/:slot",    post(post_inject))
        .route("/api/agentos/console/:slot",           get(get_console_slot))
        .fallback(fallback_404)
        .with_state(state)
}

// ─── handlers ────────────────────────────────────────────────────────────────

async fn get_agents(State(s): State<BridgeState>) -> impl IntoResponse {
    // Trigger a re-parse
    let _ = s.parse_tx.send(());

    let serial = s.serial.lock().unwrap();
    let vm_linux = &serial.lines[14];
    let linux_booted = vm_linux.iter().any(|l| {
        let ll = l.to_lowercase();
        ll.contains("login:") || ll.contains("welcome to") || ll.contains("buildroot")
    });
    let fb_ready = assets_ready(&s.guest_img_dir, &s.freebsd_ver);
    let fb_state = s.freebsd.lock().unwrap().clone();

    let freebsd_status = match &fb_state.phase {
        FreeBsdPhase::Idle      => "not_prepared".to_string(),
        FreeBsdPhase::Running   => "running".to_string(),
        other => format!("{:?}", other).to_lowercase(),
    };
    let freebsd_note: Value = if fb_state.phase == FreeBsdPhase::Running {
        json!("FreeBSD VM is running")
    } else if fb_ready {
        json!("Assets ready — boot FreeBSD from the spawn dialog")
    } else if fb_state.phase == FreeBsdPhase::Error {
        json!(format!("Error: {}", fb_state.error.as_deref().unwrap_or("")))
    } else if fb_state.phase != FreeBsdPhase::Idle {
        json!(format!("{} ({}%)", fb_state.step, fb_state.progress))
    } else {
        json!("EDK2 on-machine · FreeBSD image download required (~400 MB)")
    };

    let mut agents: Vec<Value> = vec![
        json!({
            "id": "linux_vm_0",
            "type": "linux_vm",
            "slot": 14,
            "name": "Linux VM (Buildroot)",
            "status": if linux_booted { "running" } else if !vm_linux.is_empty() { "booting" } else { "stopped" },
            "lines": vm_linux.len(),
            "note": if linux_booted {
                Value::String("Buildroot booted — log in as root (no password)".into())
            } else {
                Value::Null
            },
        }),
        json!({
            "id": "freebsd_vm_0",
            "type": "freebsd_vm",
            "slot": -1i64,
            "name": "FreeBSD VM",
            "status": freebsd_status,
            "lines": 0,
            "note": freebsd_note,
            "freebsdState": {
                "phase":    format!("{:?}", fb_state.phase).to_lowercase(),
                "step":     fb_state.step,
                "progress": fb_state.progress,
                "error":    fb_state.error,
            }
        }),
    ];

    for i in 0..4usize {
        let slot = 9 + i;
        let lines = &serial.lines[slot];
        let failed  = lines.iter().any(|l| {
            let u = l.to_uppercase();
            u.contains("FAILED") || u.contains("ERROR")
        });
        let running = lines.iter().any(|l| {
            let u = l.to_uppercase();
            u.contains("ALIVE") || u.contains("READY")
        });
        let status = if failed {
            "failed"
        } else if running {
            "running"
        } else if !lines.is_empty() {
            "idle"
        } else {
            "empty"
        };
        agents.push(json!({
            "id":     format!("wasm_{}", i),
            "type":   "wasm",
            "slot":   slot,
            "name":   format!("WASM Agent (swap_slot_{})", i),
            "status": status,
            "lines":  lines.len(),
            "note": if failed {
                Value::String("WASM runtime init failed — load a valid .wasm module".into())
            } else {
                Value::Null
            },
        }));
    }

    Json(json!({ "agents": agents }))
}

async fn get_freebsd_status(State(s): State<BridgeState>) -> impl IntoResponse {
    let fb    = s.freebsd.lock().unwrap().clone();
    let ready = assets_ready(&s.guest_img_dir, &s.freebsd_ver);
    Json(json!({
        "phase":       format!("{:?}", fb.phase).to_lowercase(),
        "step":        fb.step,
        "progress":    fb.progress,
        "error":       fb.error,
        "assetsReady": ready,
        "qemuPid":     fb.qemu_pid,
    }))
}

#[derive(Deserialize)]
struct SpawnBody {
    #[serde(rename = "type")]
    agent_type: Option<String>,
    slot:       Option<i32>,
    name:       Option<String>,
}

async fn post_spawn(
    State(s): State<BridgeState>,
    Json(body): Json<SpawnBody>,
) -> impl IntoResponse {
    let agent_type = body.agent_type.as_deref().unwrap_or("");
    if agent_type == "freebsd_vm" {
        let ready = assets_ready(&s.guest_img_dir, &s.freebsd_ver);
        let phase  = s.freebsd.lock().unwrap().phase.clone();

        // Already running — nothing to do.
        if ready && phase == FreeBsdPhase::Running {
            return (StatusCode::OK, Json(json!({
                "ok": true,
                "message": "FreeBSD VM is already running."
            })));
        }

        // Assets on disk and QEMU not yet running — launch it.
        if ready && phase != FreeBsdPhase::Running {
            {
                let mut fb = s.freebsd.lock().unwrap();
                fb.phase    = FreeBsdPhase::Running;
                fb.step     = "Launching QEMU…".into();
                fb.progress = 0;
                fb.error    = None;
            }

            let freebsd_state = Arc::clone(&s.freebsd);
            let guest_img_dir = s.guest_img_dir.clone();
            let freebsd_ver   = s.freebsd_ver.clone();

            tokio::task::spawn_blocking(move || {
                let bios_path  = format!("{}/edk2-aarch64-code.fd", guest_img_dir);
                let drive_path = format!("{}/freebsd-{}-aarch64.img", guest_img_dir, freebsd_ver);

                let mut child = match std::process::Command::new("qemu-system-aarch64")
                    .args([
                        "-machine", "virt,virtualization=on",
                        "-cpu",     "cortex-a57",
                        "-m",       "2G",
                        "-nographic",
                        "-bios",    &bios_path,
                        "-drive",   &format!("file={},format=raw,if=virtio", drive_path),
                        "-serial",  "unix:/tmp/agentos-serial.sock,server=on,wait=off",
                    ])
                    .spawn()
                {
                    Ok(c) => c,
                    Err(e) => {
                        let mut fb = freebsd_state.lock().unwrap();
                        fb.phase    = FreeBsdPhase::Error;
                        fb.error    = Some(format!("Failed to launch qemu-system-aarch64: {}", e));
                        fb.qemu_pid = None;
                        return;
                    }
                };

                let pid = child.id();
                {
                    let mut fb = freebsd_state.lock().unwrap();
                    fb.qemu_pid  = Some(pid);
                    fb.step      = "QEMU running".into();
                    fb.progress  = 100;
                }

                // Wait for QEMU to exit.
                let _ = child.wait();

                let mut fb = freebsd_state.lock().unwrap();
                fb.phase    = FreeBsdPhase::Idle;
                fb.step     = String::new();
                fb.progress = 0;
                fb.qemu_pid = None;
            });

            return (StatusCode::ACCEPTED, Json(json!({
                "ok": true,
                "queued": true,
                "message": "FreeBSD VM launch initiated. Poll /api/agentos/agents/freebsd/status for status."
            })));
        }

        // Assets not ready — kick off a download if we aren't already downloading.
        if phase != FreeBsdPhase::Preparing && phase != FreeBsdPhase::Running {
            {
                let mut fb = s.freebsd.lock().unwrap();
                fb.phase    = FreeBsdPhase::Preparing;
                fb.step     = "Downloading FreeBSD assets...".into();
                fb.progress = 50;
                fb.error    = None;
            }

            let freebsd_state = Arc::clone(&s.freebsd);
            let guest_img_dir = s.guest_img_dir.clone();

            tokio::task::spawn_blocking(move || {
                let result = std::process::Command::new("cargo")
                    .args(["xtask", "fetch-guest", "freebsd",
                           "--output-dir", &guest_img_dir])
                    .status();

                match result {
                    Ok(status) if status.success() => {
                        let mut fb = freebsd_state.lock().unwrap();
                        fb.phase    = FreeBsdPhase::Idle;
                        fb.step     = String::new();
                        fb.progress = 100;
                        fb.error    = None;
                    }
                    Ok(status) => {
                        let mut fb = freebsd_state.lock().unwrap();
                        fb.phase = FreeBsdPhase::Error;
                        fb.error = Some(format!(
                            "cargo xtask fetch-guest exited with status {}", status
                        ));
                    }
                    Err(e) => {
                        let mut fb = freebsd_state.lock().unwrap();
                        fb.phase = FreeBsdPhase::Error;
                        fb.error = Some(format!("Failed to run cargo xtask: {}", e));
                    }
                }
            });

            return (StatusCode::ACCEPTED, Json(json!({
                "ok": true,
                "queued": true,
                "message": "FreeBSD asset download started. Poll /api/agentos/agents/freebsd/status for progress."
            })));
        }

        // Already preparing or running.
        return (StatusCode::ACCEPTED, Json(json!({
            "ok": true,
            "queued": true,
            "message": "FreeBSD asset preparation already in progress."
        })));
    }

    (StatusCode::ACCEPTED, Json(json!({
        "ok": true,
        "queued": true,
        "message": format!(
            "Spawn request for {} queued (bridge stub — seL4 IPC not yet wired)",
            agent_type
        ),
        "slot": body.slot,
        "name": body.name,
    })))
}

async fn get_slots(State(s): State<BridgeState>) -> impl IntoResponse {
    let _ = s.parse_tx.send(());
    let serial = s.serial.lock().unwrap();
    let slots: Vec<Value> = SLOT_NAMES.iter().enumerate().map(|(id, name)| {
        json!({
            "id":     id,
            "name":   name,
            "active": !serial.lines[id].is_empty(),
            "lines":  serial.lines[id].len(),
            "source": "serial",
        })
    }).collect();
    Json(json!({ "slots": slots, "source": "serial_log" }))
}

async fn get_console_status() -> impl IntoResponse {
    Json(json!({
        "active_pd":     0xFFFF_FFFFu64,
        "mode":          1,
        "session_count": 9,
        "source":        "serial_log",
    }))
}

#[derive(Deserialize)]
struct ConsoleCursorQuery {
    cursor: Option<usize>,
}

async fn get_console_slot(
    State(s): State<BridgeState>,
    Path(slot_str): Path<String>,
    Query(q): Query<ConsoleCursorQuery>,
) -> Response {
    let slot: usize = match slot_str.parse() {
        Ok(n) => n,
        Err(_) => {
            return (
                StatusCode::BAD_REQUEST,
                Json(json!({ "error": "invalid slot" })),
            ).into_response();
        }
    };
    if slot >= MAX_SLOTS {
        return (
            StatusCode::BAD_REQUEST,
            Json(json!({ "error": "invalid slot" })),
        ).into_response();
    }
    let cursor = q.cursor.unwrap_or(0);
    let serial = s.serial.lock().unwrap();
    let all_lines = &serial.lines[slot];
    let lines: Vec<&str> = all_lines
        .iter()
        .skip(cursor)
        .map(|s| s.as_str())
        .collect();
    let new_cursor = cursor + lines.len();
    Json(json!({
        "slot":   slot,
        "cursor": new_cursor,
        "lines":  lines,
        "source": "serial_log",
    })).into_response()
}

async fn post_attach(Path(slot_str): Path<String>) -> impl IntoResponse {
    let slot: usize = slot_str.parse().unwrap_or(0);
    Json(json!({ "slot": slot, "ok": true, "source": "serial_log" }))
}

#[derive(Deserialize)]
struct InjectBody {
    data: Option<String>,
}

async fn post_inject(
    State(s): State<BridgeState>,
    Path(slot_str): Path<String>,
    Json(body): Json<InjectBody>,
) -> impl IntoResponse {
    let data = body.data.unwrap_or_default();
    let _slot: usize = slot_str.parse().unwrap_or(0);

    if data.is_empty() {
        return Json(json!({
            "ok": false, "bytes": 0,
            "source": "no_socket", "error": "empty input"
        }));
    }

    let guard = s.inject_tx.lock().unwrap();
    if let Some(tx) = guard.as_ref() {
        let _ = tx.send(data.as_bytes().to_vec());
        Json(json!({ "ok": true, "bytes": data.len(), "source": "socket" }))
    } else {
        Json(json!({
            "ok": false, "bytes": 0,
            "source": "no_socket", "error": "serial socket not connected"
        }))
    }
}

async fn fallback_404() -> impl IntoResponse {
    (StatusCode::NOT_FOUND, Json(json!({ "error": "not found" })))
}
