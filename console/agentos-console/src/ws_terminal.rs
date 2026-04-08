//! WebSocket terminal handler.
//!
//! Path: /terminal/:slot
//!
//! On connect: flush cached lines for that slot to client.
//! Polling (every 50ms): try bridge API, fallback to serial cache delta.
//! On message from client: forward to bridge inject endpoint.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use axum::extract::ws::{Message, WebSocket};
use futures::{SinkExt, StreamExt};
use tokio::time::interval;
use tracing::{debug, info};

use pd_slots::MAX_SLOTS;

use crate::serial::SerialCache;

const POLL_MS: u64 = 50;

#[derive(Clone)]
pub struct WsTerminalState {
    pub serial:        Arc<Mutex<SerialCache>>,
    pub agentos_base:  String,
    pub agentos_token: String,
    /// Sender for bytes to inject into QEMU via the serial socket
    pub inject_tx:     Arc<Mutex<Option<tokio::sync::mpsc::UnboundedSender<Vec<u8>>>>>,
}

pub async fn handle_terminal_ws(socket: WebSocket, state: WsTerminalState, slot: usize) {
    if slot >= MAX_SLOTS {
        // Close with policy violation
        let _ = socket.close().await;
        return;
    }

    info!("terminal ws connected for slot {}", slot);

    let (mut sink, mut stream) = socket.split();

    // Flush cached serial lines for this slot immediately
    let cached: Vec<String> = {
        let serial = state.serial.lock().unwrap();
        serial.lines[slot].clone()
    };
    let mut serial_cursor = cached.len();

    if !cached.is_empty() {
        let text: String = cached.join("\r\n") + "\r\n";
        if sink.send(Message::Text(text.into())).await.is_err() {
            return;
        }
    }

    // Internal channel to pass outgoing messages from poller → sink
    let (out_tx, mut out_rx) = tokio::sync::mpsc::unbounded_channel::<String>();

    // Poller task: every POLL_MS, try bridge API then fall back to serial cache
    let state_poll = state.clone();
    let out_tx_poll = out_tx;
    tokio::spawn(async move {
        let mut ticker = interval(Duration::from_millis(POLL_MS));
        let mut api_cursor: usize = 0;
        let mut using_serial_fallback = false;

        loop {
            ticker.tick().await;

            // Try bridge HTTP API first
            let url = format!(
                "{}/api/agentos/console/{}?cursor={}",
                state_poll.agentos_base, slot, api_cursor
            );
            let api_ok = match try_get_lines(&url, &state_poll.agentos_token).await {
                Ok(lines) if !lines.is_empty() => {
                    api_cursor += lines.len();
                    let text = lines.join("\r\n") + "\r\n";
                    if out_tx_poll.send(text).is_err() {
                        return;
                    }
                    true
                }
                Ok(_) => true,
                Err(_) => false,
            };

            if !api_ok {
                if !using_serial_fallback {
                    using_serial_fallback = true;
                    debug!("slot {}: switching to serial log fallback", slot);
                }
                let new_lines: Vec<String> = {
                    let serial = state_poll.serial.lock().unwrap();
                    serial.lines[slot]
                        .iter()
                        .skip(serial_cursor)
                        .cloned()
                        .collect()
                };
                if !new_lines.is_empty() {
                    serial_cursor += new_lines.len();
                    let text = new_lines.join("\r\n") + "\r\n";
                    if out_tx_poll.send(text).is_err() {
                        return;
                    }
                }
            } else if using_serial_fallback {
                using_serial_fallback = false;
                debug!("slot {}: switched back to live API", slot);
            }
        }
    });

    // Multiplex: either incoming ws messages or outgoing poll data
    loop {
        tokio::select! {
            msg = stream.next() => {
                match msg {
                    Some(Ok(Message::Text(t))) => {
                        // Forward keystrokes to inject endpoint
                        let data = t.to_string();
                        // Try inject via serial socket first
                        if let Some(tx) = state.inject_tx.lock().unwrap().as_ref() {
                            let _ = tx.send(data.as_bytes().to_vec());
                        } else {
                            // Fire-and-forget POST to bridge inject
                            let url = format!(
                                "{}/api/agentos/console/inject/{}",
                                state.agentos_base, slot
                            );
                            let token = state.agentos_token.clone();
                            tokio::spawn(async move {
                                let _ = try_post_inject(&url, &token, &data).await;
                            });
                        }
                    }
                    Some(Ok(Message::Binary(b))) => {
                        if let Some(tx) = state.inject_tx.lock().unwrap().as_ref() {
                            let _ = tx.send(b.to_vec());
                        }
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {}
                }
            }
            Some(text) = out_rx.recv() => {
                if sink.send(Message::Text(text.into())).await.is_err() {
                    break;
                }
            }
        }
    }

    info!("terminal ws disconnected for slot {}", slot);
}

/// Try to GET lines from the bridge API, return vec of lines or Err.
async fn try_get_lines(url: &str, _token: &str) -> Result<Vec<String>, ()> {
    use tokio::net::TcpStream;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    // Parse URL manually: http://host:port/path?query
    let url_str = url;
    let without_scheme = url_str.strip_prefix("http://").ok_or(())?;
    let slash_pos = without_scheme.find('/').ok_or(())?;
    let host_port = &without_scheme[..slash_pos];
    let path_query = &without_scheme[slash_pos..];

    let mut stream = tokio::time::timeout(
        Duration::from_millis(1500),
        TcpStream::connect(host_port),
    ).await.map_err(|_| ())?.map_err(|_| ())?;

    let request = format!(
        "GET {} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\n\r\n",
        path_query, host_port
    );
    stream.write_all(request.as_bytes()).await.map_err(|_| ())?;

    let mut response = String::new();
    stream.read_to_string(&mut response).await.map_err(|_| ())?;

    // Split headers / body
    let body = response.splitn(2, "\r\n\r\n").nth(1).unwrap_or("");
    let val: serde_json::Value = serde_json::from_str(body).map_err(|_| ())?;
    let lines: Vec<String> = val["lines"]
        .as_array()
        .map(|arr| arr.iter().filter_map(|v| v.as_str().map(String::from)).collect())
        .unwrap_or_default();
    Ok(lines)
}

/// POST inject data to bridge.
async fn try_post_inject(url: &str, _token: &str, data: &str) -> Result<(), ()> {
    use tokio::net::TcpStream;
    use tokio::io::AsyncWriteExt;

    let without_scheme = url.strip_prefix("http://").ok_or(())?;
    let slash_pos = without_scheme.find('/').ok_or(())?;
    let host_port = &without_scheme[..slash_pos];
    let path = &without_scheme[slash_pos..];

    let body = serde_json::json!({ "data": data }).to_string();
    let request = format!(
        "POST {} HTTP/1.0\r\nHost: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
        path, host_port, body.len(), body
    );

    let mut stream = tokio::time::timeout(
        Duration::from_millis(1500),
        TcpStream::connect(host_port),
    ).await.map_err(|_| ())?.map_err(|_| ())?;
    stream.write_all(request.as_bytes()).await.map_err(|_| ())?;
    Ok(())
}
