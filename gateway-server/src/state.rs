//! Shared server state and the correlation/dispatch layer that ties HTTP/WS
//! requests to real engine events.

use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, RwLock};
use std::time::{SystemTime, UNIX_EPOCH};

use codec::{AckKind, BinaryMessage, RejectReason, Side};
use gateway::Gateway;
use serde_json::json;
use tokio::sync::{broadcast, mpsc, oneshot};

use crate::book::{ticks_to_price, OrderBook};
use crate::journal::{Journal, JournalRecord};
use crate::stats::Metrics;

/// Maximum trades retained in the in-memory trade tape (newest kept).
const TRADE_TAPE_CAP: usize = 10_000;
/// Capacity of the broadcast channel fanned out to WebSocket clients.
pub const EVENT_CHANNEL_CAPACITY: usize = 4096;
/// Capacity of the command channel feeding the engine writer task.
pub const COMMAND_CHANNEL_CAPACITY: usize = 4096;

/// A single fill accumulated for an aggressor order before its terminator.
#[derive(Debug, Clone, Copy)]
pub struct Fill {
    pub exec_seq: u64,
    pub price_ticks: u64,
    pub quantity: u32,
    pub resting_id: u64,
}

/// A recorded trade for the trade tape / API.
#[derive(Debug, Clone, Copy)]
pub struct TradeRecord {
    pub exec_seq: u64,
    pub price_ticks: u64,
    pub quantity: u32,
    pub incoming_id: u64,
    pub resting_id: u64,
    pub ts_ms: u64,
}

/// The terminating outcome of a forwarded request, delivered to the waiting
/// HTTP handler.
#[derive(Debug)]
pub struct EngineReply {
    pub terminator: BinaryMessage,
    pub fills: Vec<Fill>,
}

/// Tracking state for an in-flight request awaiting its terminator.
#[derive(Debug)]
struct InFlight {
    submitted_side: Side,
    submitted_price_ticks: u64,
    submitted_quantity: u32,
    is_cancel: bool,
    fills: Vec<Fill>,
    responder: oneshot::Sender<EngineReply>,
}

/// Shared, cloneable application state.
pub struct AppState {
    pub gateway: Mutex<Gateway>,
    pub book: RwLock<OrderBook>,
    pub metrics: Metrics,
    trades: Mutex<VecDeque<TradeRecord>>,
    inflight: Mutex<HashMap<u64, InFlight>>,
    command_tx: mpsc::Sender<Vec<u8>>,
    events: broadcast::Sender<String>,
    connected: AtomicBool,
    book_dirty: AtomicBool,
    /// True while the gateway is replaying the journal against a freshly
    /// connected engine; live metrics and journaling are suppressed.
    restoring: AtomicBool,
    /// Configured API keys. Empty means authentication is disabled (open mode).
    api_keys: HashSet<String>,
    /// Optional persistence journal for accepted mutating commands.
    pub journal: Option<Journal>,
}

impl AppState {
    pub fn new(
        command_tx: mpsc::Sender<Vec<u8>>,
        events: broadcast::Sender<String>,
        api_keys: HashSet<String>,
        journal: Option<Journal>,
    ) -> Self {
        AppState {
            gateway: Mutex::new(Gateway::new()),
            book: RwLock::new(OrderBook::default()),
            metrics: Metrics::new(),
            trades: Mutex::new(VecDeque::with_capacity(1024)),
            inflight: Mutex::new(HashMap::new()),
            command_tx,
            events,
            connected: AtomicBool::new(false),
            book_dirty: AtomicBool::new(false),
            restoring: AtomicBool::new(false),
            api_keys,
            journal,
        }
    }

    // -- authentication -----------------------------------------------------

    /// Whether a valid API key is required on mutating endpoints.
    pub fn auth_required(&self) -> bool {
        !self.api_keys.is_empty()
    }

    /// Whether `key` matches a configured API key.
    pub fn is_valid_api_key(&self, key: &str) -> bool {
        self.api_keys.contains(key)
    }

    // -- replay gate --------------------------------------------------------

    /// Whether the gateway is currently replaying the journal.
    pub fn is_restoring(&self) -> bool {
        self.restoring.load(Ordering::Acquire)
    }

    /// Enter journal-replay mode: suppress live metrics, journaling, and event
    /// fan-out while the book is reconstructed.
    pub fn begin_restore(&self) {
        self.restoring.store(true, Ordering::Release);
    }

    /// Leave journal-replay mode.
    pub fn end_restore(&self) {
        self.restoring.store(false, Ordering::Release);
    }

    // -- connection state ---------------------------------------------------

    pub fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Acquire)
    }

    pub fn set_connected(&self, connected: bool) {
        self.connected.store(connected, Ordering::Release);
    }

    // -- event fan-out ------------------------------------------------------

    pub fn subscribe(&self) -> broadcast::Receiver<String> {
        self.events.subscribe()
    }

    fn broadcast(&self, value: serde_json::Value) {
        let _ = self.events.send(value.to_string());
    }

    /// Public fan-out used by the background broadcaster tasks.
    pub fn broadcast_event(&self, value: serde_json::Value) {
        self.broadcast(value);
    }

    /// Whether the book changed since the dirty flag was last taken.
    pub fn take_book_dirty(&self) -> bool {
        self.book_dirty.swap(false, Ordering::AcqRel)
    }

    fn mark_book_dirty(&self) {
        self.book_dirty.store(true, Ordering::Release);
    }

    // -- request submission -------------------------------------------------

    /// Register an in-flight request and return the receiver to await its
    /// terminator on. Returns `None` if the id is already in flight.
    pub fn register_inflight(
        &self,
        order_id: u64,
        submitted_side: Side,
        submitted_price_ticks: u64,
        submitted_quantity: u32,
        is_cancel: bool,
    ) -> Option<oneshot::Receiver<EngineReply>> {
        let (responder, receiver) = oneshot::channel();
        let mut inflight = self.inflight.lock().unwrap();
        if inflight.contains_key(&order_id) {
            return None;
        }
        inflight.insert(
            order_id,
            InFlight {
                submitted_side,
                submitted_price_ticks,
                submitted_quantity,
                is_cancel,
                fills: Vec::new(),
                responder,
            },
        );
        Some(receiver)
    }

    /// Drop an in-flight entry (e.g. on send failure or timeout).
    pub fn drop_inflight(&self, order_id: u64) {
        self.inflight.lock().unwrap().remove(&order_id);
    }

    /// Send an encoded command frame to the engine writer task.
    pub async fn send_command(&self, frame: Vec<u8>) -> bool {
        self.command_tx.send(frame).await.is_ok()
    }

    /// Fail every in-flight request (called on engine disconnect) so awaiting
    /// handlers resolve immediately rather than waiting for their timeout.
    ///
    /// Dropping each stored responder closes its oneshot, so the waiting HTTP
    /// handler observes a receive error and returns HTTP 503 engine_unavailable.
    pub fn fail_all_inflight(&self) {
        self.inflight.lock().unwrap().clear();
    }

    /// Reset the derived book projection (called when the engine connection is
    /// re-established): a reconnected engine starts from an empty book, so the
    /// projection is cleared and a fresh snapshot is pushed to clients.
    pub fn reset_book(&self) {
        *self.book.write().unwrap() = OrderBook::default();
        self.mark_book_dirty();
    }

    // -- trade tape ---------------------------------------------------------

    pub fn recent_trades(&self, limit: usize) -> Vec<TradeRecord> {
        let trades = self.trades.lock().unwrap();
        trades.iter().take(limit).copied().collect()
    }

    // -- engine event handling ----------------------------------------------

    /// Apply a single decoded engine event to all derived state and fan out the
    /// corresponding WebSocket event(s). Called serially by the reader task.
    pub fn handle_engine_event(&self, msg: BinaryMessage) {
        match msg {
            BinaryMessage::Trade {
                exec_seq,
                price_ticks,
                quantity,
                incoming_id,
                resting_id,
            } => {
                // Accumulate the fill against the aggressor's in-flight entry.
                if let Some(entry) = self.inflight.lock().unwrap().get_mut(&incoming_id) {
                    entry.fills.push(Fill {
                        exec_seq,
                        price_ticks,
                        quantity,
                        resting_id,
                    });
                }

                // Reduce the resting (maker) order in the book projection.
                self.book.write().unwrap().apply_trade(resting_id, quantity);
                self.mark_book_dirty();

                // Replayed trades only rebuild the book; they are not recorded
                // on the live tape, metrics, or event feed.
                if self.is_restoring() {
                    return;
                }

                // Record on the trade tape (newest first).
                let ts_ms = now_ms();
                {
                    let mut trades = self.trades.lock().unwrap();
                    if trades.len() == TRADE_TAPE_CAP {
                        trades.pop_back();
                    }
                    trades.push_front(TradeRecord {
                        exec_seq,
                        price_ticks,
                        quantity,
                        incoming_id,
                        resting_id,
                        ts_ms,
                    });
                }
                self.metrics.record_trade();

                self.broadcast(json!({
                    "type": "trade",
                    "exec_seq": exec_seq,
                    "price": ticks_to_price(price_ticks),
                    "quantity": quantity,
                    "incoming_id": incoming_id,
                    "resting_id": resting_id,
                    "ts_ms": ts_ms,
                }));
            }

            BinaryMessage::Ack { order_id, kind } => match kind {
                AckKind::Accepted => self.complete_accept(order_id, msg),
                AckKind::Cancelled => self.complete_cancel(order_id, msg),
            },

            BinaryMessage::Reject { order_id, reason } => {
                self.complete_reject(order_id, reason, msg)
            }

            // The engine never sends request messages back; ignore defensively.
            BinaryMessage::NewOrder { .. } | BinaryMessage::CancelOrder { .. } => {}
        }
    }

    fn complete_accept(&self, order_id: u64, terminator: BinaryMessage) {
        let Some(entry) = self.inflight.lock().unwrap().remove(&order_id) else {
            return;
        };
        let filled: u32 = entry.fills.iter().map(|f| f.quantity).sum();
        let resting = entry.submitted_quantity.saturating_sub(filled);

        if resting > 0 {
            self.book.write().unwrap().insert_resting(
                order_id,
                entry.submitted_side,
                entry.submitted_price_ticks,
                resting,
            );
            self.mark_book_dirty();
        }

        if !self.is_restoring() {
            self.metrics.record_accepted();
            self.broadcast(json!({
                "type": "accepted",
                "order_id": order_id,
                "side": entry.submitted_side,
                "price": ticks_to_price(entry.submitted_price_ticks),
                "quantity": entry.submitted_quantity,
                "filled": filled,
                "resting": resting,
            }));
            if let Some(journal) = &self.journal {
                journal.append(&JournalRecord::New {
                    order_id,
                    side: entry.submitted_side,
                    price_ticks: entry.submitted_price_ticks,
                    quantity: entry.submitted_quantity,
                });
            }
        }

        let _ = entry.responder.send(EngineReply {
            terminator,
            fills: entry.fills,
        });
    }

    fn complete_cancel(&self, order_id: u64, terminator: BinaryMessage) {
        let Some(entry) = self.inflight.lock().unwrap().remove(&order_id) else {
            return;
        };
        self.book.write().unwrap().remove(order_id);
        self.mark_book_dirty();
        if !self.is_restoring() {
            self.metrics.record_cancel();
            self.broadcast(json!({
                "type": "cancelled",
                "order_id": order_id,
            }));
            if let Some(journal) = &self.journal {
                journal.append(&JournalRecord::Cancel { order_id });
            }
        }
        let _ = entry.responder.send(EngineReply {
            terminator,
            fills: entry.fills,
        });
    }

    fn complete_reject(&self, order_id: u64, reason: RejectReason, terminator: BinaryMessage) {
        let Some(entry) = self.inflight.lock().unwrap().remove(&order_id) else {
            return;
        };
        if !self.is_restoring() {
            if !entry.is_cancel {
                self.metrics.record_rejected();
            }
            self.broadcast(json!({
                "type": "rejected",
                "order_id": order_id,
                "reason": reason,
            }));
        }
        let _ = entry.responder.send(EngineReply {
            terminator,
            fills: entry.fills,
        });
    }
}

/// Current wall-clock time in milliseconds since the Unix epoch.
pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
