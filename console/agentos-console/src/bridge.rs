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
use base64::Engine as _;
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use pd_slots::{MAX_SLOTS, SLOT_NAMES};

use crate::freebsd::{assets_ready, FreeBsdPhase, SharedFreeBsdState};
use crate::linux_vm::{ubuntu_assets_ready, create_seed_disk, LinuxVmPhase, SharedLinuxVmState};
use crate::serial::SerialCache;

pub type SharedSerial = Arc<Mutex<SerialCache>>;
pub type SharedInjectTx = Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<Vec<u8>>>>>;

#[derive(Clone)]
pub struct BridgeState {
    pub serial:        SharedSerial,
    pub freebsd:       SharedFreeBsdState,
    pub linux:         SharedLinuxVmState,
    pub _serial_path:  String,
    pub guest_img_dir: String,
    pub freebsd_ver:   String,
    /// Channel to request a serial-log re-parse from background task
    pub parse_tx:      tokio::sync::watch::Sender<()>,
    /// Channel to inject bytes into QEMU serial socket
    pub inject_tx:     SharedInjectTx,
    /// When true, vibe/generate returns a pre-baked mock response without
    /// calling any LLM backend.  Enabled by --mock-codegen CLI flag or
    /// AGENTOS_CODEGEN_BACKEND=mock env var.
    pub mock_codegen:  bool,
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
        .route("/api/agentos/vibe/generate",           post(post_vibe_generate))
        .route("/api/agentos/vibe/compile",            post(post_vibe_compile))
        .fallback(fallback_404)
        .with_state(state)
}

// ─── FreeBSD SSH auto-configuration ──────────────────────────────────────────

/// Connect to the FreeBSD serial socket, wait for the login prompt, then
/// enable and configure SSHD so that `ssh -p 2222 root@localhost` works.
///
/// Runs in a background thread after QEMU starts.  Safe to call even if
/// the VM is still booting — the function polls until the socket appears.
fn configure_freebsd_ssh() {
    use std::io::{Read, Write};
    use std::os::unix::net::UnixStream;
    use std::time::Duration;

    const SOCK: &str = "/tmp/freebsd-serial.sock";

    // Wait up to 3 minutes for the serial socket to appear.
    for _ in 0..180 {
        if std::path::Path::new(SOCK).exists() {
            break;
        }
        std::thread::sleep(Duration::from_secs(1));
    }
    if !std::path::Path::new(SOCK).exists() {
        tracing::warn!("[freebsd] serial socket never appeared — SSH not configured");
        return;
    }

    // Give QEMU a moment to fully initialise the socket.
    std::thread::sleep(Duration::from_millis(500));

    let mut stream = match UnixStream::connect(SOCK) {
        Ok(s) => s,
        Err(e) => {
            tracing::warn!("[freebsd] cannot connect to serial socket: {}", e);
            return;
        }
    };
    stream.set_read_timeout(Some(Duration::from_secs(180))).ok();

    // Read until we see the "login:" prompt (FreeBSD boot complete).
    let mut buf = Vec::<u8>::new();
    loop {
        let mut tmp = [0u8; 512];
        match stream.read(&mut tmp) {
            Ok(0) | Err(_) => {
                tracing::warn!("[freebsd] serial disconnected waiting for login prompt");
                return;
            }
            Ok(n) => {
                buf.extend_from_slice(&tmp[..n]);
                let text = String::from_utf8_lossy(&buf);
                if text.contains("login: ") {
                    break;
                }
                // Discard old data to keep the buffer from growing forever.
                if buf.len() > 65536 {
                    buf.drain(..32768);
                }
            }
        }
    }

    // Log in as root (no password on stock FreeBSD UFS cloud image).
    if stream.write_all(b"root\n").is_err() { return; }
    std::thread::sleep(Duration::from_secs(2));

    // Enable SSHD, set root password, allow root + password auth.
    let commands: &[&[u8]] = &[
        b"sysrc sshd_enable=YES\n",
        b"echo 'PermitRootLogin yes' >> /etc/ssh/sshd_config\n",
        b"echo 'PasswordAuthentication yes' >> /etc/ssh/sshd_config\n",
        b"echo 'agentos' | pw usermod root -h 0\n",
        b"service sshd start\n",
    ];
    for cmd in commands {
        if stream.write_all(cmd).is_err() { return; }
        std::thread::sleep(Duration::from_millis(800));
    }

    tracing::info!("[freebsd] SSH configured — ssh -p 2222 root@localhost (password: agentos)");
}

// ─── handlers ────────────────────────────────────────────────────────────────

async fn get_agents(State(s): State<BridgeState>) -> impl IntoResponse {
    // Trigger a re-parse
    let _ = s.parse_tx.send(());

    let fb_ready = assets_ready(&s.guest_img_dir, &s.freebsd_ver);
    let fb_state = s.freebsd.lock().unwrap().clone();
    let lx_ready = ubuntu_assets_ready(&s.guest_img_dir);
    let lx_state = s.linux.lock().unwrap().clone();

    let freebsd_status = match &fb_state.phase {
        FreeBsdPhase::Idle      => "not_prepared".to_string(),
        FreeBsdPhase::Running   => "running".to_string(),
        other => format!("{:?}", other).to_lowercase(),
    };
    let freebsd_note: Value = if fb_state.phase == FreeBsdPhase::Running {
        json!("FreeBSD VM running — SSH: ssh -p 2222 root@localhost")
    } else if fb_ready {
        json!("Assets ready — boot FreeBSD from the spawn dialog")
    } else if fb_state.phase == FreeBsdPhase::Error {
        json!(format!("Error: {}", fb_state.error.as_deref().unwrap_or("")))
    } else if fb_state.phase != FreeBsdPhase::Idle {
        json!(format!("{} ({}%)", fb_state.step, fb_state.progress))
    } else {
        json!("EDK2 on-machine · FreeBSD image download required (~400 MB)")
    };

    let linux_status = match &lx_state.phase {
        LinuxVmPhase::Idle      => "not_prepared".to_string(),
        LinuxVmPhase::Running   => "running".to_string(),
        other => format!("{:?}", other).to_lowercase(),
    };
    let linux_note: Value = if lx_state.phase == LinuxVmPhase::Running {
        json!("Ubuntu VM running — SSH: ssh -p 2223 ubuntu@localhost (password: agentos)")
    } else if lx_ready {
        json!("Assets ready — boot Ubuntu VM from the spawn dialog")
    } else if lx_state.phase == LinuxVmPhase::Error {
        json!(format!("Error: {}", lx_state.error.as_deref().unwrap_or("")))
    } else if lx_state.phase != LinuxVmPhase::Idle {
        json!(format!("{} ({}%)", lx_state.step, lx_state.progress))
    } else {
        json!("Ubuntu 24.04 LTS AArch64 — image download required (~1 GB)")
    };

    let mut agents: Vec<Value> = vec![
        json!({
            "id": "linux_vm_0",
            "type": "linux_vm",
            "slot": -1i64,
            "name": "Ubuntu Linux VM",
            "status": linux_status,
            "lines": 0,
            "note": linux_note,
            "linuxState": {
                "phase":    format!("{:?}", lx_state.phase).to_lowercase(),
                "step":     lx_state.step,
                "progress": lx_state.progress,
                "error":    lx_state.error,
            }
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

    let serial = s.serial.lock().unwrap();
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

                // Remove stale socket so QEMU can create it fresh.
                let _ = std::fs::remove_file("/tmp/freebsd-serial.sock");

                let mut child = match std::process::Command::new("qemu-system-aarch64")
                    .args([
                        "-machine", "virt,virtualization=on",
                        "-cpu",     "cortex-a57",
                        "-m",       "2G",
                        "-nographic",
                        "-bios",    &bios_path,
                        "-drive",   &format!("file={},format=raw,if=virtio", drive_path),
                        "-netdev",  "user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22",
                        "-device",  "virtio-net-device,netdev=net0",
                        "-device",  "virtio-rng-device",
                        "-serial",  "unix:/tmp/freebsd-serial.sock,server=on,wait=off",
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
                    fb.step      = "QEMU running — waiting for SSH…".into();
                    fb.progress  = 100;
                }

                // Auto-configure SSH via serial once FreeBSD reaches login prompt.
                std::thread::spawn(|| configure_freebsd_ssh());

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
                    .args(["xtask", "fetch-guest", "--os", "freebsd",
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

    if agent_type == "linux_vm" {
        let lx_ready = ubuntu_assets_ready(&s.guest_img_dir);
        let phase    = s.linux.lock().unwrap().phase.clone();

        // Already running — nothing to do.
        if lx_ready && phase == LinuxVmPhase::Running {
            return (StatusCode::OK, Json(json!({
                "ok": true,
                "message": "Ubuntu Linux VM is already running."
            })));
        }

        // Assets on disk and QEMU not yet running — launch it.
        if lx_ready && phase != LinuxVmPhase::Running {
            {
                let mut lx = s.linux.lock().unwrap();
                lx.phase    = LinuxVmPhase::Running;
                lx.step     = "Launching QEMU…".into();
                lx.progress = 0;
                lx.error    = None;
            }

            let linux_state   = Arc::clone(&s.linux);
            let guest_img_dir = s.guest_img_dir.clone();

            tokio::task::spawn_blocking(move || {
                let bios_path   = format!("{}/edk2-aarch64-code.fd",      guest_img_dir);
                let ubuntu_path = format!("{}/ubuntu-24.04-aarch64.img",  guest_img_dir);
                let seed_path   = format!("{}/linux-seed.img",            guest_img_dir);

                // Remove stale socket so QEMU can create it fresh.
                let _ = std::fs::remove_file("/tmp/linux-serial.sock");

                let mut child = match std::process::Command::new("qemu-system-aarch64")
                    .args([
                        "-machine", "virt,virtualization=on",
                        "-cpu",     "cortex-a57",
                        "-m",       "2G",
                        "-nographic",
                        "-bios",    &bios_path,
                        "-drive",   &format!("file={},format=raw,if=virtio", ubuntu_path),
                        "-drive",   &format!("file={},format=raw,if=virtio", seed_path),
                        "-netdev",  "user,id=net0,hostfwd=tcp:127.0.0.1:2223-:22",
                        "-device",  "virtio-net-device,netdev=net0",
                        "-device",  "virtio-rng-device",
                        "-serial",  "unix:/tmp/linux-serial.sock,server=on,wait=off",
                    ])
                    .spawn()
                {
                    Ok(c) => c,
                    Err(e) => {
                        let mut lx = linux_state.lock().unwrap();
                        lx.phase    = LinuxVmPhase::Error;
                        lx.error    = Some(format!("Failed to launch qemu-system-aarch64: {}", e));
                        lx.qemu_pid = None;
                        return;
                    }
                };

                let pid = child.id();
                {
                    let mut lx = linux_state.lock().unwrap();
                    lx.qemu_pid  = Some(pid);
                    lx.step      = "QEMU running".into();
                    lx.progress  = 100;
                }

                let _ = child.wait();

                let mut lx = linux_state.lock().unwrap();
                lx.phase    = LinuxVmPhase::Idle;
                lx.step     = String::new();
                lx.progress = 0;
                lx.qemu_pid = None;
            });

            return (StatusCode::ACCEPTED, Json(json!({
                "ok": true,
                "queued": true,
                "message": "Ubuntu Linux VM launch initiated. SSH on port 2223 once booted (user: ubuntu, password: agentos)."
            })));
        }

        // Assets not ready — kick off download + seed disk creation.
        if phase != LinuxVmPhase::Preparing && phase != LinuxVmPhase::Running {
            {
                let mut lx = s.linux.lock().unwrap();
                lx.phase    = LinuxVmPhase::Preparing;
                lx.step     = "Downloading Ubuntu 24.04 assets...".into();
                lx.progress = 10;
                lx.error    = None;
            }

            let linux_state   = Arc::clone(&s.linux);
            let guest_img_dir = s.guest_img_dir.clone();

            tokio::task::spawn_blocking(move || {
                // Step 1: download Ubuntu image.
                let result = std::process::Command::new("cargo")
                    .args(["xtask", "fetch-guest", "--os", "ubuntu",
                           "--output-dir", &guest_img_dir])
                    .status();

                match result {
                    Ok(status) if status.success() => {}
                    Ok(status) => {
                        let mut lx = linux_state.lock().unwrap();
                        lx.phase = LinuxVmPhase::Error;
                        lx.error = Some(format!(
                            "cargo xtask fetch-guest --os ubuntu exited with status {}", status
                        ));
                        return;
                    }
                    Err(e) => {
                        let mut lx = linux_state.lock().unwrap();
                        lx.phase = LinuxVmPhase::Error;
                        lx.error = Some(format!("Failed to run cargo xtask: {}", e));
                        return;
                    }
                }

                // Step 2: create cloud-init seed disk.
                {
                    let mut lx = linux_state.lock().unwrap();
                    lx.step     = "Creating cloud-init seed disk...".into();
                    lx.progress = 90;
                }

                let seed_path = format!("{}/linux-seed.img", guest_img_dir);
                if let Err(e) = create_seed_disk(&seed_path) {
                    let mut lx = linux_state.lock().unwrap();
                    lx.phase = LinuxVmPhase::Error;
                    lx.error = Some(format!("Failed to create seed disk: {}", e));
                    return;
                }

                let mut lx = linux_state.lock().unwrap();
                lx.phase    = LinuxVmPhase::Idle;
                lx.step     = String::new();
                lx.progress = 100;
                lx.error    = None;
            });

            return (StatusCode::ACCEPTED, Json(json!({
                "ok": true,
                "queued": true,
                "message": "Ubuntu asset download started. Poll /api/agentos/agents for status."
            })));
        }

        // Already preparing or running.
        return (StatusCode::ACCEPTED, Json(json!({
            "ok": true,
            "queued": true,
            "message": "Ubuntu Linux VM preparation already in progress."
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

// ─── /api/agentos/vibe/generate ──────────────────────────────────────────────

/// Full system prompt injected for every agentOS service code-generation request.
const AGENTOS_SERVICE_SYSTEM_PROMPT: &str = "\
You are an expert embedded OS engineer. You are generating a replacement system \
service for agentOS, built on the seL4 microkernel. The service runs as a WASM \
module loaded by wasm3.\n\
\n\
Your output must be valid C99 source code only — no markdown, no explanation, \
no code fences. The code must compile with: \
clang --target=wasm32-wasi -O2 -nostdlib -Wl,--no-entry -Wl,--export-all\n\
\n\
The service MUST export these three functions:\n\
  int service_init(void);\n\
  int service_dispatch(uint32_t label, uint32_t in_count, uint32_t out_count);\n\
  int service_health(void);\n\
\n\
Message registers are accessed via:\n\
  uint32_t aos_mr_get(int idx);\n\
  void aos_mr_set(int idx, uint32_t val);\n\
(These are provided by the agentOS WASM runtime — do not implement them yourself.)\n\
\n\
Return 0 on success, negative errno on error.";

/// ABI shim header written into the temp dir before compilation.
const AGENTOS_WASM_ABI_H: &str = "\
/* agentOS WASM Service ABI — injected by compile service */\n\
#ifndef AGENTOS_WASM_ABI_H\n\
#define AGENTOS_WASM_ABI_H\n\
#include <stdint.h>\n\
\n\
/* Provided by agentOS WASM runtime */\n\
extern uint32_t aos_mr_get(int idx);\n\
extern void     aos_mr_set(int idx, uint32_t val);\n\
extern void     aos_log_str(const char *s, int len);\n\
\n\
/* IPC label constants */\n\
#define AOS_LABEL_INIT     0x0000u\n\
#define AOS_LABEL_HEALTH   0xFFFFu\n\
\n\
/* Service must export these */\n\
int service_init(void);\n\
int service_dispatch(uint32_t label, uint32_t in_count, uint32_t out_count);\n\
int service_health(void);\n\
#endif /* AGENTOS_WASM_ABI_H */\n";

#[derive(Deserialize)]
struct VibeGenerateBody {
    prompt: String,
    system_prompt: Option<String>,
    service_id: Option<String>,
}

/// Minimal conformant agentOS service returned in mock-codegen mode.
/// Compiles cleanly with clang --target=wasm32-wasi -O2 -nostdlib
/// and satisfies all three mandatory ABI exports.
const MOCK_SERVICE_C: &str = r#"#include <stdint.h>
extern uint32_t aos_mr_get(int idx);
extern void     aos_mr_set(int idx, uint32_t val);
extern void     aos_log_str(const char *s, int len);

/* Minimal key-value store: 8 slots, keys/values up to 31 bytes */
#define KV_SLOTS 8
#define KV_LEN   32
static char kv_key[KV_SLOTS][KV_LEN];
static char kv_val[KV_SLOTS][KV_LEN];
static uint32_t kv_used;

static int kv_find(uint32_t key) {
    for (uint32_t i = 0; i < kv_used; i++)
        if (*(uint32_t *)kv_key[i] == key) return (int)i;
    return -1;
}

int service_init(void) { kv_used = 0; return 0; }

int service_dispatch(uint32_t label, uint32_t in_count, uint32_t out_count) {
    (void)in_count; (void)out_count;
    uint32_t key = aos_mr_get(1);
    if (label == 0x30u) { /* WRITE */
        uint32_t val = aos_mr_get(2);
        int i = kv_find(key);
        if (i < 0 && kv_used < KV_SLOTS) i = (int)kv_used++;
        if (i >= 0) { *(uint32_t *)kv_key[i] = key; *(uint32_t *)kv_val[i] = val; }
        aos_mr_set(1, 0); return 0;
    }
    if (label == 0x31u) { /* READ */
        int i = kv_find(key);
        aos_mr_set(1, i >= 0 ? *(uint32_t *)kv_val[i] : 0u);
        aos_mr_set(2, i >= 0 ? 1u : 0u); return 0;
    }
    if (label == 0x32u) { /* DELETE */
        int i = kv_find(key);
        if (i >= 0) { kv_key[i][0] = 0; } aos_mr_set(1, 0); return 0;
    }
    if (label == 0xFFFFu) { return service_health(); } /* HEALTH */
    return -1;
}

int service_health(void) { aos_mr_set(1, kv_used); return 0; }
"#;

async fn post_vibe_generate(
    State(state): State<BridgeState>,
    Json(body): Json<VibeGenerateBody>,
) -> impl IntoResponse {
    // ── Mock mode ─────────────────────────────────────────────────────────────
    if state.mock_codegen {
        tracing::info!("[vibe/generate] mock-codegen mode — returning pre-baked service");
        return (
            StatusCode::OK,
            Json(json!({
                "ok":          true,
                "code":        MOCK_SERVICE_C,
                "tokens_used": 0u64,
                "backend":     "mock",
            })),
        );
    }

    // Build the user-facing prompt. If a service_id is given, prepend the ABI
    // requirement so the model knows the interface contract.
    let user_prompt = if let Some(ref sid) = body.service_id {
        format!(
            "Service ID: {}\n\nABI contract:\n{}\n\nRequest:\n{}",
            sid, AGENTOS_SERVICE_SYSTEM_PROMPT, body.prompt
        )
    } else {
        body.prompt.clone()
    };

    // The system prompt: caller-supplied takes precedence, otherwise use the
    // hardcoded agentOS service prompt.
    let system_prompt = body
        .system_prompt
        .as_deref()
        .unwrap_or(AGENTOS_SERVICE_SYSTEM_PROMPT);

    // Try HTTP API first, then fall back to CLI tools.
    match run_codegen_http_then_cli(system_prompt, &user_prompt).await {
        Ok((code, tokens_used, backend)) => (
            StatusCode::OK,
            Json(json!({
                "ok":          true,
                "code":        code,
                "tokens_used": tokens_used,
                "backend":     backend,
            })),
        ),
        Err(e) => (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(json!({
                "ok":    false,
                "error": e,
            })),
        ),
    }
}

/// Detect and call the best available codegen backend (HTTP API then CLI).
/// Returns `(generated_code, tokens_used, backend_label)`.
async fn run_codegen_http_then_cli(
    system_prompt: &str,
    user_prompt: &str,
) -> Result<(String, u64, String), String> {
    use reqwest::Client;

    // ── 1. HTTP API backends ─────────────────────────────────────────────────

    // Check ANTHROPIC_API_KEY first (most likely for agentOS development).
    if let Ok(api_key) = std::env::var("ANTHROPIC_API_KEY") {
        if !api_key.is_empty() {
            let base_url = std::env::var("ANTHROPIC_BASE_URL")
                .unwrap_or_else(|_| "https://api.anthropic.com".to_string());
            let model = std::env::var("ANTHROPIC_MODEL")
                .unwrap_or_else(|_| "claude-sonnet-4-5".to_string());
            match call_openai_compat(&Client::new(), &base_url, &api_key, &model, system_prompt, user_prompt).await {
                Ok((content, tok_in, tok_out)) => {
                    return Ok((content, tok_in + tok_out, "anthropic-api".to_string()));
                }
                Err(e) => {
                    tracing::warn!("[vibe/generate] Anthropic API error: {}", e);
                    // fall through
                }
            }
        }
    }

    // Check OPENAI_API_KEY.
    if let Ok(api_key) = std::env::var("OPENAI_API_KEY") {
        if !api_key.is_empty() {
            let base_url = std::env::var("OPENAI_BASE_URL")
                .unwrap_or_else(|_| "https://api.openai.com".to_string());
            let model = std::env::var("OPENAI_MODEL")
                .unwrap_or_else(|_| "gpt-4o".to_string());
            match call_openai_compat(&Client::new(), &base_url, &api_key, &model, system_prompt, user_prompt).await {
                Ok((content, tok_in, tok_out)) => {
                    return Ok((content, tok_in + tok_out, "http-api".to_string()));
                }
                Err(e) => {
                    tracing::warn!("[vibe/generate] OpenAI API error: {}", e);
                    // fall through
                }
            }
        }
    }

    // ── 2. CLI tool fallback ─────────────────────────────────────────────────

    let full_prompt = format!("{}\n\n{}", system_prompt, user_prompt);

    let cli_candidates = ["claude", "codex", "cursor"];
    for cli_name in cli_candidates {
        if let Some(cli_path) = find_in_path(cli_name) {
            let label = format!("{}-cli", cli_name);
            let cli_path_clone = cli_path.clone();
            let prompt_clone   = full_prompt.clone();

            let result = tokio::task::spawn_blocking(move || {
                invoke_cli_sync(&cli_path_clone, &prompt_clone)
            })
            .await
            .map_err(|e| format!("CLI task panicked: {}", e))?;

            match result {
                Ok(content) => {
                    let tokens = estimate_tokens_bridge(&full_prompt) + estimate_tokens_bridge(&content);
                    return Ok((content, tokens, label));
                }
                Err(e) => {
                    tracing::warn!("[vibe/generate] CLI '{}' error: {}", cli_name, e);
                    // try next candidate
                }
            }
        }
    }

    Err(
        "No codegen backend available. \
         Set OPENAI_API_KEY or ANTHROPIC_API_KEY, or install claude/codex."
        .to_string(),
    )
}

/// POST to an OpenAI-compatible `/v1/chat/completions` endpoint.
async fn call_openai_compat(
    client: &reqwest::Client,
    base_url: &str,
    api_key: &str,
    model: &str,
    system_prompt: &str,
    user_prompt: &str,
) -> Result<(String, u64, u64), String> {
    let url = format!("{}/v1/chat/completions", base_url.trim_end_matches('/'));

    let body = serde_json::json!({
        "model": model,
        "messages": [
            { "role": "system", "content": system_prompt },
            { "role": "user",   "content": user_prompt   },
        ],
        "max_tokens": 4096u32,
        "stream": false,
    });

    let mut builder = client.post(&url).json(&body);
    if !api_key.is_empty() {
        builder = builder.bearer_auth(api_key);
    }

    let resp = builder
        .send()
        .await
        .map_err(|e| format!("HTTP request failed: {}", e))?;

    let status = resp.status();
    if !status.is_success() {
        let text = resp.text().await.unwrap_or_default();
        return Err(format!("API {} error {}: {}", url, status, text));
    }

    let json: serde_json::Value = resp
        .json()
        .await
        .map_err(|e| format!("JSON decode failed: {}", e))?;

    let content = json["choices"][0]["message"]["content"]
        .as_str()
        .ok_or_else(|| "missing choices[0].message.content".to_string())?
        .to_string();

    let tokens_in  = json["usage"]["prompt_tokens"].as_u64().unwrap_or_else(|| estimate_tokens_bridge(user_prompt));
    let tokens_out = json["usage"]["completion_tokens"].as_u64().unwrap_or_else(|| estimate_tokens_bridge(&content));

    Ok((content, tokens_in, tokens_out))
}

/// Locate an executable by name in `PATH`.
fn find_in_path(name: &str) -> Option<String> {
    let path_var = std::env::var("PATH").unwrap_or_default();
    for dir in path_var.split(':') {
        let candidate = std::path::Path::new(dir).join(name);
        if candidate.is_file() {
            return Some(candidate.to_string_lossy().to_string());
        }
    }
    None
}

/// Rough token estimate: 1 token ≈ 4 chars.
fn estimate_tokens_bridge(text: &str) -> u64 {
    (text.len() as u64 + 3) / 4
}

/// Synchronous CLI invocation (used inside `spawn_blocking`).
fn invoke_cli_sync(cli_path: &str, full_prompt: &str) -> Result<String, String> {
    use std::process::{Command, Stdio};
    use std::io::Write;

    let cli_name = std::path::Path::new(cli_path)
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or(cli_path);

    let mut cmd = if cli_name == "claude" {
        let mut c = Command::new(cli_path);
        c.arg("-p").arg(full_prompt);
        c
    } else {
        let mut c = Command::new(cli_path);
        c.arg(full_prompt);
        c
    };

    cmd.stdin(Stdio::piped())
       .stdout(Stdio::piped())
       .stderr(Stdio::piped());

    let mut child = cmd.spawn()
        .map_err(|e| format!("failed to spawn '{}': {}", cli_path, e))?;

    // Also pipe to stdin in case the CLI reads from there.
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(full_prompt.as_bytes());
    }

    let output = child.wait_with_output()
        .map_err(|e| format!("wait failed: {}", e))?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return Err(format!("exit {}: {}", output.status, stderr));
    }

    Ok(String::from_utf8_lossy(&output.stdout).to_string())
}

// ─── /api/agentos/vibe/compile ───────────────────────────────────────────────

#[derive(Deserialize)]
struct VibeCompileBody {
    source_c: String,
    service_id: Option<String>,
}

async fn post_vibe_compile(
    Json(body): Json<VibeCompileBody>,
) -> impl IntoResponse {
    match compile_to_wasm(
        &body.source_c,
        body.service_id.as_deref().unwrap_or("service"),
    )
    .await
    {
        Ok((wasm_bytes, sha256_hex)) => {
            let wasm_b64 = base64::engine::general_purpose::STANDARD.encode(&wasm_bytes);
            (
                StatusCode::OK,
                Json(json!({
                    "ok":       true,
                    "wasm_b64": wasm_b64,
                    "size":     wasm_bytes.len(),
                    "sha256":   sha256_hex,
                })),
            )
        }
        Err(CompileError::ClangNotFound) => (
            StatusCode::SERVICE_UNAVAILABLE,
            Json(json!({
                "ok":    false,
                "error": "clang not found — install llvm with wasm32-wasi target \
                          (brew install llvm  or  apt install clang)",
            })),
        ),
        Err(CompileError::CompileFailed { stderr }) => (
            StatusCode::UNPROCESSABLE_ENTITY,
            Json(json!({
                "ok":              false,
                "error":           "compilation failed",
                "compiler_output": stderr,
            })),
        ),
        Err(CompileError::Other(msg)) => (
            StatusCode::INTERNAL_SERVER_ERROR,
            Json(json!({
                "ok":    false,
                "error": msg,
            })),
        ),
    }
}

enum CompileError {
    ClangNotFound,
    CompileFailed { stderr: String },
    Other(String),
}

async fn compile_to_wasm(
    source_c: &str,
    service_id: &str,
) -> Result<(Vec<u8>, String), CompileError> {
    use tokio::process::Command;

    let tmpdir = std::env::temp_dir();
    let timestamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_micros();

    // Sanitise service_id for use in filenames.
    let safe_id: String = service_id
        .chars()
        .map(|c| if c.is_alphanumeric() || c == '.' || c == '-' { c } else { '_' })
        .collect();

    let src_path  = tmpdir.join(format!("{}_{}.c", safe_id, timestamp));
    let out_path  = tmpdir.join(format!("{}_{}.wasm", safe_id, timestamp));
    let hdr_path  = tmpdir.join("agentos_wasm_abi.h");

    // Write source and ABI header.
    tokio::fs::write(&src_path, source_c)
        .await
        .map_err(|e| CompileError::Other(format!("write source: {}", e)))?;
    tokio::fs::write(&hdr_path, AGENTOS_WASM_ABI_H)
        .await
        .map_err(|e| CompileError::Other(format!("write header: {}", e)))?;

    // Find a suitable clang binary.
    let clang_bin = ["clang", "clang-17", "clang-16", "clang-15"]
        .iter()
        .find(|&&name| find_in_path(name).is_some())
        .copied()
        .ok_or(CompileError::ClangNotFound)?;

    let output = Command::new(clang_bin)
        .arg("--target=wasm32-wasi")
        .arg("-O2")
        .arg("-nostdlib")
        .arg("-Wl,--no-entry")
        .arg("-Wl,--export-all")
        .arg(format!("-include{}", hdr_path.display()))
        .arg("-o")
        .arg(&out_path)
        .arg(&src_path)
        .output()
        .await
        .map_err(|e| CompileError::Other(format!("spawn clang: {}", e)))?;

    // Clean up source file regardless of outcome.
    let _ = tokio::fs::remove_file(&src_path).await;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr).to_string();
        return Err(CompileError::CompileFailed { stderr });
    }

    // Read and hash the WASM output.
    let wasm_bytes = tokio::fs::read(&out_path)
        .await
        .map_err(|e| CompileError::Other(format!("read wasm: {}", e)))?;

    let _ = tokio::fs::remove_file(&out_path).await;

    let sha256_hex = hex::encode(Sha256::digest(&wasm_bytes));

    Ok((wasm_bytes, sha256_hex))
}

// ─── fallback ────────────────────────────────────────────────────────────────

async fn fallback_404() -> impl IntoResponse {
    (StatusCode::NOT_FOUND, Json(json!({ "error": "not found" })))
}
