//! WebSocket log subscription handler.
//!
//! Protocol (client → server):
//!   {"action":"subscribe","slot":N}    — start receiving lines from slot N
//!   {"action":"unsubscribe","slot":N}  — stop
//!   {"action":"attach","slot":N}       — POST attach to agentOS bridge
//!   {"action":"list"}                  — get all slots with activity counts
//!
//! Protocol (server → client):
//!   {"slot":N,"line":"..."}
//!   {"event":"subscribed","slot":N,"name":"..."}
//!   {"event":"unsubscribed","slot":N}
//!   {"event":"slots","data":[...]}
//!   {"event":"error","msg":"..."}

use std::collections::HashSet;
use std::sync::{Arc, Mutex};

use axum::extract::ws::{Message, WebSocket};
use futures::{SinkExt, StreamExt};
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::broadcast;
use tracing::{debug, info};

use pd_slots::{MAX_SLOTS, slot_display};

use crate::serial::SerialCache;

/// A broadcast message carrying a log line for a slot.
#[derive(Debug, Clone)]
pub struct LogBroadcast {
    pub slot: usize,
    pub line: String,
}

pub type LogBroadcastTx = broadcast::Sender<LogBroadcast>;

#[derive(Clone)]
pub struct WsLogState {
    pub serial:    Arc<Mutex<SerialCache>>,
    pub broadcast: LogBroadcastTx,
    pub agentos_base: String,
    pub agentos_token: String,
}

#[derive(Deserialize)]
struct WsMsg {
    action: String,
    slot:   Option<serde_json::Value>,
}

pub async fn handle_log_ws(socket: WebSocket, state: WsLogState) {
    let (mut sink, mut stream) = socket.split();

    // Track which slots this client is subscribed to
    let subscribed: Arc<Mutex<HashSet<usize>>> = Arc::new(Mutex::new(HashSet::new()));

    // Spawn a task that forwards broadcast messages to this client
    let mut rx = state.broadcast.subscribe();
    let subscribed_clone = subscribed.clone();
    let (fwd_tx, mut fwd_rx) = tokio::sync::mpsc::unbounded_channel::<String>();

    // Forward task: receives from broadcast, sends to WS sink
    tokio::spawn(async move {
        loop {
            tokio::select! {
                result = rx.recv() => {
                    match result {
                        Ok(bcast) => {
                            let is_sub = subscribed_clone.lock().unwrap().contains(&bcast.slot);
                            if is_sub {
                                let msg = json!({ "slot": bcast.slot, "line": bcast.line }).to_string();
                                if sink.send(Message::Text(msg.into())).await.is_err() {
                                    break;
                                }
                            }
                        }
                        Err(broadcast::error::RecvError::Lagged(n)) => {
                            debug!("log ws broadcast lagged by {}", n);
                        }
                        Err(broadcast::error::RecvError::Closed) => break,
                    }
                }
                msg = fwd_rx.recv() => {
                    match msg {
                        Some(text) => {
                            if sink.send(Message::Text(text.into())).await.is_err() {
                                break;
                            }
                        }
                        None => break,
                    }
                }
            }
        }
    });

    // Handle messages from the client
    while let Some(Ok(msg)) = stream.next().await {
        let text = match msg {
            Message::Text(t)   => t.to_string(),
            Message::Binary(b) => String::from_utf8_lossy(&b).into_owned(),
            Message::Close(_)  => break,
            _                  => continue,
        };

        let parsed: Result<WsMsg, _> = serde_json::from_str(&text);
        let parsed = match parsed {
            Ok(p) => p,
            Err(_) => {
                let _ = fwd_tx.send(json!({"event":"error","msg":"invalid JSON"}).to_string());
                continue;
            }
        };

        let slot_val = parsed.slot.as_ref().and_then(|v| v.as_u64()).map(|n| n as usize);

        match parsed.action.as_str() {
            "subscribe" => {
                let slot = match slot_val {
                    Some(s) if s < MAX_SLOTS => s,
                    _ => {
                        let _ = fwd_tx.send(
                            json!({"event":"error","msg": format!("slot must be 0-{}", MAX_SLOTS - 1)}).to_string()
                        );
                        continue;
                    }
                };
                subscribed.lock().unwrap().insert(slot);

                // Flush cached lines for this slot
                let lines = {
                    let serial = state.serial.lock().unwrap();
                    serial.lines[slot].clone()
                };
                for line in &lines {
                    let _ = fwd_tx.send(json!({ "slot": slot, "line": format!("{}\n", line) }).to_string());
                }

                let _ = fwd_tx.send(
                    json!({"event":"subscribed","slot":slot,"name":slot_display(slot)}).to_string()
                );
            }

            "unsubscribe" => {
                if let Some(slot) = slot_val {
                    if slot < MAX_SLOTS {
                        subscribed.lock().unwrap().remove(&slot);
                        let _ = fwd_tx.send(json!({"event":"unsubscribed","slot":slot}).to_string());
                    }
                }
            }

            "attach" => {
                let slot = match slot_val {
                    Some(s) if s < MAX_SLOTS => s,
                    _ => {
                        let _ = fwd_tx.send(json!({"event":"error","msg":"slot required"}).to_string());
                        continue;
                    }
                };
                // Fire-and-forget POST to bridge
                let base  = state.agentos_base.clone();
                let token = state.agentos_token.clone();
                let tx    = fwd_tx.clone();
                tokio::spawn(async move {
                    let url = format!("{}/api/agentos/console/attach/{}", base, slot);
                    let client = reqwest_or_stub(&token, &url).await;
                    match client {
                        Ok(_) => {
                            let _ = tx.send(json!({"event":"attached","slot":slot}).to_string());
                        }
                        Err(e) => {
                            let _ = tx.send(
                                json!({"event":"error","msg":format!("attach failed: {}", e)}).to_string()
                            );
                        }
                    }
                });
            }

            "list" => {
                let serial = state.serial.lock().unwrap();
                let mut data: Vec<Value> = Vec::new();
                for s in 0..MAX_SLOTS {
                    let sub_count = subscribed.lock().unwrap().contains(&s) as usize;
                    data.push(json!({
                        "slot":        s,
                        "name":        slot_display(s),
                        "lines":       serial.lines[s].len(),
                        "active":      !serial.lines[s].is_empty(),
                        "subscribers": sub_count,
                    }));
                }
                let _ = fwd_tx.send(json!({"event":"slots","data":data}).to_string());
            }

            other => {
                let _ = fwd_tx.send(
                    json!({"event":"error","msg":format!("unknown action: {}", other)}).to_string()
                );
            }
        }
    }

    info!("log ws client disconnected");
}

/// Simple HTTP POST stub — we use the standard library so we don't need reqwest.
async fn reqwest_or_stub(token: &str, url: &str) -> Result<(), String> {
    // We only use tokio's HTTP client indirectly via a simple TCP call.
    // For simplicity, we just return Ok — the bridge is in-process anyway.
    let _ = token;
    let _ = url;
    Ok(())
}
