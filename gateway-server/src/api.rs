//! HTTP + WebSocket API surface and the background broadcaster tasks.

use std::sync::Arc;
use std::time::{Duration, Instant};

use axum::body::Bytes;
use axum::extract::ws::{Message, WebSocket, WebSocketUpgrade};
use axum::extract::{Path, Query, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::Json;
use codec::{layout, AckKind, BinaryMessage, RejectReason, Side};
use futures_util::{SinkExt, StreamExt};
use gateway::GatewayError;
use serde::Deserialize;
use serde_json::{json, Value};
use tokio::sync::broadcast::error::RecvError;
use tokio::time::{interval, timeout};

use crate::book::ticks_to_price;
use crate::state::{AppState, EngineReply};

/// Engine round-trip ceiling: no terminator within this window → HTTP 503.
const ENGINE_TIMEOUT: Duration = Duration::from_millis(1000);
/// Book WebSocket push throttle (≤20/sec).
const BOOK_PUSH_INTERVAL: Duration = Duration::from_millis(50);
/// Stats WebSocket push cadence (~2/sec).
const STATS_PUSH_INTERVAL: Duration = Duration::from_millis(500);
/// Default order-book depth.
const DEFAULT_DEPTH: usize = 15;
/// Default trade-tape page size.
const DEFAULT_TRADE_LIMIT: usize = 50;

// ---------------------------------------------------------------------------
// POST /api/orders
// ---------------------------------------------------------------------------

pub async fn post_order(State(state): State<Arc<AppState>>, body: Bytes) -> Response {
    // Validate + assign id via the Gateway library.
    // This is a trust-boundary concern independent of engine availability, so a
    // malformed or duplicate request is rejected with the correct 4xx even when
    // the engine is down.
    let message = {
        let mut gateway = state.gateway.lock().unwrap();
        match gateway.accept_new_order(&body) {
            Ok(message) => message,
            Err(err) => return gateway_error_response(err),
        }
    };

    if !state.is_connected() {
        return engine_unavailable();
    }

    let BinaryMessage::NewOrder {
        order_id,
        side,
        price_ticks,
        quantity,
        ..
    } = message
    else {
        return engine_unavailable();
    };

    let Some(receiver) =
        state.register_inflight(order_id, side, price_ticks, quantity, false)
    else {
        return engine_unavailable();
    };

    let mut frame = vec![0u8; layout::NEW_ORDER_LEN];
    if message.encode(&mut frame).is_err() {
        state.drop_inflight(order_id);
        return engine_unavailable();
    }

    state.metrics.record_submission();
    let started = Instant::now();
    if !state.send_command(frame).await {
        state.drop_inflight(order_id);
        return engine_unavailable();
    }

    match timeout(ENGINE_TIMEOUT, receiver).await {
        Ok(Ok(reply)) => {
            let latency_us = started.elapsed().as_micros() as u64;
            state.metrics.record_latency(latency_us);
            new_order_response(order_id, side, price_ticks, quantity, reply, latency_us)
        }
        _ => {
            state.drop_inflight(order_id);
            engine_unavailable()
        }
    }
}

fn new_order_response(
    order_id: u64,
    side: Side,
    price_ticks: u64,
    quantity: u32,
    reply: EngineReply,
    latency_us: u64,
) -> Response {
    match reply.terminator {
        BinaryMessage::Ack {
            kind: AckKind::Accepted,
            ..
        } => {
            let filled: u32 = reply.fills.iter().map(|f| f.quantity).sum();
            let resting = quantity.saturating_sub(filled);
            let fills: Vec<Value> = reply
                .fills
                .iter()
                .map(|f| {
                    json!({
                        "price": ticks_to_price(f.price_ticks),
                        "quantity": f.quantity,
                        "resting_id": f.resting_id,
                        "exec_seq": f.exec_seq,
                    })
                })
                .collect();
            let body = json!({
                "status": "accepted",
                "order_id": order_id,
                "side": side,
                "price": ticks_to_price(price_ticks),
                "quantity": quantity,
                "filled": filled,
                "resting": resting,
                "fills": fills,
                "latency_us": latency_us,
            });
            (StatusCode::OK, Json(body)).into_response()
        }
        BinaryMessage::Reject { reason, .. } => reject_response(order_id, reason),
        // Any other terminator for a NewOrder indicates an engine fault.
        _ => engine_unavailable(),
    }
}

fn reject_response(order_id: u64, reason: RejectReason) -> Response {
    let (status, reason_str) = match reason {
        RejectReason::InvalidPrice => (StatusCode::BAD_REQUEST, "invalid_price"),
        RejectReason::InvalidQuantity => (StatusCode::BAD_REQUEST, "invalid_quantity"),
        RejectReason::OrderNotFound => (StatusCode::CONFLICT, "order_not_found"),
        RejectReason::NoLongerResting => (StatusCode::CONFLICT, "no_longer_resting"),
        RejectReason::IntegrityViolation => return engine_unavailable(),
    };
    let body = json!({
        "status": "rejected",
        "reason": reason_str,
        "order_id": order_id,
    });
    (status, Json(body)).into_response()
}

// ---------------------------------------------------------------------------
// POST /api/cancel/{id}
// ---------------------------------------------------------------------------

pub async fn post_cancel(
    State(state): State<Arc<AppState>>,
    Path(order_id): Path<u64>,
) -> Response {
    if !state.is_connected() {
        return engine_unavailable();
    }

    let Some(receiver) =
        state.register_inflight(order_id, Side::Buy, 0, 0, true)
    else {
        return engine_unavailable();
    };

    let message = BinaryMessage::CancelOrder { order_id };
    let mut frame = vec![0u8; layout::CANCEL_ORDER_LEN];
    if message.encode(&mut frame).is_err() {
        state.drop_inflight(order_id);
        return engine_unavailable();
    }

    state.metrics.record_submission();
    let started = Instant::now();
    if !state.send_command(frame).await {
        state.drop_inflight(order_id);
        return engine_unavailable();
    }

    match timeout(ENGINE_TIMEOUT, receiver).await {
        Ok(Ok(reply)) => {
            let latency_us = started.elapsed().as_micros() as u64;
            state.metrics.record_latency(latency_us);
            cancel_response(order_id, reply, latency_us)
        }
        _ => {
            state.drop_inflight(order_id);
            engine_unavailable()
        }
    }
}

fn cancel_response(order_id: u64, reply: EngineReply, latency_us: u64) -> Response {
    match reply.terminator {
        BinaryMessage::Ack {
            kind: AckKind::Cancelled,
            ..
        } => {
            let body = json!({
                "status": "cancelled",
                "order_id": order_id,
                "latency_us": latency_us,
            });
            (StatusCode::OK, Json(body)).into_response()
        }
        BinaryMessage::Reject { reason, .. } => {
            let reason_str = match reason {
                RejectReason::OrderNotFound => "order_not_found",
                RejectReason::NoLongerResting => "no_longer_resting",
                _ => return engine_unavailable(),
            };
            let body = json!({
                "status": "rejected",
                "reason": reason_str,
                "order_id": order_id,
            });
            (StatusCode::CONFLICT, Json(body)).into_response()
        }
        _ => engine_unavailable(),
    }
}

// ---------------------------------------------------------------------------
// GET /api/book
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
pub struct BookQuery {
    depth: Option<usize>,
}

pub async fn get_book(
    State(state): State<Arc<AppState>>,
    Query(query): Query<BookQuery>,
) -> Response {
    let depth = query.depth.unwrap_or(DEFAULT_DEPTH).clamp(1, 1000);
    let snapshot = state.book.read().unwrap().snapshot(depth);
    Json(snapshot).into_response()
}

// ---------------------------------------------------------------------------
// GET /api/stats
// ---------------------------------------------------------------------------

pub async fn get_stats(State(state): State<Arc<AppState>>) -> Response {
    Json(stats_value(&state)).into_response()
}

/// Build the `/api/stats` JSON object (also used by the WebSocket pusher).
pub fn stats_value(state: &AppState) -> Value {
    let metrics = state.metrics.snapshot();
    let resting_orders = state.book.read().unwrap().resting_count();
    json!({
        "engine_connected": state.is_connected(),
        "orders_submitted": metrics.orders_submitted,
        "orders_accepted": metrics.orders_accepted,
        "orders_rejected": metrics.orders_rejected,
        "cancels": metrics.cancels,
        "trades": metrics.trades,
        "resting_orders": resting_orders,
        "latency_us": {
            "p50": metrics.latency_us.p50,
            "p99": metrics.latency_us.p99,
            "max": metrics.latency_us.max,
        },
        "throughput_ops": metrics.throughput_ops,
    })
}

// ---------------------------------------------------------------------------
// GET /api/trades
// ---------------------------------------------------------------------------

#[derive(Debug, Deserialize)]
pub struct TradesQuery {
    limit: Option<usize>,
}

pub async fn get_trades(
    State(state): State<Arc<AppState>>,
    Query(query): Query<TradesQuery>,
) -> Response {
    let limit = query.limit.unwrap_or(DEFAULT_TRADE_LIMIT).clamp(1, 10_000);
    let trades: Vec<Value> = state
        .recent_trades(limit)
        .into_iter()
        .map(|t| {
            json!({
                "exec_seq": t.exec_seq,
                "price": ticks_to_price(t.price_ticks),
                "quantity": t.quantity,
                "incoming_id": t.incoming_id,
                "resting_id": t.resting_id,
                "ts_ms": t.ts_ms,
            })
        })
        .collect();
    Json(trades).into_response()
}

// ---------------------------------------------------------------------------
// GET /ws
// ---------------------------------------------------------------------------

pub async fn ws_handler(
    State(state): State<Arc<AppState>>,
    upgrade: WebSocketUpgrade,
) -> Response {
    upgrade.on_upgrade(move |socket| handle_socket(socket, state))
}

async fn handle_socket(socket: WebSocket, state: Arc<AppState>) {
    let mut events = state.subscribe();
    let (mut sender, mut receiver) = socket.split();

    loop {
        tokio::select! {
            event = events.recv() => match event {
                Ok(text) => {
                    if sender.send(Message::Text(text)).await.is_err() {
                        break;
                    }
                }
                Err(RecvError::Lagged(_)) => continue,
                Err(RecvError::Closed) => break,
            },
            inbound = receiver.next() => match inbound {
                Some(Ok(Message::Close(_))) | None => break,
                Some(Ok(_)) => {}
                Some(Err(_)) => break,
            },
        }
    }
}

// ---------------------------------------------------------------------------
// Background broadcasters
// ---------------------------------------------------------------------------

/// Push a book snapshot to WebSocket clients when the book changes (≤20/sec).
pub async fn book_broadcaster(state: Arc<AppState>) {
    let mut ticker = interval(BOOK_PUSH_INTERVAL);
    loop {
        ticker.tick().await;
        if !state.take_book_dirty() {
            continue;
        }
        let snapshot = state.book.read().unwrap().snapshot(DEFAULT_DEPTH);
        let event = json!({
            "type": "book",
            "bids": snapshot.bids,
            "asks": snapshot.asks,
        });
        state.broadcast_event(event);
    }
}

/// Push a stats snapshot to WebSocket clients (~2/sec).
pub async fn stats_broadcaster(state: Arc<AppState>) {
    let mut ticker = interval(STATS_PUSH_INTERVAL);
    loop {
        ticker.tick().await;
        let mut event = stats_value(&state);
        if let Value::Object(ref mut map) = event {
            map.insert("type".to_string(), Value::String("stats".to_string()));
        }
        state.broadcast_event(event);
    }
}

// ---------------------------------------------------------------------------
// Shared response helpers
// ---------------------------------------------------------------------------

fn engine_unavailable() -> Response {
    let body = json!({ "status": "error", "reason": "engine_unavailable" });
    (StatusCode::SERVICE_UNAVAILABLE, Json(body)).into_response()
}

fn gateway_error_response(err: GatewayError) -> Response {
    let status = StatusCode::from_u16(err.http_status()).unwrap_or(StatusCode::BAD_REQUEST);
    let body = match &err {
        GatewayError::InvalidJson(_) => json!({
            "status": "rejected",
            "reason": "malformed_json",
        }),
        GatewayError::MissingField(field) => json!({
            "status": "rejected",
            "reason": "missing_field",
            "field": field,
        }),
        GatewayError::InvalidField(field) => {
            let reason = match field.as_str() {
                "price" => "invalid_price",
                "quantity" => "invalid_quantity",
                _ => "invalid_field",
            };
            json!({
                "status": "rejected",
                "reason": reason,
                "field": field,
            })
        }
        GatewayError::DuplicateOrderId(id) => json!({
            "status": "rejected",
            "reason": "duplicate_order_id",
            "order_id": id,
        }),
        GatewayError::EngineUnavailable => json!({
            "status": "error",
            "reason": "engine_unavailable",
        }),
    };
    (status, Json(body)).into_response()
}
