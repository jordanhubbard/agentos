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
//! In the MVP the proxy step is stubbed: the gateway returns a 200 response
//! with JSON metadata identifying the matched app, rather than forwarding
//! to a real upstream socket.  The IPC calls to http_svc are also stubbed
//! until the seL4 IPC bridge is wired up.
//!
//! Configuration (environment variables):
//!   GATEWAY_LISTEN  — bind address (default: "0.0.0.0:8080")
//!   GATEWAY_IPC     — path to agentOS IPC socket (default: "/run/agentos.sock")
//!   RUST_LOG        — tracing filter (default: "info")

use std::convert::Infallible;
use std::net::SocketAddr;

use bytes::Bytes;
use http_body_util::{combinators::BoxBody, BodyExt, Empty, Full};
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
    let _ = (path, OP_HTTP_DISPATCH, HTTP_OK);  // suppress unused warnings
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

fn empty_response(status: StatusCode) -> Response<BoxBody<Bytes, Infallible>> {
    Response::builder()
        .status(status)
        .body(Empty::<Bytes>::new().map_err(|e| match e {}).boxed())
        .unwrap()
}

// ── Request handler ──────────────────────────────────────────────────────────

async fn handle_request(
    req: Request<Incoming>,
) -> Result<Response<BoxBody<Bytes, Infallible>>, Infallible> {
    let method = req.method().clone();
    let path   = req.uri().path().to_owned();

    debug!("{} {}", method, path);

    // Health / introspection endpoints handled directly by the gateway.
    if path == "/healthz" || path == "/_gateway/health" {
        return Ok(json_response(
            StatusCode::OK,
            json!({ "status": "ok", "service": "agentos-http-gateway" }),
        ));
    }

    if path == "/_gateway/routes" {
        // TODO: list routes from http_svc via OP_HTTP_LIST
        return Ok(json_response(
            StatusCode::OK,
            json!({ "routes": [], "note": "IPC bridge not yet connected" }),
        ));
    }

    // Dispatch all other requests via http_svc.
    match dispatch_via_http_svc(&path).await {
        Some(result) => {
            info!("dispatched {} {} → app_id={}", method, path, result.app_id);

            // MVP: return metadata instead of proxying to the upstream app.
            // Replace this block with a real reverse proxy once vNIC-to-socket
            // mapping is implemented.
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
                warn!("no handler for {} {}", method, path);
            }
            Ok(json_response(
                StatusCode::NOT_FOUND,
                json!({
                    "error": "no_handler",
                    "path": path,
                    "hint": "register an app via OP_APP_LAUNCH with an http_prefix"
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

    let listen_addr: SocketAddr = std::env::var("GATEWAY_LISTEN")
        .unwrap_or_else(|_| "0.0.0.0:8080".to_string())
        .parse()
        .expect("GATEWAY_LISTEN must be a valid socket address");

    let ipc_path = std::env::var("GATEWAY_IPC")
        .unwrap_or_else(|_| "/run/agentos.sock".to_string());

    info!("agentOS HTTP gateway starting on {}", listen_addr);
    info!("IPC socket: {} (stub — not yet connected)", ipc_path);

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
