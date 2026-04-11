//! agentOS HTTP Gateway
//!
//! A Tokio/hyper HTTP/1.1 server that acts as the public-facing HTTP entry
//! point for agentOS full-stack applications.  Each incoming request is
//! dispatched to the appropriate application by querying the http_svc PD
//! (kernel/agentos-root-task/src/http_svc.c) via the agentOS SDK IPC layer.
//!
//! Architecture:
//!   Internet → [http-gateway] → IPC: OP_HTTP_DISPATCH → [http_svc PD]
//!                                                        → matched app_id
//!   [http-gateway] → proxy request to app's local socket
//!
//! Configuration (environment variables):
//!   GATEWAY_LISTEN  — bind address (default: "0.0.0.0:8080")
//!   GATEWAY_IPC     — path to agentOS IPC socket (default: "/run/agentos.sock")
//!   GATEWAY_ROUTES  — comma-separated prefix=upstream pairs
//!                     e.g. "/api=127.0.0.1:9090,/app=127.0.0.1:9091"
//!   RUST_LOG        — tracing filter (default: "info")

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

/// Find the longest prefix match for `path` in the route table.
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

// ── Dispatch result returned by the (stubbed) http_svc query ────────────────
#[derive(Debug)]
struct DispatchResult {
    app_id:     u32,
    vnic_id:    u32,
    handler_id: u32,
}

/// Query the http_svc PD for which app handles the given URL path.
///
/// MVP stub: always returns "no match" since the seL4 IPC bridge is not yet
/// wired.  Replace the body of this function with real IPC once the bridge
/// is available.
async fn dispatch_via_http_svc(path: &str) -> Option<DispatchResult> {
    // TODO: encode path into 8 × u64 MRs and call OP_HTTP_DISPATCH over the
    //       agentOS IPC socket.  For now return None (no match).
    let _ = (path, OP_HTTP_DISPATCH, HTTP_OK, HTTP_APP_ID_NONE);
    None
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

    // ── Route table lookup (in-process proxy) ────────────────────────────────
    if let Some(upstream) = route_lookup(&path) {
        debug!("[req-{}] route match: {} → {}", req_id, path, upstream);

        // Re-attach X-Request-Id header before forwarding.
        let (mut parts, body) = req.into_parts();
        parts.headers.insert(
            "x-request-id",
            format!("{}", req_id).parse().unwrap(),
        );
        let req = Request::from_parts(parts, body);

        match proxy_request(&upstream, req).await {
            Ok(upstream_resp) => {
                let status = upstream_resp.status();
                info!(
                    "[req-{}] {} {} → {} via {} ({}ms)",
                    req_id, method, path, status, upstream, start.elapsed().as_millis()
                );
                // Re-box with Infallible error type
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

    // ── IPC dispatch via http_svc (stubbed) ──────────────────────────────────
    match dispatch_via_http_svc(&path).await {
        Some(result) => {
            info!(
                "[req-{}] {} {} → app_id={} ({}ms)",
                req_id, method, path, result.app_id, start.elapsed().as_millis()
            );
            Ok(json_response(
                StatusCode::OK,
                json!({
                    "dispatched": true,
                    "app_id": result.app_id,
                    "vnic_id": result.vnic_id,
                    "handler_id": result.handler_id,
                    "path": path,
                    "method": method.as_str(),
                    "note": "proxy stub — upstream forwarding not yet implemented"
                }),
            ))
        }
        None => {
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
                    "hint": "register an app via OP_APP_LAUNCH with an http_prefix or set GATEWAY_ROUTES"
                }),
            ))
        }
    }
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

    let ipc_path = std::env::var("GATEWAY_IPC")
        .unwrap_or_else(|_| "/run/agentos.sock".to_string());

    let route_count = ROUTE_TABLE.get().map(|t| t.len()).unwrap_or(0);
    info!("agentOS HTTP gateway starting on {}", listen_addr);
    info!("IPC socket: {} (stub — not yet connected)", ipc_path);
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
