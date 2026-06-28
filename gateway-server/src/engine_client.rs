//! Persistent TCP client to the C++ Matching_Engine.
//!
//! A single connection is maintained with a reconnect loop (exponential backoff
//! capped at ~2s). Within a connection the read and write halves are driven
//! concurrently: a reader task frames and decodes engine events while the
//! manager loop writes queued command frames. On any disconnect the in-flight
//! requests are failed so awaiting handlers resolve immediately.

use std::sync::Arc;
use std::time::Duration;

use codec::{layout, BinaryMessage, MessageType, Side};
use gateway::ValidatedOrder;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::sync::mpsc;
use tokio::time::{sleep, timeout};

use crate::journal::JournalRecord;
use crate::state::AppState;

const INITIAL_BACKOFF: Duration = Duration::from_millis(100);
const MAX_BACKOFF: Duration = Duration::from_secs(2);
/// Per-command ceiling while replaying the journal against a fresh engine.
const REPLAY_TIMEOUT: Duration = Duration::from_millis(1000);

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

                let (read_half, mut write_half) = stream.into_split();
                let reader_state = state.clone();
                let mut reader = tokio::spawn(read_loop(read_half, reader_state));

                // Reconstruct the book by replaying the journal before any new
                // client traffic is accepted (`connected` stays false until the
                // replay completes, so mutations are gated to HTTP 503).
                if !replay_journal(&state, &mut write_half).await {
                    reader.abort();
                    state.fail_all_inflight();
                    tracing::warn!(
                        engine = %engine_addr,
                        "engine connection lost during journal replay; reconnecting"
                    );
                    continue;
                }
                state.set_connected(true);

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

/// Replay the persistence journal against a freshly connected engine,
/// re-sending each accepted command (preserving original order ids) and
/// awaiting its terminator so the book projection is rebuilt deterministically.
///
/// Returns `false` if the connection broke mid-replay (the caller reconnects).
/// With no journal configured or an empty/missing journal this is a no-op that
/// returns `true`.
async fn replay_journal<W>(state: &Arc<AppState>, write_half: &mut W) -> bool
where
    W: AsyncWriteExt + Unpin,
{
    let Some(journal) = state.journal.as_ref() else {
        return true;
    };
    let records = match journal.read_records() {
        Ok(records) => records,
        Err(err) => {
            tracing::error!(error = %err, "failed to read journal; skipping replay");
            return true;
        }
    };
    if records.is_empty() {
        return true;
    }

    tracing::info!(records = records.len(), "replaying journal");
    state.begin_restore();
    let mut ok = true;
    for record in &records {
        let Some((order_id, frame, receiver)) = prepare_replay(state, record) else {
            continue;
        };
        if write_half.write_all(&frame).await.is_err() {
            state.drop_inflight(order_id);
            ok = false;
            break;
        }
        // Await the terminator (resolved by the reader task) so replay stays
        // ordered; the outcome itself is only used to rebuild the book.
        let _ = timeout(REPLAY_TIMEOUT, receiver).await;
    }
    state.end_restore();

    if ok {
        tracing::info!(
            resting_orders = state.book.read().unwrap().resting_count(),
            "journal replay complete"
        );
    }
    ok
}

/// Build the wire frame and register the in-flight entry for a single replayed
/// record, reserving its original id in the gateway. Returns `None` if the
/// frame cannot be prepared (the record is then skipped).
fn prepare_replay(
    state: &Arc<AppState>,
    record: &JournalRecord,
) -> Option<(u64, Vec<u8>, tokio::sync::oneshot::Receiver<crate::state::EngineReply>)> {
    match *record {
        JournalRecord::New {
            order_id,
            side,
            price_ticks,
            quantity,
        } => {
            let message = {
                let mut gateway = state.gateway.lock().unwrap();
                // Reserve the id so future auto-assigned ids never collide and
                // duplicate detection stays correct.
                gateway.reserve(order_id);
                gateway.new_order_message(&ValidatedOrder {
                    order_id,
                    side,
                    price_ticks,
                    quantity,
                })
            };
            let receiver =
                state.register_inflight(order_id, side, price_ticks, quantity, false)?;
            let mut frame = vec![0u8; layout::NEW_ORDER_LEN];
            if message.encode(&mut frame).is_err() {
                state.drop_inflight(order_id);
                return None;
            }
            Some((order_id, frame, receiver))
        }
        JournalRecord::Cancel { order_id } => {
            let receiver = state.register_inflight(order_id, Side::Buy, 0, 0, true)?;
            let message = BinaryMessage::CancelOrder { order_id };
            let mut frame = vec![0u8; layout::CANCEL_ORDER_LEN];
            if message.encode(&mut frame).is_err() {
                state.drop_inflight(order_id);
                return None;
            }
            Some((order_id, frame, receiver))
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
