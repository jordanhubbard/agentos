/*!
 * agentOS Mesh Bridge — SquirrelBus ↔ seL4 mesh_agent relay
 *
 * Runs on the host Linux side of each agentOS node.
 * Bridges the seL4 mesh_agent PD (which can't make HTTP calls) to the
 * SquirrelBus REST API on do-host1 (100.89.199.14:8788).
 *
 * Architecture:
 *   seL4 mesh_agent PD
 *       ↕ (shared memory / doorbell, via microkit virtio or simulated IPC)
 *   [mesh-bridge userspace daemon]
 *       ↕ HTTP
 *   SquirrelBus (POST /bus/send, GET /bus/messages, SSE /bus/stream)
 *
 * In QEMU/simulation mode: uses a Unix socket at /tmp/agentos-mesh.sock
 * In production (hardware): uses the VirtIO console or shared memory region
 *
 * Message format on the socket (newline-delimited JSON):
 *   {"tag":2561,"mr":[8,4,4,0,0,28265,26994,0]}
 *   tag = MsgTag (MSG_MESH_ANNOUNCE = 0x0A01 = 2561)
 *   mr  = message registers [MR0..MR7]
 */

use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

// ── Config ────────────────────────────────────────────────────────────────────

const SQUIRRELBUS_URL: &str = "http://100.89.199.14:8788";
const SQUIRRELBUS_TOKEN: &str = "wq-5dcad756f6d3e345c00b5cb3dfcbdedb";
const BRIDGE_LISTEN:  &str = "127.0.0.1:18800";
const ANNOUNCE_INTERVAL_S: u64 = 30;

// agentOS node identity (override via AGENTOS_NODE_ID env)
fn node_id() -> String {
    std::env::var("AGENTOS_NODE_ID")
        .unwrap_or_else(|_| hostname::get().ok()
            .and_then(|h| h.into_string().ok())
            .unwrap_or_else(|| "agentOS-node".into()))
}

// ── Message tags (mirror of agentos.h) ───────────────────────────────────────

const MSG_MESH_ANNOUNCE:  u32 = 0x0A01;
const MSG_MESH_HEARTBEAT: u32 = 0x0A07;
const MSG_REMOTE_SPAWN:   u32 = 0x0A05;

// ── SquirrelBus HTTP helpers ──────────────────────────────────────────────────

/// Send a message to the SquirrelBus mesh channel.
fn bus_send(from: &str, to: &str, msg_type: &str, payload: &str) {
    let body = format!(
        r#"{{"from":"{from}","to":"{to}","type":"{msg_type}","payload":{payload}}}"#,
    );
    let url = format!("{SQUIRRELBUS_URL}/bus/send");
    // Use a simple blocking HTTP POST (no async needed here)
    let output = std::process::Command::new("curl")
        .args([
            "-s", "-X", "POST", &url,
            "-H", "Content-Type: application/json",
            "-H", &format!("Authorization: Bearer {SQUIRRELBUS_TOKEN}"),
            "-d", &body,
            "--max-time", "5",
        ])
        .output();
    if let Err(e) = output {
        eprintln!("[mesh-bridge] bus_send failed: {e}");
    }
}

/// Poll the SquirrelBus for messages addressed to this node.
fn bus_poll(node: &str) -> Vec<serde_json::Value> {
    let url = format!("{SQUIRRELBUS_URL}/bus/messages");
    let output = std::process::Command::new("curl")
        .args([
            "-s", &url,
            "-H", &format!("Authorization: Bearer {SQUIRRELBUS_TOKEN}"),
            "--max-time", "5",
        ])
        .output()
        .ok()
        .and_then(|o| String::from_utf8(o.stdout).ok());

    let raw = output.unwrap_or_default();
    let all: Vec<serde_json::Value> = serde_json::from_str(&raw).unwrap_or_default();

    // Filter to messages addressed to us (or broadcast "all")
    all.into_iter().filter(|m| {
        let to = m["to"].as_str().unwrap_or("");
        to == node || to == "all" || to == "mesh"
    }).collect()
}

// ── Main ─────────────────────────────────────────────────────────────────────

fn main() {
    let node = node_id();
    eprintln!("[mesh-bridge] Starting for node: {}", node);
    eprintln!("[mesh-bridge] SquirrelBus: {}", SQUIRRELBUS_URL);
    eprintln!("[mesh-bridge] Listening on: {}", BRIDGE_LISTEN);

    // Announce this node to the mesh
    announce_self(&node);

    // Spawn background thread for periodic announce + heartbeat
    let node_bg = node.clone();
    std::thread::spawn(move || {
        loop {
            std::thread::sleep(Duration::from_secs(ANNOUNCE_INTERVAL_S));
            announce_self(&node_bg);
            // Also poll for incoming mesh messages
            let messages = bus_poll(&node_bg);
            for msg in messages {
                handle_bus_message(&msg, &node_bg);
            }
        }
    });

    // TCP listener for seL4 mesh_agent IPC relay
    let listener = match TcpListener::bind(BRIDGE_LISTEN) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("[mesh-bridge] bind failed: {e}");
            std::process::exit(1);
        }
    };
    eprintln!("[mesh-bridge] Ready");

    for stream in listener.incoming() {
        let Ok(stream) = stream else { continue };
        let node = node.clone();
        std::thread::spawn(move || {
            let reader = BufReader::new(&stream);
            for line in reader.lines() {
                let Ok(line) = line else { break };
                handle_ipc_message(&line, &node);
            }
        });
    }
}

/// Announce this node's capabilities to the SquirrelBus mesh.
fn announce_self(node: &str) {
    // Read local slot stats from agentOS status endpoint (if available)
    let worker_total = std::env::var("AGENTOS_WORKER_SLOTS")
        .ok().and_then(|v| v.parse::<u32>().ok()).unwrap_or(8);
    let gpu_total = std::env::var("AGENTOS_GPU_SLOTS")
        .ok().and_then(|v| v.parse::<u32>().ok()).unwrap_or(4);

    let payload = format!(
        r#"{{"node_id":"{node}","worker_slots_total":{worker_total},"worker_slots_free":{worker_total},"gpu_slots_total":{gpu_total},"gpu_slots_free":{gpu_total},"arch":"{}"}}"#,
        std::env::consts::ARCH,
    );
    bus_send(node, "mesh", "agent_mesh_announce", &payload);
}

/// Handle a message received from the SquirrelBus (bus_poll).
fn handle_bus_message(msg: &serde_json::Value, node: &str) {
    let msg_type = msg["type"].as_str().unwrap_or("");
    match msg_type {
        "agent_mesh_announce" => {
            // Another node announced itself — we'd relay to seL4 mesh_agent
            // via the TCP bridge socket (wire format: tag + MRs as JSON)
            let payload = &msg["payload"];
            let peer_id = payload["node_id"].as_str().unwrap_or("?");
            if peer_id != node {
                eprintln!("[mesh-bridge] Peer announced: {}", peer_id);
                // TODO: relay to seL4 mesh_agent via TCP socket
            }
        }
        "remote_spawn" => {
            let payload = &msg["payload"];
            let target = payload["target_node"].as_str().unwrap_or("");
            if target == node {
                eprintln!("[mesh-bridge] Remote spawn request received");
                // TODO: relay to seL4 init_agent spawn handler via TCP
            }
        }
        _ => {}
    }
}

/// Handle an IPC message from the seL4 mesh_agent (via TCP bridge socket).
fn handle_ipc_message(line: &str, node: &str) {
    let Ok(msg) = serde_json::from_str::<serde_json::Value>(line) else { return };
    let tag = msg["tag"].as_u64().unwrap_or(0) as u32;
    let mrs = msg["mr"].as_array().cloned().unwrap_or_default();

    match tag {
        t if t == MSG_MESH_ANNOUNCE => {
            // seL4 mesh_agent wants to announce itself
            announce_self(node);
        }
        t if t == MSG_MESH_HEARTBEAT => {
            // seL4 wants to send a heartbeat
            let payload = format!(r#"{{"node_id":"{node}"}}"#);
            bus_send(node, "mesh", "agent_mesh_heartbeat", &payload);
        }
        t if t == MSG_REMOTE_SPAWN => {
            // seL4 wants to spawn on a peer
            let target_lo = mrs.get(5).and_then(|v| v.as_u64()).unwrap_or(0) as u32;
            let target_hi = mrs.get(6).and_then(|v| v.as_u64()).unwrap_or(0) as u32;
            // Unpack target node_id
            let mut target_id = [0u8; 9];
            for i in 0..4 { target_id[i] = ((target_lo >> (i*8)) & 0xFF) as u8; }
            for i in 0..4 { target_id[4+i] = ((target_hi >> (i*8)) & 0xFF) as u8; }
            let target_node = std::str::from_utf8(&target_id).unwrap_or("?").trim_matches('\0');

            let hash_lo = mrs.get(1).and_then(|v| v.as_u64()).unwrap_or(0);
            let hash_hi = mrs.get(3).and_then(|v| v.as_u64()).unwrap_or(0);
            let priority = mrs.get(4).and_then(|v| v.as_u64()).unwrap_or(50);
            let flags = mrs.get(5).and_then(|v| v.as_u64()).unwrap_or(0);

            let payload = format!(
                r#"{{"target_node":"{target_node}","wasm_hash_lo":{hash_lo},"wasm_hash_hi":{hash_hi},"priority":{priority},"flags":{flags},"from_node":"{node}"}}"#,
            );
            bus_send(node, target_node, "remote_spawn", &payload);
            eprintln!("[mesh-bridge] Forwarded remote_spawn → {}", target_node);
        }
        _ => {
            eprintln!("[mesh-bridge] Unknown IPC tag: 0x{:04X}", tag);
        }
    }
}
