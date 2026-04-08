// agent_context.rs — async AgentContext with Unix socket RPC
//
// Port of sdk/python/agentos_sdk/context.py to Rust.
//
// Only compiled when the `async-context` feature is enabled.
// This module requires std + tokio and is NOT compatible with the bare-metal
// seL4 environment; it is intended for host-side simulation and testing.
//
// Copyright (c) 2026 The agentOS Project
// SPDX-License-Identifier: Apache-2.0

#![cfg(feature = "async-context")]

use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use serde_json::Value;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::UnixStream;
use tokio::sync::{mpsc, oneshot, Mutex};

// ── Wire types ────────────────────────────────────────────────────────────────

#[derive(Serialize)]
struct RpcRequest {
    id: String,
    op: String,
    params: Value,
}

#[derive(Deserialize)]
struct RpcResponse {
    id: String,
    result: Option<Value>,
    error: Option<String>,
}

// ── Connection internals ──────────────────────────────────────────────────────

type PendingMap = Arc<Mutex<HashMap<String, oneshot::Sender<Result<Value, String>>>>>;

struct Connection {
    tx: mpsc::Sender<Vec<u8>>,
    pending: PendingMap,
}

impl Connection {
    /// Open the Unix socket, start reader + writer tasks, return a Connection.
    async fn open(socket_path: &str) -> std::io::Result<Self> {
        let stream = UnixStream::connect(socket_path).await?;
        let (read_half, write_half) = stream.into_split();

        let pending: PendingMap = Arc::new(Mutex::new(HashMap::new()));
        let pending_reader = Arc::clone(&pending);

        // Writer task: receives raw frames (newline-terminated JSON) and sends them
        let (tx, mut rx) = mpsc::channel::<Vec<u8>>(64);
        let mut write_half = write_half;
        tokio::spawn(async move {
            while let Some(frame) = rx.recv().await {
                if write_half.write_all(&frame).await.is_err() {
                    break;
                }
            }
        });

        // Reader task: reads response lines, dispatches to waiting callers
        tokio::spawn(async move {
            let mut reader = BufReader::new(read_half);
            let mut line = String::new();
            loop {
                line.clear();
                match reader.read_line(&mut line).await {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {}
                }
                let trimmed = line.trim();
                if trimmed.is_empty() {
                    continue;
                }
                if let Ok(resp) = serde_json::from_str::<RpcResponse>(trimmed) {
                    let mut map = pending_reader.lock().await;
                    if let Some(sender) = map.remove(&resp.id) {
                        let outcome = match resp.error {
                            Some(e) => Err(e),
                            None => Ok(resp.result.unwrap_or(Value::Null)),
                        };
                        let _ = sender.send(outcome);
                    }
                }
            }
        });

        Ok(Connection { tx, pending })
    }

    /// Send an RPC request and await the response.
    async fn call(&self, op: &str, params: Value) -> Result<Value, String> {
        let id = uuid::Uuid::new_v4().to_string();
        let req = RpcRequest {
            id: id.clone(),
            op: op.to_string(),
            params,
        };

        let (resp_tx, resp_rx) = oneshot::channel();
        {
            let mut map = self.pending.lock().await;
            map.insert(id, resp_tx);
        }

        let mut frame = serde_json::to_vec(&req).map_err(|e| e.to_string())?;
        frame.push(b'\n');
        self.tx
            .send(frame)
            .await
            .map_err(|_| "send channel closed".to_string())?;

        resp_rx.await.map_err(|_| "response channel dropped".to_string())?
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

/// Async agent context backed by a Unix-socket RPC channel.
///
/// In mock mode (no socket available) every operation returns a canned response
/// so that agent code can be tested without a running agentOS daemon.
pub struct AgentContext {
    pub agent_id: String,
    socket_path: String,
    mock: bool,
    conn: Option<Arc<Connection>>,
}

impl AgentContext {
    /// Create a new AgentContext.
    ///
    /// `mock` can be set explicitly; it is also auto-activated during `connect()`
    /// when the socket path does not exist.
    pub fn new(agent_id: impl Into<String>, socket_path: impl Into<String>, mock: bool) -> Self {
        Self {
            agent_id: agent_id.into(),
            socket_path: socket_path.into(),
            mock,
            conn: None,
        }
    }

    /// Open the socket connection and call `agent.init`.
    ///
    /// If the socket path does not exist the context silently enters mock mode.
    pub async fn connect(&mut self) -> &mut Self {
        if self.mock {
            return self;
        }
        if !Path::new(&self.socket_path).exists() {
            self.mock = true;
            return self;
        }
        match Connection::open(&self.socket_path).await {
            Ok(conn) => {
                let conn = Arc::new(conn);
                // Send agent.init handshake; ignore errors (daemon may not require it)
                let _ = conn
                    .call(
                        "agent.init",
                        serde_json::json!({ "agent_id": self.agent_id }),
                    )
                    .await;
                self.conn = Some(conn);
            }
            Err(_) => {
                self.mock = true;
            }
        }
        self
    }

    /// Returns true if running in mock mode.
    pub fn is_mock(&self) -> bool {
        self.mock
    }

    // ── Helper ────────────────────────────────────────────────────────────────

    async fn rpc(&self, op: &str, params: Value) -> Result<Value, String> {
        match &self.conn {
            Some(conn) => conn.call(op, params).await,
            None => Err("not connected".to_string()),
        }
    }

    // ── Message / pub-sub ─────────────────────────────────────────────────────

    /// Send a message to another agent and return the message-id.
    pub async fn send_message(&self, to: &str, content: &str) -> String {
        if self.mock {
            return format!("mock-ack:{}:{}", to, content.len());
        }
        let params = serde_json::json!({ "to": to, "content": content });
        match self.rpc("message.send", params).await {
            Ok(v) => v
                .get("message_id")
                .and_then(|v| v.as_str())
                .unwrap_or("ok")
                .to_string(),
            Err(e) => format!("error:{}", e),
        }
    }

    /// Publish a payload on a channel.
    pub async fn publish(&self, channel: &str, payload: Value) {
        if self.mock {
            return;
        }
        let params = serde_json::json!({ "channel": channel, "payload": payload });
        let _ = self.rpc("event.publish", params).await;
    }

    // ── Storage ───────────────────────────────────────────────────────────────

    /// Store a value under a key with an optional scope.
    pub async fn store(&self, key: &str, value: Value, scope: Option<&str>) {
        if self.mock {
            return;
        }
        let params = serde_json::json!({ "key": key, "value": value, "scope": scope });
        let _ = self.rpc("store.put", params).await;
    }

    /// Recall values matching a query (semantic search), returning up to `k` strings.
    pub async fn recall(&self, query: &str, k: usize) -> Vec<String> {
        if self.mock {
            return vec![format!("mock-recall:{}", query)];
        }
        let params = serde_json::json!({ "query": query, "k": k });
        match self.rpc("store.recall", params).await {
            Ok(Value::Array(arr)) => arr
                .into_iter()
                .filter_map(|v| v.as_str().map(|s| s.to_string()))
                .collect(),
            _ => vec![],
        }
    }

    // ── Tools ─────────────────────────────────────────────────────────────────

    /// List available tool names.
    pub async fn list_tools(&self) -> Vec<String> {
        if self.mock {
            return vec!["mock.echo".to_string(), "mock.ping".to_string()];
        }
        let params = serde_json::json!({});
        match self.rpc("tools.list", params).await {
            Ok(Value::Array(arr)) => arr
                .into_iter()
                .filter_map(|v| v.as_str().map(|s| s.to_string()))
                .collect(),
            _ => vec![],
        }
    }

    /// Call a named tool with JSON arguments and return the result.
    pub async fn call_tool(&self, name: &str, args: Value) -> Value {
        if self.mock {
            return serde_json::json!({ "mock": true, "tool": name, "args": args });
        }
        let params = serde_json::json!({ "name": name, "args": args });
        self.rpc("tools.call", params).await.unwrap_or(Value::Null)
    }

    // ── Model inference ───────────────────────────────────────────────────────

    /// Run a prompt through a model and return the response string.
    pub async fn query_model(
        &self,
        prompt: &str,
        model_id: &str,
        temperature: f64,
        max_tokens: u32,
    ) -> String {
        if self.mock {
            return format!("mock-response-to:{}", prompt);
        }
        let params = serde_json::json!({
            "prompt": prompt,
            "model_id": model_id,
            "temperature": temperature,
            "max_tokens": max_tokens,
        });
        match self.rpc("model.query", params).await {
            Ok(v) => v
                .get("text")
                .and_then(|v| v.as_str())
                .unwrap_or("")
                .to_string(),
            Err(_) => String::new(),
        }
    }

    // ── Capabilities ──────────────────────────────────────────────────────────

    /// List capabilities available to this agent.
    pub async fn list_caps(&self) -> Vec<Value> {
        if self.mock {
            return vec![serde_json::json!({ "cap": "mock.cap", "rights": ["read"] })];
        }
        let params = serde_json::json!({});
        match self.rpc("caps.list", params).await {
            Ok(Value::Array(arr)) => arr,
            _ => vec![],
        }
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    /// Gracefully close the connection.
    pub async fn close(&self) {
        if let Some(conn) = &self.conn {
            let _ = conn.call("agent.close", serde_json::json!({})).await;
        }
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    fn mock_ctx() -> AgentContext {
        AgentContext::new("test-agent", "/nonexistent.sock", true)
    }

    #[tokio::test]
    async fn test_mock_mode_send() {
        let ctx = mock_ctx();
        let ack = ctx.send_message("other-agent", "hello").await;
        assert!(
            ack.starts_with("mock-ack:"),
            "Expected mock-ack prefix, got: {}",
            ack
        );
    }

    #[tokio::test]
    async fn test_mock_mode_list_tools() {
        let ctx = mock_ctx();
        let tools = ctx.list_tools().await;
        assert!(!tools.is_empty(), "Mock list_tools should return at least one tool");
    }

    #[tokio::test]
    async fn test_mock_mode_recall() {
        let ctx = mock_ctx();
        // store is a no-op in mock mode; recall returns a mock response
        ctx.store("key1", serde_json::json!("value1"), None).await;
        let results = ctx.recall("key1", 5).await;
        assert!(!results.is_empty(), "Mock recall should return at least one result");
        assert!(
            results[0].contains("key1"),
            "Mock recall result should echo the query"
        );
    }
}
