//! agentOS Console Server — Rust port of agentos_console.mjs
//!
//! HTTP routes:
//!   GET  /                             → dashboard.html
//!   GET  /health                       → JSON health check
//!   GET  /api/pd-slots                 → PD slot definitions
//!   GET  /api/agentos/profiler/snapshot → mock profiler data
//!   GET  /api/agentos/slots            → slot list (proxy or serial fallback)
//!   GET  /api/agentos/agents           → proxy to bridge
//!   POST /api/agentos/agents/spawn     → proxy to bridge
//!   GET  /api/debug                    → probe bridge
//!   GET  /api/debug/serial             → last lines of serial log
//!
//! WebSocket routes:
//!   ws://.../ws             → log subscription bus
//!   ws://.../terminal/:N    → raw terminal I/O for slot N

mod bridge;
mod freebsd;
mod profiler;
mod routes;
mod serial;
mod ws_log;
mod ws_terminal;

use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::{
    routing::{get, post},
    Router,
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::time::interval;
use tower_http::cors::CorsLayer;
use tower_http::services::ServeDir;
use tracing::info;

use crate::bridge::{BridgeState, SharedSerial};
use crate::freebsd::new_shared;
use crate::serial::{SerialCache, parse_serial_log};
use crate::ws_log::{LogBroadcast, LogBroadcastTx, WsLogState};
use crate::ws_terminal::WsTerminalState;

// ─── Config ──────────────────────────────────────────────────────────────────

fn env_u16(name: &str, default: u16) -> u16 {
    std::env::var(name)
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(default)
}

fn env_str(name: &str, default: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| default.to_string())
}

// ─── Main ────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    // Initialise tracing
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::from_default_env()
                .add_directive("agentos_console=info".parse().unwrap()),
        )
        .init();

    // ── Config ────────────────────────────────────────────────────────────────
    let console_port: u16 = env_u16("CONSOLE_PORT", 8080);
    let bridge_port:  u16 = env_u16("BRIDGE_PORT",  8790);
    let agentos_host  = env_str("AGENTOS_HOST", "127.0.0.1");
    let agentos_api_port: u16 = std::env::var("AGENTOS_API_PORT")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(bridge_port);
    let agentos_base  = format!("http://{}:{}", agentos_host, agentos_api_port);
    let agentos_token = env_str("AGENTOS_TOKEN", "");
    let serial_log    = env_str("SERIAL_LOG",  "/tmp/agentos-serial.log");
    let serial_sock   = env_str("SERIAL_SOCK", "/tmp/agentos-serial.sock");

    if agentos_token.is_empty() {
        eprintln!("[console] WARNING: AGENTOS_TOKEN not set — agentOS API auth will be rejected");
    }

    // ── Shared state ─────────────────────────────────────────────────────────
    let serial_cache: SharedSerial = Arc::new(Mutex::new(SerialCache::new()));
    let freebsd_state = new_shared();

    // Resolve the Trunk-built dist/ directory (used both for index.html and ServeDir).
    let exe_dir = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()));

    let dist_dir: Option<std::path::PathBuf> = {
        let candidates: [Option<std::path::PathBuf>; 3] = [
            exe_dir.as_deref()
                .map(|d| d.join("../../../console/dashboard/dist"))
                .and_then(|p| p.canonicalize().ok()),
            Some(std::path::PathBuf::from("console/dashboard/dist")),
            Some(std::path::PathBuf::from("dashboard/dist")),
        ];
        candidates.into_iter().flatten().find(|p| p.is_dir())
    };

    // Dashboard HTML — prefer dist/index.html (Trunk output), fall back to legacy file.
    let dashboard_html: Arc<String> = Arc::new({
        let candidates: [Option<std::path::PathBuf>; 4] = [
            dist_dir.as_deref().map(|d| d.join("index.html")),
            exe_dir.as_deref()
                .map(|d| d.join("../../../console/dashboard.html"))
                .and_then(|p| p.canonicalize().ok()),
            Some(std::path::PathBuf::from("console/dashboard.html")),
            Some(std::path::PathBuf::from("dashboard.html")),
        ];
        candidates.into_iter()
            .flatten()
            .find(|p| p.exists())
            .and_then(|p| std::fs::read_to_string(&p).ok())
            .unwrap_or_else(|| {
                "<html><body>dashboard not found — run <code>trunk build</code> in console/dashboard/</body></html>".to_string()
            })
    });

    // ── Serial socket inject channel ─────────────────────────────────────────
    // Shared between bridge inject handler, ws_terminal, and the socket loop.
    let inject_tx_shared: Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<Vec<u8>>>>> =
        Arc::new(Mutex::new(None));

    // ── Broadcast channel for log lines ──────────────────────────────────────
    let (log_tx, _log_rx): (LogBroadcastTx, _) = tokio::sync::broadcast::channel(4096);

    // ── Watch channel to trigger serial re-parse ──────────────────────────────
    let (parse_tx, mut parse_rx) = tokio::sync::watch::channel(());

    // ── Background: serial log poller (every 100 ms) ─────────────────────────
    {
        let serial_cache_bg = serial_cache.clone();
        let serial_log_bg   = serial_log.clone();
        let log_tx_bg       = log_tx.clone();
        tokio::spawn(async move {
            let mut ticker = interval(Duration::from_millis(100));
            loop {
                tokio::select! {
                    _ = ticker.tick() => {}
                    result = parse_rx.changed() => {
                        if result.is_err() { break; }
                    }
                }
                let new_lines = {
                    let mut cache = serial_cache_bg.lock().unwrap();
                    parse_serial_log(&mut cache, &serial_log_bg)
                };
                for (slot, line) in new_lines {
                    let _ = log_tx_bg.send(LogBroadcast { slot, line });
                }
            }
        });
    }

    // ── Background: QEMU serial socket connector ──────────────────────────────
    {
        let serial_cache_sock = serial_cache.clone();
        let log_tx_sock       = log_tx.clone();
        let inject_tx_sock    = inject_tx_shared.clone();
        let serial_sock_path  = serial_sock.clone();
        let serial_log_path   = serial_log.clone();
        tokio::spawn(async move {
            loop {
                match UnixStream::connect(&serial_sock_path).await {
                    Ok(stream) => {
                        info!("[console] connected to QEMU serial socket (bidirectional)");

                        let (mut reader, mut writer) = stream.into_split();

                        // Channel for bytes to write to the socket
                        let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();
                        *inject_tx_sock.lock().unwrap() = Some(tx);

                        // Writer task
                        tokio::spawn(async move {
                            while let Some(bytes) = rx.recv().await {
                                if writer.write_all(&bytes).await.is_err() {
                                    break;
                                }
                            }
                        });

                        // Reader loop
                        let mut line_buf = String::new();
                        let mut seen_pd  = false;
                        let mut buf = [0u8; 4096];
                        loop {
                            match reader.read(&mut buf).await {
                                Ok(0) | Err(_) => break,
                                Ok(n) => {
                                    let raw_str = String::from_utf8_lossy(&buf[..n]).into_owned();

                                    // Write through to log file (best effort)
                                    if let Ok(mut f) = tokio::fs::OpenOptions::new()
                                        .create(true).append(true)
                                        .open(&serial_log_path).await
                                    {
                                        let _ = f.write_all(raw_str.as_bytes()).await;
                                    }

                                    // Line-buffer for cache
                                    line_buf.push_str(&raw_str);
                                    while let Some(nl_pos) = line_buf.find('\n') {
                                        let raw_line = line_buf[..nl_pos].to_string();
                                        line_buf = line_buf[nl_pos + 1..].to_string();

                                        if let Some(routed) = serial::route_log_line(&raw_line, seen_pd) {
                                            seen_pd = routed.seen_pd;
                                            let is_new = {
                                                let mut cache = serial_cache_sock.lock().unwrap();
                                                cache.add_line(routed.slot, routed.line.clone())
                                            };
                                            if is_new {
                                                let _ = log_tx_sock.send(LogBroadcast {
                                                    slot: routed.slot,
                                                    line: routed.line,
                                                });
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        *inject_tx_sock.lock().unwrap() = None;
                        info!("[console] QEMU serial socket disconnected, retrying in 1s");
                    }
                    Err(_) => {} // socket not yet available
                }
                tokio::time::sleep(Duration::from_secs(1)).await;
            }
        });
    }

    // ── Main HTTP router ──────────────────────────────────────────────────────
    // Share the same guest_img_dir path with AppState
    let guest_img_dir_for_state = {
        let exe = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|d| d.to_path_buf()));
        exe.map(|d| d.join("../../..").join("guest-images"))
            .and_then(|p| p.canonicalize().ok())
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_else(|| "guest-images".to_string())
    };

    let app_state = routes::AppState {
        serial:         serial_cache.clone(),
        dashboard_html: dashboard_html.clone(),
        serial_path:    serial_log.clone(),
        agentos_base:   agentos_base.clone(),
        agentos_token:  agentos_token.clone(),
        guest_img_dir:  guest_img_dir_for_state,
        parse_tx:       parse_tx.clone(),
    };

    let ws_log_state = WsLogState {
        serial:        serial_cache.clone(),
        broadcast:     log_tx.clone(),
        agentos_base:  agentos_base.clone(),
        agentos_token: agentos_token.clone(),
    };

    let ws_terminal_state = WsTerminalState {
        serial:        serial_cache.clone(),
        agentos_base:  agentos_base.clone(),
        agentos_token: agentos_token.clone(),
        inject_tx:     inject_tx_shared.clone(),
    };

    // Build axum app.  WS handlers need their state baked in via closures
    // because axum's .with_state() only applies to a single state type.
    let ws_log_state_for_route    = ws_log_state.clone();
    let ws_term_state_for_route   = ws_terminal_state.clone();

    let app = Router::new()
        // Dashboard
        .route("/",          get(routes::get_dashboard))
        .route("/dashboard", get(routes::get_dashboard))
        // Health
        .route("/health",    get(routes::get_health))
        // PD slots
        .route("/api/pd-slots", get(routes::get_pd_slots))
        // Profiler
        .route("/api/agentos/profiler/snapshot", get(routes::get_profiler_snapshot))
        // Slots / agents
        .route("/api/agentos/slots",         get(routes::get_agentos_slots))
        .route("/api/agentos/agents",        get(routes::get_agentos_agents))
        .route("/api/agentos/agents/spawn",  post(routes::post_spawn_agent))
        // Images + topology
        .route("/api/images",   get(routes::get_images))
        .route("/api/topology", get(routes::get_topology))
        // Debug
        .route("/api/debug",        get(routes::get_debug))
        .route("/api/debug/serial", get(routes::get_debug_serial))
        // WebSocket — log bus
        .route("/ws", get({
            let s = ws_log_state_for_route;
            move |ws: axum::extract::WebSocketUpgrade| {
                let state = s.clone();
                async move {
                    ws.on_upgrade(move |socket| ws_log::handle_log_ws(socket, state))
                }
            }
        }))
        // WebSocket — terminal tiles
        .route("/terminal/:slot", get({
            let s = ws_term_state_for_route;
            move |ws: axum::extract::WebSocketUpgrade,
                  path: axum::extract::Path<String>| {
                let state = s.clone();
                async move {
                    let slot: usize = path.0.parse().unwrap_or(usize::MAX);
                    ws.on_upgrade(move |socket| {
                        ws_terminal::handle_terminal_ws(socket, state, slot)
                    })
                }
            }
        }))
        .layer(CorsLayer::permissive())
        .with_state(app_state);

    // Serve Trunk-compiled WASM assets as a fallback (wasm, js glue, css hashes).
    // Explicit API/WS routes above always take priority.
    let app = if let Some(ref dist) = dist_dir {
        info!("[console] serving WASM bundle from {}", dist.display());
        app.fallback_service(ServeDir::new(dist))
    } else {
        info!("[console] no dist/ found — run `trunk build` in console/dashboard/");
        app
    };

    // ── Bridge HTTP router ────────────────────────────────────────────────────
    let guest_img_dir = {
        let exe_dir = std::env::current_exe()
            .ok()
            .and_then(|p| p.parent().map(|d| d.to_path_buf()));
        exe_dir
            .map(|d| d.join("../../..").join("guest-images"))
            .and_then(|p| p.canonicalize().ok())
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_else(|| "guest-images".to_string())
    };

    let bridge_state = BridgeState {
        serial:        serial_cache.clone(),
        freebsd:       freebsd_state.clone(),
        _serial_path:  serial_log.clone(),
        guest_img_dir,
        freebsd_ver:   "14.3".to_string(),
        parse_tx:      parse_tx.clone(),
        inject_tx:     inject_tx_shared.clone(),
    };

    let bridge_router = bridge::build_router(bridge_state)
        .layer(CorsLayer::permissive());

    // ── Bind and serve ────────────────────────────────────────────────────────
    let console_addr: SocketAddr = format!("0.0.0.0:{}", console_port).parse()?;
    let bridge_addr:  SocketAddr = format!("127.0.0.1:{}", bridge_port).parse()?;

    let console_listener = tokio::net::TcpListener::bind(console_addr).await?;
    let bridge_listener  = tokio::net::TcpListener::bind(bridge_addr).await?;

    info!("[console] agentOS console listening on http://0.0.0.0:{}", console_port);
    info!("[console] dashboard: http://127.0.0.1:{}/", console_port);
    info!("[console] bridge:    http://127.0.0.1:{}/", bridge_port);

    tokio::select! {
        result = axum::serve(console_listener, app) => {
            result?;
        }
        result = axum::serve(bridge_listener, bridge_router) => {
            result?;
        }
    }

    Ok(())
}
