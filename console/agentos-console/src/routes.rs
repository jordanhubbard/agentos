//! Main HTTP routes for the agentOS console server.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::{
    extract::State,
    http::{header, StatusCode},
    response::{Html, IntoResponse},
    Json,
};
use serde_json::{json, Value};

use pd_slots::{PD_SLOTS, SLOT_NAMES, MAX_SLOTS};

use crate::profiler::generate_profiler_snapshot;
use crate::serial::SerialCache;

#[derive(Clone)]
pub struct AppState {
    pub serial:        Arc<Mutex<SerialCache>>,
    pub dashboard_html: Arc<String>,
    pub serial_path:   String,
    pub agentos_base:  String,
    pub agentos_token: String,
    /// Watch channel to request a re-parse
    pub parse_tx:      tokio::sync::watch::Sender<()>,
}

// ─── GET / ───────────────────────────────────────────────────────────────────

pub async fn get_dashboard(State(s): State<AppState>) -> impl IntoResponse {
    Html(s.dashboard_html.as_ref().clone())
}

// ─── GET /health ─────────────────────────────────────────────────────────────

pub async fn get_health(State(s): State<AppState>) -> impl IntoResponse {
    let serial = s.serial.lock().unwrap();
    let subscriber_counts: serde_json::Map<String, Value> = (0..MAX_SLOTS)
        .map(|i| (i.to_string(), json!(serial.lines[i].len())))
        .collect();
    drop(serial);
    Json(json!({
        "ok": true,
        "subscriberCounts": subscriber_counts,
    }))
}

// ─── GET /api/pd-slots ───────────────────────────────────────────────────────

pub async fn get_pd_slots() -> impl IntoResponse {
    let slots: Vec<Value> = PD_SLOTS.iter().map(|pd| {
        json!({
            "id":      pd.id,
            "name":    pd.name,
            "display": pd.display,
            "aliases": pd.aliases,
        })
    }).collect();
    Json(json!(slots))
}

// ─── GET /api/agentos/profiler/snapshot ──────────────────────────────────────

pub async fn get_profiler_snapshot() -> impl IntoResponse {
    Json(serde_json::to_value(generate_profiler_snapshot()).unwrap_or_default())
}

// ─── GET /api/debug ──────────────────────────────────────────────────────────

pub async fn get_debug(State(s): State<AppState>) -> impl IntoResponse {
    let mut probes: serde_json::Map<String, Value> = serde_json::Map::new();
    for slot in [0usize, 1, 2] {
        let url = format!("{}/api/agentos/console/{}?cursor=0", s.agentos_base, slot);
        match quick_get(&url, &s.agentos_token).await {
            Ok(val) => {
                let lines = val["lines"].as_array().map(|a| a.len()).unwrap_or(0);
                let sample = val["lines"].as_array()
                    .and_then(|a| a.first())
                    .cloned()
                    .unwrap_or(Value::Null);
                probes.insert(slot.to_string(), json!({ "ok": true, "lines": lines, "sample": sample }));
            }
            Err(e) => {
                probes.insert(slot.to_string(), json!({ "ok": false, "error": e }));
            }
        }
    }
    let slots_result = match quick_get(&format!("{}/api/agentos/slots", s.agentos_base), &s.agentos_token).await {
        Ok(v)  => v,
        Err(e) => json!({ "error": e }),
    };
    Json(json!({
        "agentosBase":    s.agentos_base,
        "slots":          slots_result,
        "consoleSamples": probes,
    }))
}

// ─── GET /api/debug/serial ───────────────────────────────────────────────────

pub async fn get_debug_serial(State(s): State<AppState>) -> impl IntoResponse {
    let serial = s.serial.lock().unwrap();
    // Collect last 40 lines from all slots
    let all_lines: Vec<String> = serial.lines.iter().flatten().cloned().collect();
    drop(serial);
    let start = all_lines.len().saturating_sub(40);
    let tail = all_lines[start..].join("\n");
    (
        [(header::CONTENT_TYPE, "text/plain; charset=utf-8")],
        if tail.is_empty() {
            format!("(no serial log at {})", s.serial_path)
        } else {
            tail
        },
    )
}

// ─── GET /api/agentos/slots ──────────────────────────────────────────────────

pub async fn get_agentos_slots(State(s): State<AppState>) -> impl IntoResponse {
    // Try bridge API first
    let bridge_url = format!("{}/api/agentos/slots", s.agentos_base);
    if let Ok(val) = quick_get(&bridge_url, &s.agentos_token).await {
        return Json(val);
    }

    // Fallback: serial cache
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

// ─── GET /api/agentos/agents ─────────────────────────────────────────────────

pub async fn get_agentos_agents(State(s): State<AppState>) -> impl IntoResponse {
    let url = format!("{}/api/agentos/agents", s.agentos_base);
    match quick_get(&url, &s.agentos_token).await {
        Ok(val)  => (StatusCode::OK, Json(val)),
        Err(e)   => (StatusCode::BAD_GATEWAY, Json(json!({ "error": e }))),
    }
}

// ─── POST /api/agentos/agents/spawn ──────────────────────────────────────────

pub async fn post_spawn_agent(
    State(s): State<AppState>,
    body: axum::body::Bytes,
) -> impl IntoResponse {
    let url = format!("{}/api/agentos/agents/spawn", s.agentos_base);
    let body_str = String::from_utf8_lossy(&body).into_owned();
    match quick_post(&url, &s.agentos_token, &body_str).await {
        Ok(val)  => (StatusCode::ACCEPTED, Json(val)),
        Err(e)   => (StatusCode::BAD_GATEWAY, Json(json!({ "error": e }))),
    }
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

/// Very small HTTP GET using raw tokio TCP (avoids needing reqwest).
pub async fn quick_get(url: &str, _token: &str) -> Result<Value, String> {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpStream;

    let without_scheme = url.strip_prefix("http://").ok_or("bad url")?;
    let slash_pos = without_scheme.find('/').ok_or("bad url")?;
    let host_port = &without_scheme[..slash_pos];
    let path_query = &without_scheme[slash_pos..];

    let mut stream = tokio::time::timeout(
        Duration::from_millis(1500),
        TcpStream::connect(host_port),
    ).await.map_err(|_| "timeout")?.map_err(|e| e.to_string())?;

    let request = format!(
        "GET {} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\n\r\n",
        path_query, host_port
    );
    stream.write_all(request.as_bytes()).await.map_err(|e| e.to_string())?;

    let mut response = String::new();
    stream.read_to_string(&mut response).await.map_err(|e| e.to_string())?;

    let body = response.splitn(2, "\r\n\r\n").nth(1).unwrap_or("");
    serde_json::from_str(body).map_err(|e| e.to_string())
}

pub async fn quick_post(url: &str, _token: &str, body: &str) -> Result<Value, String> {
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpStream;

    let without_scheme = url.strip_prefix("http://").ok_or("bad url")?;
    let slash_pos = without_scheme.find('/').ok_or("bad url")?;
    let host_port = &without_scheme[..slash_pos];
    let path = &without_scheme[slash_pos..];

    let request = format!(
        "POST {} HTTP/1.0\r\nHost: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
        path, host_port, body.len(), body
    );

    let mut stream = tokio::time::timeout(
        Duration::from_millis(2000),
        TcpStream::connect(host_port),
    ).await.map_err(|_| "timeout")?.map_err(|e| e.to_string())?;

    stream.write_all(request.as_bytes()).await.map_err(|e| e.to_string())?;

    let mut response = String::new();
    stream.read_to_string(&mut response).await.map_err(|e| e.to_string())?;

    let resp_body = response.splitn(2, "\r\n\r\n").nth(1).unwrap_or("");
    serde_json::from_str(resp_body).map_err(|e| e.to_string())
}
