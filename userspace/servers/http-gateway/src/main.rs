//! agentOS HTTP Gateway
//!
//! A Tokio/hyper HTTP/1.1 server that acts as the public-facing HTTP entry
//! point for agentOS full-stack applications.  Each incoming request is
//! dispatched to the appropriate application by querying the IPC bridge
//! (GATEWAY_IPC) via HTTP POST, with a static per-app env-var route table as
//! fallback.
//!
//! Architecture:
//!   Internet → [http-gateway] → POST /dispatch → [IPC bridge]
//!                                                 → {"addr": "127.0.0.1:PORT"}
//!   [http-gateway] → proxy request to app's local socket
//!
//!   Fallback: GATEWAY_ROUTE_<APPNAME>=host:port env vars
//!
//! Configuration (environment variables):
//!   GATEWAY_LISTEN        — bind address (default: "0.0.0.0:8080")
//!   GATEWAY_IPC           — IPC bridge address (default: "127.0.0.1:9090")
//!   GATEWAY_ROUTES        — comma-separated prefix=upstream pairs
//!                           e.g. "/api=127.0.0.1:9090,/app=127.0.0.1:9091"
//!   GATEWAY_ROUTE_<NAME>  — per-app static override, e.g. GATEWAY_ROUTE_FOO=127.0.0.1:9200
//!   RUST_LOG              — tracing filter (default: "info")

use std::collections::HashMap;
use std::convert::Infallible;
use std::net::SocketAddr;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use bytes::Bytes;
use http_body_util::{combinators::BoxBody, BodyExt, Full};
use hyper::body::Incoming;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Method, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use serde_json::json;
use tokio::net::TcpListener;
use tracing::{debug, error, info, warn};

// ── HTTP opcodes (must match http_svc.h) ────────────────────────────────────
const OP_HTTP_DISPATCH: u32 = 0x92;
const HTTP_APP_ID_NONE: u32 = 0xFFFF_FFFF;

// ── IPC result codes ─────────────────────────────────────────────────────────
const HTTP_OK: u32 = 0;

// ── Global request counter for X-Request-Id ─────────────────────────────────
static REQUEST_COUNTER: AtomicU64 = AtomicU64::new(1);

// ── Route table (path prefix → upstream address) ─────────────────────────────
static ROUTE_TABLE: OnceLock<HashMap<String, String>> = OnceLock::new();

/// Parse GATEWAY_ROUTES env var into a prefix→upstream map.
///
/// Format: "/api=127.0.0.1:9090,/app=127.0.0.1:9091"
fn init_route_table() -> HashMap<String, String> {
    let mut map = HashMap::new();
    let raw = std::env::var("GATEWAY_ROUTES").unwrap_or_default();
    if raw.is_empty() {
        return map;
    }
    for entry in raw.split(',') {
        let entry = entry.trim();
        if let Some((prefix, upstream)) = entry.split_once('=') {
            let prefix = prefix.trim().to_string();
            let upstream = upstream.trim().to_string();
            if !prefix.is_empty() && !upstream.is_empty() {
                info!("route: {} → {}", prefix, upstream);
                map.insert(prefix, upstream);
            }
        }
    }
    map
}

/// Find the longest prefix match for `path` in the static GATEWAY_ROUTES table.
fn route_lookup(path: &str) -> Option<String> {
    let table = ROUTE_TABLE.get().expect("route table not initialised");
    let mut best: Option<(&str, &str)> = None;
    for (prefix, upstream) in table.iter() {
        if path.starts_with(prefix.as_str()) {
            let is_longer = best.map_or(true, |(bp, _)| prefix.len() > bp.len());
            if is_longer {
                best = Some((prefix.as_str(), upstream.as_str()));
            }
        }
    }
    best.map(|(_, upstream)| upstream.to_string())
}

/// Per-app static route from `GATEWAY_ROUTE_<APPNAME>` env vars.
///
/// For a path like `/app/foo/bar` the app name is `foo` and the env var is
/// `GATEWAY_ROUTE_FOO`.  Returns `None` if the env var is absent or the value
/// cannot be parsed as a `SocketAddr`.
fn static_route(path: &str) -> Option<SocketAddr> {
    let app = path.split('/').nth(2)?;
    let key = format!("GATEWAY_ROUTE_{}", app.to_uppercase());
    let addr_str = std::env::var(&key).ok()?;
    addr_str.parse().ok()
}

/// Query the IPC bridge for which socket address handles `path`.
///
/// POSTs `{"path": path}` to `http://{GATEWAY_IPC}/dispatch` and expects a
/// JSON response of the form `{"app_id": N, "addr": "127.0.0.1:PORT"}`.
/// Returns `None` on any network or parse failure, or when the bridge signals
/// no route (`{"error": "..."}`).
async fn dispatch_via_http_svc(path: &str) -> Option<SocketAddr> {
    // Keep the legacy constants referenced so they remain reachable.
    let _ = (OP_HTTP_DISPATCH, HTTP_OK, HTTP_APP_ID_NONE);

    let bridge_addr = std::env::var("GATEWAY_IPC")
        .unwrap_or_else(|_| "127.0.0.1:9090".to_string());

    let url = format!("http://{}/dispatch", bridge_addr);

    let client = reqwest::Client::new();
    let resp = match client
        .post(&url)
        .json(&json!({ "path": path }))
        .send()
        .await
    {
        Ok(r) => r,
        Err(e) => {
            debug!("dispatch_via_http_svc: POST {} failed: {}", url, e);
            return None;
        }
    };

    if !resp.status().is_success() {
        debug!(
            "dispatch_via_http_svc: bridge returned HTTP {}",
            resp.status()
        );
        return None;
    }

    let body: serde_json::Value = match resp.json().await {
        Ok(v) => v,
        Err(e) => {
            debug!("dispatch_via_http_svc: failed to decode JSON: {}", e);
            return None;
        }
    };

    if let Some(err_msg) = body.get("error").and_then(|v| v.as_str()) {
        debug!("dispatch_via_http_svc: bridge error: {}", err_msg);
        return None;
    }

    let addr_str = body.get("addr")?.as_str()?;
    let addr: SocketAddr = addr_str.parse().ok()?;
    Some(addr)
}

// ── Response helpers ─────────────────────────────────────────────────────────

fn json_response(status: StatusCode, body: serde_json::Value) -> Response<BoxBody<Bytes, Infallible>> {
    let json_bytes = body.to_string();
    Response::builder()
        .status(status)
        .header("Content-Type", "application/json")
        .body(
            Full::new(Bytes::from(json_bytes))
                .map_err(|e| match e {})
                .boxed(),
        )
        .unwrap()
}

/// Error response body for proxy failures — uses Infallible body.
fn error_response(status: StatusCode, msg: &str) -> Response<BoxBody<Bytes, Infallible>> {
    json_response(status, json!({ "error": msg }))
}

// ── Real HTTP proxy ──────────────────────────────────────────────────────────

/// Proxy an HTTP request to `upstream` ("host:port") and return the response
/// with a `BoxBody<Bytes, hyper::Error>`.
async fn proxy_request(
    upstream: &str,
    req: Request<Incoming>,
) -> Result<Response<BoxBody<Bytes, hyper::Error>>, hyper::Error> {
    use hyper::client::conn::http1::handshake;
    use tokio::net::TcpStream;

    let stream = match TcpStream::connect(upstream).await {
        Ok(s) => s,
        Err(e) => {
            warn!("proxy: cannot connect to {}: {}", upstream, e);
            // Return a 502 wrapped as a hyper::Error is not possible without
            // an actual hyper error — so we return an empty body 502.
            let resp: Response<BoxBody<Bytes, hyper::Error>> = Response::builder()
                .status(StatusCode::BAD_GATEWAY)
                .header("Content-Type", "application/json")
                .body(
                    Full::new(Bytes::from(
                        json!({"error": "bad_gateway", "upstream": upstream}).to_string(),
                    ))
                    .map_err(|e| match e {})
                    .boxed(),
                )
                .unwrap();
            // We cannot return an Err(hyper::Error) for a TCP connect failure,
            // so return Ok with 502.
            return Ok(resp);
        }
    };

    let (mut sender, conn) = handshake(TokioIo::new(stream)).await?;
    tokio::spawn(async move {
        if let Err(e) = conn.await {
            debug!("proxy connection closed: {}", e);
        }
    });

    sender.send_request(req).await.map(|r| r.map(|b| b.boxed()))
}

// ── Request handler ──────────────────────────────────────────────────────────

async fn handle_request(
    req: Request<Incoming>,
) -> Result<Response<BoxBody<Bytes, Infallible>>, Infallible> {
    let start    = Instant::now();
    let method   = req.method().clone();
    let path     = req.uri().path().to_owned();
    let req_id   = REQUEST_COUNTER.fetch_add(1, Ordering::Relaxed);

    debug!("[req-{}] {} {}", req_id, method, path);

    // ── Built-in gateway endpoints ───────────────────────────────────────────

    if method == Method::GET && (path == "/health" || path == "/healthz" || path == "/_gateway/health") {
        let route_count = ROUTE_TABLE
            .get()
            .map(|t| t.len())
            .unwrap_or(0);
        let resp = json_response(
            StatusCode::OK,
            json!({ "status": "ok", "routes": route_count }),
        );
        info!(
            "[req-{}] GET {} → 200 ({}ms)",
            req_id, path, start.elapsed().as_millis()
        );
        return Ok(resp);
    }

    if method == Method::GET && path == "/_gateway/routes" {
        let routes: Vec<serde_json::Value> = ROUTE_TABLE
            .get()
            .map(|t| {
                t.iter()
                    .map(|(prefix, upstream)| json!({ "prefix": prefix, "upstream": upstream }))
                    .collect()
            })
            .unwrap_or_default();
        return Ok(json_response(StatusCode::OK, json!({ "routes": routes })));
    }

    // ── IPC dispatch via http_svc bridge (primary) ───────────────────────────
    let upstream: Option<String> = if let Some(addr) = dispatch_via_http_svc(&path).await {
        info!(
            "[req-{}] ipc-dispatch: {} → {} ({}ms)",
            req_id, path, addr, start.elapsed().as_millis()
        );
        Some(addr.to_string())
    } else {
        debug!("[req-{}] ipc-dispatch: no route for {}", req_id, path);

        // ── Fallback 1: GATEWAY_ROUTE_<APPNAME> per-app env vars ────────────
        if let Some(addr) = static_route(&path) {
            info!(
                "[req-{}] static-route (env): {} → {} ({}ms)",
                req_id, path, addr, start.elapsed().as_millis()
            );
            Some(addr.to_string())
        } else {
            // ── Fallback 2: GATEWAY_ROUTES prefix table ──────────────────────
            if let Some(up) = route_lookup(&path) {
                info!(
                    "[req-{}] static-route (prefix): {} → {} ({}ms)",
                    req_id, path, up, start.elapsed().as_millis()
                );
                Some(up)
            } else {
                None
            }
        }
    };

    if let Some(upstream_addr) = upstream {
        // Re-attach X-Request-Id header before forwarding.
        let (mut parts, body) = req.into_parts();
        parts.headers.insert(
            "x-request-id",
            format!("{}", req_id).parse().unwrap(),
        );
        let req = Request::from_parts(parts, body);

        match proxy_request(&upstream_addr, req).await {
            Ok(upstream_resp) => {
                let status = upstream_resp.status();
                info!(
                    "[req-{}] {} {} → {} via {} ({}ms)",
                    req_id, method, path, status, upstream_addr, start.elapsed().as_millis()
                );
                let (parts, body) = upstream_resp.into_parts();
                let infallible_body = body.map_err(|_| -> Infallible { unreachable!() }).boxed();
                return Ok(Response::from_parts(parts, infallible_body));
            }
            Err(e) => {
                error!("[req-{}] proxy error: {}", req_id, e);
                let resp = error_response(StatusCode::BAD_GATEWAY, "upstream error");
                info!(
                    "[req-{}] {} {} → 502 ({}ms)",
                    req_id, method, path, start.elapsed().as_millis()
                );
                return Ok(resp);
            }
        }
    }

    // ── No route found ───────────────────────────────────────────────────────
    if path != "/favicon.ico" {
        warn!("[req-{}] no handler for {} {}", req_id, method, path);
    }
    info!(
        "[req-{}] {} {} → 404 ({}ms)",
        req_id, method, path, start.elapsed().as_millis()
    );
    Ok(json_response(
        StatusCode::NOT_FOUND,
        json!({
            "error": "no_handler",
            "path": path,
            "hint": "register an app via OP_APP_LAUNCH with an http_prefix, set GATEWAY_IPC, or set GATEWAY_ROUTES / GATEWAY_ROUTE_<APPNAME>"
        }),
    ))
}

// ── Main ─────────────────────────────────────────────────────────────────────

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Initialise tracing
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("info")),
        )
        .init();

    // Initialise route table from GATEWAY_ROUTES env var
    ROUTE_TABLE.get_or_init(init_route_table);

    let listen_addr: SocketAddr = std::env::var("GATEWAY_LISTEN")
        .unwrap_or_else(|_| "0.0.0.0:8080".to_string())
        .parse()
        .expect("GATEWAY_LISTEN must be a valid socket address");

    let ipc_addr = std::env::var("GATEWAY_IPC")
        .unwrap_or_else(|_| "127.0.0.1:9090".to_string());

    let route_count = ROUTE_TABLE.get().map(|t| t.len()).unwrap_or(0);
    info!("agentOS HTTP gateway starting on {}", listen_addr);
    info!("IPC bridge: http://{}/dispatch", ipc_addr);
    info!("route table: {} entries", route_count);

    let listener = TcpListener::bind(listen_addr).await?;
    info!("listening on {}", listen_addr);

    loop {
        let (stream, peer) = listener.accept().await?;
        debug!("accepted connection from {}", peer);

        let io = TokioIo::new(stream);
        tokio::spawn(async move {
            if let Err(e) = http1::Builder::new()
                .serve_connection(io, service_fn(handle_request))
                .await
            {
                error!("connection error from {}: {}", peer, e);
            }
        });
    }
}
