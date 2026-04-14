//! Bidirectional WebSocket bridge to standalone VM serial sockets.
//!
//! Handles ws://.../ws/vm/:vm_id where vm_id is "freebsd" or "linux".
//! Bridges the WebSocket directly to the VM's dedicated Unix serial socket:
//!   freebsd → /tmp/freebsd-serial.sock
//!   linux   → /tmp/linux-serial.sock

use axum::extract::ws::{Message, WebSocket};
use futures::{SinkExt, StreamExt};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tracing::info;

fn vm_sock_path(vm_id: &str) -> Option<&'static str> {
    match vm_id {
        "freebsd" => Some("/tmp/freebsd-serial.sock"),
        "linux"   => Some("/tmp/linux-serial.sock"),
        _ => None,
    }
}

pub async fn handle_vm_serial_ws(socket: WebSocket, vm_id: String) {
    let sock_path = match vm_sock_path(&vm_id) {
        Some(p) => p,
        None => {
            let _ = socket.close().await;
            return;
        }
    };

    info!("vm serial ws connected for {}", vm_id);

    let (mut ws_sink, mut ws_stream) = socket.split();

    let stream = match UnixStream::connect(sock_path).await {
        Ok(s) => s,
        Err(e) => {
            let msg = format!(
                "\r\n\x1b[31m[serial] {} socket not available: {} — is the VM running?\x1b[0m\r\n",
                vm_id, e
            );
            let _ = ws_sink.send(Message::Text(msg.into())).await;
            return;
        }
    };

    let (mut reader, mut writer) = stream.into_split();

    // Serial reader → WebSocket channel
    let (out_tx, mut out_rx) = tokio::sync::mpsc::unbounded_channel::<Vec<u8>>();
    tokio::spawn(async move {
        let mut buf = [0u8; 4096];
        loop {
            match reader.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    if out_tx.send(buf[..n].to_vec()).is_err() {
                        break;
                    }
                }
            }
        }
    });

    // Multiplex: WebSocket input → serial, serial output → WebSocket
    loop {
        tokio::select! {
            msg = ws_stream.next() => {
                match msg {
                    Some(Ok(Message::Text(t))) => {
                        if writer.write_all(t.as_bytes()).await.is_err() {
                            break;
                        }
                    }
                    Some(Ok(Message::Binary(b))) => {
                        if writer.write_all(&b).await.is_err() {
                            break;
                        }
                    }
                    Some(Ok(Message::Close(_))) | None => break,
                    _ => {}
                }
            }
            Some(bytes) = out_rx.recv() => {
                let text = String::from_utf8_lossy(&bytes).into_owned();
                if ws_sink.send(Message::Text(text.into())).await.is_err() {
                    break;
                }
            }
        }
    }

    info!("vm serial ws disconnected for {}", vm_id);
}
