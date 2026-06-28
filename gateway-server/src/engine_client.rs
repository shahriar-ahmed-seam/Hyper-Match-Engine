//! Persistent TCP client to the C++ Matching_Engine.
//!
//! A single connection is maintained with a reconnect loop (exponential backoff
//! capped at ~2s). Within a connection the read and write halves are driven
//! concurrently: a reader task frames and decodes engine events while the
//! manager loop writes queued command frames. On any disconnect the in-flight
//! requests are failed so awaiting handlers resolve immediately.

use std::sync::Arc;
use std::time::Duration;

use codec::MessageType;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tokio::time::sleep;

use crate::state::AppState;

const INITIAL_BACKOFF: Duration = Duration::from_millis(100);
const MAX_BACKOFF: Duration = Duration::from_secs(2);

/// Run the engine connection manager until the command channel is closed.
pub async fn run(state: Arc<AppState>, engine_addr: String, mut command_rx: mpsc::Receiver<Vec<u8>>) {
    let mut backoff = INITIAL_BACKOFF;

    loop {
        match TcpStream::connect(&engine_addr).await {
            Ok(stream) => {
                let _ = stream.set_nodelay(true);
                tracing::info!(engine = %engine_addr, "connected to matching engine");
                backoff = INITIAL_BACKOFF;
                // A reconnected engine starts from an empty book, so clear any
                // stale projection before processing fresh events.
                state.reset_book();
                state.set_connected(true);

                let (read_half, mut write_half) = stream.into_split();
                let reader_state = state.clone();
                let mut reader = tokio::spawn(read_loop(read_half, reader_state));

                // Drive writes from the command channel until the connection
                // breaks or the reader task ends (signalling a disconnect).
                let channel_closed = loop {
                    tokio::select! {
                        command = command_rx.recv() => match command {
                            Some(frame) => {
                                if write_half.write_all(&frame).await.is_err() {
                                    break false;
                                }
                            }
                            // Command channel closed: server is shutting down.
                            None => break true,
                        },
                        _ = &mut reader => break false,
                    }
                };

                state.set_connected(false);
                reader.abort();
                state.fail_all_inflight();

                if channel_closed {
                    return;
                }
                tracing::warn!(engine = %engine_addr, "engine connection lost; reconnecting");
            }
            Err(err) => {
                state.set_connected(false);
                tracing::warn!(
                    engine = %engine_addr,
                    error = %err,
                    backoff_ms = backoff.as_millis() as u64,
                    "engine connect failed; retrying"
                );
                sleep(backoff).await;
                backoff = (backoff * 2).min(MAX_BACKOFF);
            }
        }
    }
}

/// Read, frame, and decode engine events until the stream errors or yields an
/// unknown/undecodable frame (either of which forces a reconnect).
async fn read_loop<R>(mut read_half: R, state: Arc<AppState>)
where
    R: AsyncReadExt + Unpin,
{
    loop {
        let mut type_byte = [0u8; 1];
        if read_half.read_exact(&mut type_byte).await.is_err() {
            return;
        }

        let Some(message_type) = MessageType::from_byte(type_byte[0]) else {
            tracing::error!(byte = type_byte[0], "unknown engine message type byte");
            return;
        };

        let total_len = message_type.encoded_len();
        let mut frame = vec![0u8; total_len];
        frame[0] = type_byte[0];
        if read_half.read_exact(&mut frame[1..]).await.is_err() {
            return;
        }

        match codec::decode(&frame) {
            Ok(message) => state.handle_engine_event(message),
            Err(err) => {
                tracing::error!(error = %err, "failed to decode engine frame");
                return;
            }
        }
    }
}
