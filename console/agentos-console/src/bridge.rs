//! Stub agentOS bridge HTTP server (port 8790).
//!
//! Implements all /api/agentos/* stub routes backed by the SerialCache.
//! Runs as a separate axum Router on a separate TcpListener, but in the
//! same process.

use std::collections::HashMap;
use std::convert::Infallible;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::{
    extract::{Path, Query, State},
    http::StatusCode,
    response::{sse::{Event, KeepAlive, Sse}, IntoResponse, Response},
    routing::{delete, get, post},
    Json, Router,
};
use base64::Engine as _;
use futures::{stream, StreamExt as _};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use pd_slots::{MAX_SLOTS, SLOT_NAMES};

use crate::freebsd::{assets_ready, FreeBsdPhase, SharedFreeBsdState, VmExtraDevice};
use crate::linux_vm::{ubuntu_assets_ready, create_seed_disk, LinuxVmPhase, SharedLinuxVmState};
use crate::serial::SerialCache;
use crate::ws_log::{LogBroadcast, LogBroadcastTx};

pub type SharedSerial     = Arc<Mutex<SerialCache>>;
pub type SharedInjectTx   = Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<Vec<u8>>>>>;
/// seL4 VM state registry: vm_id → "running" | "stopped"
/// Populated by parsing \x01VM:start/stop:id escape sequences from console_shell.
pub type SeL4VmRegistry   = Arc<Mutex<HashMap<String, String>>>;

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
    /// Broadcast channel for live serial log lines (shared with ws_log)
    pub log_tx:        LogBroadcastTx,
    /// seL4-managed VM lifecycle state (updated from \x01VM: serial escapes)
    pub sel4_vms:      SeL4VmRegistry,
    /// When true, vibe/generate returns a pre-baked mock response without
    /// calling any LLM backend.  Enabled by --mock-codegen CLI flag or
    /// AGENTOS_CODEGEN_BACKEND=mock env var.
    pub mock_codegen:  bool,
}

pub fn build_router(state: BridgeState) -> Router {
    Router::new()
        .route("/api/agentos/agents",                     get(get_agents))
        .route("/api/agentos/agents/freebsd/status",      get(get_freebsd_status))
        .route("/api/agentos/agents/spawn",                post(post_spawn))
        .route("/api/agentos/slots",                       get(get_slots))
        // Static console routes before :slot wildcard
        .route("/api/agentos/console/status",              get(get_console_status))
        .route("/api/agentos/console/stream",              get(get_console_stream))
        .route("/api/agentos/console/cmd",                 post(post_console_cmd))
        .route("/api/agentos/console/vms",                 get(get_sel4_vms))
        .route("/api/agentos/console/attach/:slot",        post(post_attach))
        .route("/api/agentos/console/inject/:slot",        post(post_inject))
        .route("/api/agentos/console/:slot",               get(get_console_slot))
        .route("/api/agentos/vibe/generate",               post(post_vibe_generate))
        .route("/api/agentos/vibe/compile",                post(post_vibe_compile))
        // VM management
        .route("/api/agentos/vms",                         get(get_vms))
        .route("/api/agentos/vms/:vm_id/devices",          get(get_vm_devices))
        .route("/api/agentos/vms/:vm_id/devices",          post(post_vm_device))
        .route("/api/agentos/vms/:vm_id/devices/:dev_id",  delete(delete_vm_device))
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

// ─── VM management API ────────────────────────────────────────────────────────

/// Send a single QMP command to a running QEMU instance and return the reply.
/// Performs the QMP capability negotiation handshake on first contact.
/// Returns `None` if the socket is unavailable or the command fails.
fn qmp_exec(sock_path: &str, cmd: &str) -> Option<String> {
    use std::io::{BufRead, BufReader, Write};
    use std::os::unix::net::UnixStream;
    use std::time::Duration;

    let mut stream = UnixStream::connect(sock_path).ok()?;
    stream.set_read_timeout(Some(Duration::from_secs(5))).ok();
    stream.set_write_timeout(Some(Duration::from_secs(5))).ok();

    let mut reader = BufReader::new(stream.try_clone().ok()?);

    // Read the QMP greeting line.
    let mut greeting = String::new();
    reader.read_line(&mut greeting).ok()?;

    // Send capability negotiation.
    stream.write_all(b"{\"execute\":\"qmp_capabilities\"}\n").ok()?;

    // Read the {"return":{}} ack.
    let mut ack = String::new();
    reader.read_line(&mut ack).ok()?;

    // Send the actual command.
    stream.write_all(cmd.as_bytes()).ok()?;
    stream.write_all(b"\n").ok()?;

    // Read the reply.
    let mut reply = String::new();
    reader.read_line(&mut reply).ok()?;
    Some(reply)
}

/// Attempt to hotplug a disk into a running QEMU VM via QMP.
/// Returns true if the command was accepted.
fn qmp_hotplug_disk(sock_path: &str, dev_id: &str, img_path: &str) -> bool {
    // Step 1: add the block device backend.
    let blockdev = format!(
        "{{\"execute\":\"blockdev-add\",\"arguments\":{{\"driver\":\"raw\",\"node-name\":\"{id}\",\"file\":{{\"driver\":\"file\",\"filename\":\"{path}\"}}}}}}",
        id = dev_id, path = img_path
    );
    if qmp_exec(sock_path, &blockdev).is_none() { return false; }

    // Step 2: attach the virtio-blk device.
    let devaddr = format!(
        "{{\"execute\":\"device_add\",\"arguments\":{{\"driver\":\"virtio-blk-device\",\"drive\":\"{id}\",\"id\":\"{id}\"}}}}",
        id = dev_id
    );
    qmp_exec(sock_path, &devaddr).is_some()
}

/// Attempt to hotplug a NIC into a running QEMU VM via QMP.
fn qmp_hotplug_nic(sock_path: &str, dev_id: &str, host_port: u16) -> bool {
    let netdev = format!(
        "{{\"execute\":\"netdev_add\",\"arguments\":{{\"type\":\"user\",\"id\":\"{id}\",\"hostfwd\":\"tcp:127.0.0.1:{port}-:22\"}}}}",
        id = dev_id, port = host_port
    );
    if qmp_exec(sock_path, &netdev).is_none() { return false; }

    let devaddr = format!(
        "{{\"execute\":\"device_add\",\"arguments\":{{\"driver\":\"virtio-net-device\",\"netdev\":\"{id}\",\"id\":\"{id}\"}}}}",
        id = dev_id
    );
    qmp_exec(sock_path, &devaddr).is_some()
}

/// Build the JSON representation of a VM for the /api/agentos/vms response.
fn vm_json(
    id: &str,
    name: &str,
    state: &str,
    ram_mb: u32,
    ssh_port: u16,
    ssh_user: &str,
    extra_devices: &[VmExtraDevice],
    boot_disk: &str,
) -> Value {
    // Built-in devices always present.
    let mut devices: Vec<Value> = vec![
        json!({
            "id":   "boot",
            "type": "disk",
            "label": "Boot disk",
            "path": boot_disk,
            "live": true,
        }),
        json!({
            "id":   "net0",
            "type": "nic",
            "label": format!("Primary NIC (SSH port {})", ssh_port),
            "port": ssh_port,
            "live": true,
        }),
    ];
    for d in extra_devices {
        devices.push(serde_json::to_value(d).unwrap_or(Value::Null));
    }

    json!({
        "id":           id,
        "name":         name,
        "state":        state,
        "ram_mb":       ram_mb,
        "ssh_port":     ssh_port,
        "ssh_user":     ssh_user,
        "ssh_password": "agentos",
        "ssh_note":     format!("ssh -p {} {}@localhost  (password: agentos)", ssh_port, ssh_user),
        "devices":      devices,
    })
}

async fn get_vms(State(s): State<BridgeState>) -> impl IntoResponse {
    let fb      = s.freebsd.lock().unwrap().clone();
    let lx      = s.linux.lock().unwrap().clone();
    let fb_disk = format!("{}/freebsd-{}-aarch64.img", s.guest_img_dir, s.freebsd_ver);
    let lx_disk = format!("{}/ubuntu-24.04-aarch64.img", s.guest_img_dir);

    let fb_state = match fb.phase {
        FreeBsdPhase::Running   => "running",
        FreeBsdPhase::Preparing => "preparing",
        FreeBsdPhase::Error     => "error",
        FreeBsdPhase::Idle      => "stopped",
    };
    let lx_state = match lx.phase {
        LinuxVmPhase::Running   => "running",
        LinuxVmPhase::Preparing => "preparing",
        LinuxVmPhase::Error     => "error",
        LinuxVmPhase::Idle      => "stopped",
    };

    Json(json!([
        vm_json("freebsd", "FreeBSD 14.4 (aarch64)", fb_state,
                2048, 2222, "root", &fb.devices, &fb_disk),
        vm_json("ubuntu",  "Ubuntu 24.04 LTS (aarch64)", lx_state,
                2048, 2223, "ubuntu", &lx.devices, &lx_disk),
    ]))
}

async fn get_vm_devices(
    State(s): State<BridgeState>,
    Path(vm_id): Path<String>,
) -> impl IntoResponse {
    match vm_id.as_str() {
        "freebsd" => {
            let fb = s.freebsd.lock().unwrap();
            Json(json!({ "vm": "freebsd", "devices": fb.devices })).into_response()
        }
        "ubuntu" => {
            let lx = s.linux.lock().unwrap();
            Json(json!({ "vm": "ubuntu", "devices": lx.devices })).into_response()
        }
        _ => (StatusCode::NOT_FOUND, Json(json!({ "error": "unknown VM id" }))).into_response(),
    }
}

#[derive(Deserialize)]
struct AddDeviceBody {
    #[serde(rename = "type")]
    kind:  String,
    label: Option<String>,
    /// For disk: path to existing image, or create a new one if size_gb is given.
    path:  Option<String>,
    /// For disk: size in GB for a new image to be created.
    size_gb: Option<u32>,
    /// For NIC: host-side TCP port (auto-assigned if omitted).
    port: Option<u16>,
}

async fn post_vm_device(
    State(s): State<BridgeState>,
    Path(vm_id): Path<String>,
    Json(body): Json<AddDeviceBody>,
) -> impl IntoResponse {
    let kind = body.kind.trim().to_lowercase();
    if kind != "disk" && kind != "nic" {
        return (StatusCode::BAD_REQUEST,
                Json(json!({ "error": "type must be 'disk' or 'nic'" }))).into_response();
    }

    // Build the device entry.
    let dev_id = format!("{}-{}", kind, uuid_short());
    let label  = body.label.unwrap_or_else(|| format!("Extra {}", kind));

    let (disk_path, nic_port, qmp_sock): (Option<String>, Option<u16>, Option<String>) =
        match vm_id.as_str() {
            "freebsd" => {
                let fb = s.freebsd.lock().unwrap();
                let p = if kind == "disk" {
                    resolve_disk_path(&body.path, body.size_gb, &dev_id, &s.guest_img_dir)
                } else { None };
                let port = if kind == "nic" {
                    Some(body.port.unwrap_or_else(|| next_free_port_freebsd(&fb.devices)))
                } else { None };
                (p, port, fb.qmp_sock.clone())
            }
            "ubuntu" => {
                let lx = s.linux.lock().unwrap();
                let p = if kind == "disk" {
                    resolve_disk_path(&body.path, body.size_gb, &dev_id, &s.guest_img_dir)
                } else { None };
                let port = if kind == "nic" {
                    Some(body.port.unwrap_or_else(|| next_free_port_linux(&lx.devices)))
                } else { None };
                (p, port, lx.qmp_sock.clone())
            }
            _ => return (StatusCode::NOT_FOUND,
                         Json(json!({ "error": "unknown VM id" }))).into_response(),
        };

    // If disk: create image file if size_gb was specified and path doesn't exist.
    if let Some(ref p) = disk_path {
        if !std::path::Path::new(p).exists() {
            let size_gb = body.size_gb.unwrap_or(10);
            match std::process::Command::new("qemu-img")
                .args(["create", "-f", "raw", p, &format!("{}G", size_gb)])
                .status()
            {
                Ok(st) if st.success() => {}
                Ok(st) => return (StatusCode::INTERNAL_SERVER_ERROR,
                    Json(json!({ "error": format!("qemu-img create failed: {}", st) }))).into_response(),
                Err(e) => return (StatusCode::INTERNAL_SERVER_ERROR,
                    Json(json!({ "error": format!("qemu-img not found: {}", e) }))).into_response(),
            }
        }
    }

    let mut live = false;

    // Attempt QMP hotplug if the VM is currently running.
    if let Some(ref sock) = qmp_sock {
        if std::path::Path::new(sock).exists() {
            live = if kind == "disk" {
                qmp_hotplug_disk(sock, &dev_id, disk_path.as_deref().unwrap_or(""))
            } else {
                qmp_hotplug_nic(sock, &dev_id, nic_port.unwrap_or(0))
            };
        }
    }

    let device = VmExtraDevice {
        id:    dev_id.clone(),
        kind:  kind.clone(),
        label: label.clone(),
        path:  disk_path,
        port:  nic_port,
        live,
    };

    // Add to state.
    match vm_id.as_str() {
        "freebsd" => s.freebsd.lock().unwrap().devices.push(device.clone()),
        "ubuntu"  => s.linux.lock().unwrap().devices.push(device.clone()),
        _ => {}
    }

    let note = if live {
        "Device hotplugged into running VM".to_string()
    } else {
        "Device queued — will be attached at next VM start".to_string()
    };

    (StatusCode::CREATED, Json(json!({
        "ok":     true,
        "device": device,
        "note":   note,
    }))).into_response()
}

async fn delete_vm_device(
    State(s): State<BridgeState>,
    Path((vm_id, dev_id)): Path<(String, String)>,
) -> impl IntoResponse {
    let removed = match vm_id.as_str() {
        "freebsd" => {
            let mut fb = s.freebsd.lock().unwrap();
            let before = fb.devices.len();
            fb.devices.retain(|d| d.id != dev_id);
            fb.devices.len() < before
        }
        "ubuntu" => {
            let mut lx = s.linux.lock().unwrap();
            let before = lx.devices.len();
            lx.devices.retain(|d| d.id != dev_id);
            lx.devices.len() < before
        }
        _ => return (StatusCode::NOT_FOUND,
                     Json(json!({ "error": "unknown VM id" }))).into_response(),
    };

    if removed {
        Json(json!({ "ok": true, "note": "Device removed (takes effect at next VM start if live-attached)" })).into_response()
    } else {
        (StatusCode::NOT_FOUND, Json(json!({ "error": "device not found" }))).into_response()
    }
}

// ─── Device helpers ───────────────────────────────────────────────────────────

fn uuid_short() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let t = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    format!("{:x}", t.subsec_nanos() ^ (t.as_secs() as u32))
}

fn resolve_disk_path(path: &Option<String>, size_gb: Option<u32>, dev_id: &str, img_dir: &str) -> Option<String> {
    if let Some(p) = path.as_ref().filter(|p| !p.trim().is_empty()) {
        return Some(p.clone());
    }
    if size_gb.is_some() {
        return Some(format!("{}/{}.img", img_dir, dev_id));
    }
    None
}

fn next_free_port_freebsd(devices: &[VmExtraDevice]) -> u16 {
    let used: std::collections::HashSet<u16> = devices.iter()
        .filter(|d| d.kind == "nic")
        .filter_map(|d| d.port)
        .collect();
    (2230u16..).find(|p| !used.contains(p)).unwrap_or(2230)
}

fn next_free_port_linux(devices: &[VmExtraDevice]) -> u16 {
    let used: std::collections::HashSet<u16> = devices.iter()
        .filter(|d| d.kind == "nic")
        .filter_map(|d| d.port)
        .collect();
    (2240u16..).find(|p| !used.contains(p)).unwrap_or(2240)
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
                let qmp_sock   = "/tmp/freebsd-qmp.sock".to_string();

                // Remove stale sockets so QEMU can create them fresh.
                let _ = std::fs::remove_file("/tmp/freebsd-serial.sock");
                let _ = std::fs::remove_file(&qmp_sock);

                // Collect extra drives from the device list.
                let extra_drives: Vec<(String, String)> = {
                    let fb = freebsd_state.lock().unwrap();
                    fb.devices.iter()
                        .filter(|d| d.kind == "disk")
                        .enumerate()
                        .map(|(i, d)| {
                            let path = d.path.clone().unwrap_or_default();
                            let id   = format!("extra{}", i + 1);
                            (id, path)
                        })
                        .collect()
                };
                let extra_nics: Vec<(String, u16)> = {
                    let fb = freebsd_state.lock().unwrap();
                    fb.devices.iter()
                        .filter(|d| d.kind == "nic")
                        .enumerate()
                        .map(|(i, d)| (format!("netx{}", i + 1), d.port.unwrap_or(2230 + i as u16)))
                        .collect()
                };

                let mut args: Vec<String> = vec![
                    "-machine".into(), "virt,virtualization=on".into(),
                    "-cpu".into(),     "cortex-a57".into(),
                    "-m".into(),       "2G".into(),
                    "-nographic".into(),
                    "-bios".into(),    bios_path.clone(),
                    "-drive".into(),   format!("file={},format=raw,if=virtio", drive_path),
                    "-netdev".into(),  "user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22".into(),
                    "-device".into(),  "virtio-net-device,netdev=net0".into(),
                    "-device".into(),  "virtio-rng-device".into(),
                    "-serial".into(),  "unix:/tmp/freebsd-serial.sock,server=on,wait=off".into(),
                    "-qmp".into(),     format!("unix:{},server=on,wait=off", qmp_sock),
                ];
                for (id, path) in &extra_drives {
                    args.push("-drive".into());
                    args.push(format!("file={},format=raw,if=none,id={}", path, id));
                    args.push("-device".into());
                    args.push(format!("virtio-blk-device,drive={},id={}", id, id));
                }
                for (id, port) in &extra_nics {
                    args.push("-netdev".into());
                    args.push(format!("user,id={},hostfwd=tcp:127.0.0.1:{}-:22", id, port));
                    args.push("-device".into());
                    args.push(format!("virtio-net-device,netdev={},id={}", id, id));
                }

                {
                    let mut fb = freebsd_state.lock().unwrap();
                    fb.qmp_sock = Some(qmp_sock.clone());
                }

                let mut child = match std::process::Command::new("qemu-system-aarch64")
                    .args(&args)
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
                fb.qmp_sock = None;
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

                let qmp_sock = "/tmp/linux-qmp.sock".to_string();

                // Remove stale sockets so QEMU can create them fresh.
                let _ = std::fs::remove_file("/tmp/linux-serial.sock");
                let _ = std::fs::remove_file(&qmp_sock);

                // Collect extra drives/NICs from the device list.
                let extra_drives: Vec<(String, String)> = {
                    let lx = linux_state.lock().unwrap();
                    lx.devices.iter()
                        .filter(|d| d.kind == "disk")
                        .enumerate()
                        .map(|(i, d)| {
                            let path = d.path.clone().unwrap_or_default();
                            (format!("extra{}", i + 1), path)
                        })
                        .collect()
                };
                let extra_nics: Vec<(String, u16)> = {
                    let lx = linux_state.lock().unwrap();
                    lx.devices.iter()
                        .filter(|d| d.kind == "nic")
                        .enumerate()
                        .map(|(i, d)| (format!("netx{}", i + 1), d.port.unwrap_or(2240 + i as u16)))
                        .collect()
                };

                let mut args: Vec<String> = vec![
                    "-machine".into(), "virt,virtualization=on".into(),
                    "-cpu".into(),     "cortex-a57".into(),
                    "-m".into(),       "2G".into(),
                    "-nographic".into(),
                    "-bios".into(),    bios_path.clone(),
                    "-drive".into(),   format!("file={},format=raw,if=virtio", ubuntu_path),
                    "-drive".into(),   format!("file={},format=raw,if=virtio", seed_path),
                    "-netdev".into(),  "user,id=net0,hostfwd=tcp:127.0.0.1:2223-:22".into(),
                    "-device".into(),  "virtio-net-device,netdev=net0".into(),
                    "-device".into(),  "virtio-rng-device".into(),
                    "-serial".into(),  "unix:/tmp/linux-serial.sock,server=on,wait=off".into(),
                    "-qmp".into(),     format!("unix:{},server=on,wait=off", qmp_sock),
                ];
                for (id, path) in &extra_drives {
                    args.push("-drive".into());
                    args.push(format!("file={},format=raw,if=none,id={}", path, id));
                    args.push("-device".into());
                    args.push(format!("virtio-blk-device,drive={},id={}", id, id));
                }
                for (id, port) in &extra_nics {
                    args.push("-netdev".into());
                    args.push(format!("user,id={},hostfwd=tcp:127.0.0.1:{}-:22", id, port));
                    args.push("-device".into());
                    args.push(format!("virtio-net-device,netdev={},id={}", id, id));
                }

                {
                    let mut lx = linux_state.lock().unwrap();
                    lx.qmp_sock = Some(qmp_sock.clone());
                }

                let mut child = match std::process::Command::new("qemu-system-aarch64")
                    .args(&args)
                    .spawn()
                {
                    Ok(c) => c,
                    Err(e) => {
                        let mut lx = linux_state.lock().unwrap();
                        lx.phase    = LinuxVmPhase::Error;
                        lx.error    = Some(format!("Failed to launch qemu-system-aarch64: {}", e));
                        lx.qemu_pid = None;
                        lx.qmp_sock = None;
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
                lx.qmp_sock = None;
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

// ─── Console SSE stream ───────────────────────────────────────────────────────

/// Optional slot filter for the SSE stream.
#[derive(Deserialize)]
struct StreamQuery {
    /// If given, only emit lines from this slot.  Omit for all slots.
    slot: Option<usize>,
}

/// GET /api/agentos/console/stream
///
/// Server-Sent Events stream of serial log lines.  Each event is a JSON object:
///   data: {"slot": N, "line": "...", "cached": true}   ← historical replay
///   data: {"slot": N, "line": "..."}                   ← live
///   event: lag / data: {"lag": N}                      ← broadcast overrun
///
/// Optionally filter to one slot with ?slot=N.
async fn get_console_stream(
    State(s): State<BridgeState>,
    Query(q): Query<StreamQuery>,
) -> Sse<impl stream::Stream<Item = Result<Event, Infallible>>> {
    let filter_slot = q.slot;

    // Replay cached lines for the requested slot (or nothing for all-slots mode).
    let initial: Vec<Result<Event, Infallible>> = if let Some(slot) = filter_slot {
        if slot < MAX_SLOTS {
            let serial = s.serial.lock().unwrap();
            serial.lines[slot]
                .iter()
                .map(|line| {
                    let data = json!({ "slot": slot, "line": line, "cached": true }).to_string();
                    Ok(Event::default().data(data))
                })
                .collect()
        } else {
            vec![]
        }
    } else {
        vec![]
    };

    let rx = s.log_tx.subscribe();

    // Live stream: receives from broadcast, skips lines that don't match the filter.
    let live = stream::unfold(rx, move |mut rx| async move {
        loop {
            match rx.recv().await {
                Ok(LogBroadcast { slot, line }) => {
                    if filter_slot.map_or(false, |f| slot != f) {
                        continue;
                    }
                    let data = json!({ "slot": slot, "line": line }).to_string();
                    return Some((Ok::<Event, Infallible>(Event::default().data(data)), rx));
                }
                Err(tokio::sync::broadcast::error::RecvError::Lagged(n)) => {
                    let data = json!({ "lag": n }).to_string();
                    return Some((Ok(Event::default().event("lag").data(data)), rx));
                }
                Err(tokio::sync::broadcast::error::RecvError::Closed) => return None,
            }
        }
    });

    let combined = stream::iter(initial).chain(live);
    Sse::new(combined).keep_alive(KeepAlive::new().interval(Duration::from_secs(15)))
}

// ─── Console command injection ────────────────────────────────────────────────

#[derive(Deserialize)]
struct ConsoleCmdBody {
    /// Command string to send to console_shell (newline appended automatically).
    cmd: String,
}

/// POST /api/agentos/console/cmd
///
/// Injects a command into the seL4 console_shell PD by writing it to the
/// QEMU serial socket.  The console_shell PD reads the line and dispatches it.
///
/// Example body: {"cmd": "vm list"}
async fn post_console_cmd(
    State(s): State<BridgeState>,
    Json(body): Json<ConsoleCmdBody>,
) -> impl IntoResponse {
    let cmd = body.cmd.trim().to_string();
    if cmd.is_empty() {
        return Json(json!({ "ok": false, "error": "empty cmd" }));
    }
    let mut payload = cmd.clone();
    payload.push('\n');

    let guard = s.inject_tx.lock().unwrap();
    if let Some(tx) = guard.as_ref() {
        let _ = tx.send(payload.as_bytes().to_vec());
        Json(json!({ "ok": true, "cmd": cmd, "source": "socket" }))
    } else {
        Json(json!({ "ok": false, "cmd": cmd, "error": "serial socket not connected", "source": "no_socket" }))
    }
}

// ─── seL4 VM registry ─────────────────────────────────────────────────────────

/// GET /api/agentos/console/vms
///
/// Returns the lifecycle state of seL4-managed VMs as parsed from
/// \x01VM:start/stop:id escape sequences emitted by the console_shell PD.
/// These are distinct from the QEMU-hosted FreeBSD/Ubuntu VMs.
async fn get_sel4_vms(State(s): State<BridgeState>) -> impl IntoResponse {
    let vms = s.sel4_vms.lock().unwrap();
    let list: Vec<Value> = vms
        .iter()
        .map(|(id, state)| json!({ "id": id, "state": state }))
        .collect();
    Json(json!({ "vms": list, "source": "sel4_console" }))
}

// ─── Slots / console ─────────────────────────────────────────────────────────

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
