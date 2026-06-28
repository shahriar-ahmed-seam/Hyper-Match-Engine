//! Gateway crate for the Hyper-Match-Engine.
//!
//! The Gateway is the trust boundary that translates client JSON/HTTP into the
//! binary wire protocol and back.
//!
//! The request half:
//!
//! - parse the JSON body and validate `side`, `price`, and `quantity`,
//! - assign a unique order identifier when none is supplied, or preserve the
//!   supplied one,
//! - detect duplicate order identifiers against the set of live orders,
//! - produce a normalized [`ValidatedOrder`] or a typed [`GatewayError`], and
//! - encode an accepted order into a `NewOrder` message ready for the ingress
//!   ring, rejecting before forwarding.
//!
//! The response half:
//!
//! - decode an engine response message and convert it into a JSON response body,
//! - map engine `Reject` reasons to the correct HTTP status (400/409/503), and
//! - enforce the 1000 ms engine-response ceiling, returning HTTP 503
//!   ([`GatewayError::EngineUnavailable`]) when no response arrives in time.
//!
//! The timeout is modeled as a pure decision over an optional response and an
//! elapsed [`Duration`] ([`Gateway::resolve_engine_response`]) rather than by
//! sleeping on a real clock, so it is deterministic and unit-testable.

use std::collections::HashSet;
use std::time::Duration;

use codec::{decode, limits, BinaryMessage, CodecError, Side};

// Re-export the shared protocol enums so downstream Gateway code has a single
// import path.
pub use codec::{AckKind, RejectReason};

/// JSON limit-price bounds, in price units.
///
/// `price_ticks = round(price * 100)`, so these map to the
/// [`limits::MIN_PRICE_TICKS`] / [`limits::MAX_PRICE_TICKS`] wire bounds.
const MIN_PRICE: f64 = 0.01;
const MAX_PRICE: f64 = 999_999_999.99;

/// The maximum time the Gateway waits for a response message from the matching
/// engine before declaring it unavailable.
///
/// A response that arrives within this ceiling is converted to JSON; once the
/// elapsed wait exceeds it, the Gateway responds HTTP 503.
pub const ENGINE_RESPONSE_CEILING: Duration = Duration::from_millis(1000);

/// A validated, normalized order ready for encoding into the wire format.
///
/// Every field is guaranteed to lie within its permitted range, so encoding a
/// `ValidatedOrder` to a `NewOrder` message never fails a range check.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ValidatedOrder {
    /// Unique identifier among live orders (assigned or preserved).
    pub order_id: u64,
    /// Buy or Sell.
    pub side: Side,
    /// Limit price as fixed-point ticks (`price * 100`), in
    /// `[MIN_PRICE_TICKS, MAX_PRICE_TICKS]`.
    pub price_ticks: u64,
    /// Order quantity, in `[MIN_GATEWAY_QUANTITY, MAX_GATEWAY_QUANTITY]`.
    pub quantity: u32,
}

/// An HTTP/JSON response the Gateway returns to the originating client.
///
/// Produced by the response half of the Gateway: a decoded engine response is
/// converted into a `status` + JSON `body` pair. A successful decode always
/// yields a `JsonResponse` even when it represents a rejection (the `status`
/// then carries the mapped 4xx/5xx code); the [`GatewayError`] path is reserved
/// for the engine-unavailable timeout and undecodable engine output.
#[derive(Debug, Clone, PartialEq)]
pub struct JsonResponse {
    /// HTTP status code for the response (200 for success, mapped 4xx/5xx for
    /// engine rejections).
    pub status: u16,
    /// The JSON response body returned to the client.
    pub body: serde_json::Value,
}

/// A typed Gateway error. Each variant maps to a single HTTP status via
/// [`GatewayError::http_status`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum GatewayError {
    /// The request body was not valid JSON → HTTP 400.
    InvalidJson(String),
    /// A required field was absent → HTTP 400. Names the field.
    MissingField(String),
    /// A field was present but invalid → HTTP 400. Names the field.
    InvalidField(String),
    /// The order identifier is already in use → HTTP 409.
    DuplicateOrderId(u64),
    /// The matching engine did not respond in time → HTTP 503.
    EngineUnavailable,
}

impl GatewayError {
    /// The HTTP status code this error maps to.
    pub fn http_status(&self) -> u16 {
        match self {
            GatewayError::InvalidJson(_)
            | GatewayError::MissingField(_)
            | GatewayError::InvalidField(_) => 400,
            GatewayError::DuplicateOrderId(_) => 409,
            GatewayError::EngineUnavailable => 503,
        }
    }
}

impl std::fmt::Display for GatewayError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            GatewayError::InvalidJson(msg) => write!(f, "invalid JSON: {msg}"),
            GatewayError::MissingField(field) => write!(f, "missing required field: {field}"),
            GatewayError::InvalidField(field) => write!(f, "invalid field: {field}"),
            GatewayError::DuplicateOrderId(id) => write!(f, "duplicate order identifier: {id}"),
            GatewayError::EngineUnavailable => write!(f, "matching engine unavailable"),
        }
    }
}

impl std::error::Error for GatewayError {}

/// The request-side Gateway: validates JSON orders, manages order identifiers,
/// and encodes accepted orders into the wire format.
///
/// A single `Gateway` owns the set of live order identifiers (for duplicate
/// detection), the auto-id allocator, and the monotonic arrival-sequence
/// counter stamped onto each forwarded `NewOrder` message.
#[derive(Debug)]
pub struct Gateway {
    /// Identifiers currently in use by live orders.
    in_use: HashSet<u64>,
    /// Next candidate identifier for auto-assignment.
    next_auto_id: u64,
    /// Monotonic arrival-sequence number stamped on each forwarded order.
    next_seq: u64,
}

impl Default for Gateway {
    fn default() -> Self {
        Self::new()
    }
}

impl Gateway {
    /// Create a Gateway with no live orders.
    pub fn new() -> Self {
        Gateway {
            in_use: HashSet::new(),
            next_auto_id: 1,
            next_seq: 0,
        }
    }

    /// Number of order identifiers currently in use.
    pub fn live_order_count(&self) -> usize {
        self.in_use.len()
    }

    /// Whether `order_id` is currently in use by a live order.
    pub fn is_in_use(&self, order_id: u64) -> bool {
        self.in_use.contains(&order_id)
    }

    /// Record `order_id` as in use and advance the auto-id allocator past it.
    ///
    /// Returns `false` if the identifier was already in use (leaving state
    /// unchanged). Used to reserve identifiers reconstructed from the
    /// persistence journal so later auto-assigned ids never collide with them
    /// and duplicate detection stays correct.
    pub fn reserve(&mut self, order_id: u64) -> bool {
        if !self.in_use.insert(order_id) {
            return false;
        }
        if order_id >= self.next_auto_id {
            self.next_auto_id = order_id.wrapping_add(1);
        }
        true
    }

    /// Validate a JSON order body and reserve its identifier.
    ///
    /// On success the returned [`ValidatedOrder`] carries normalized, in-range
    /// fields and a unique identifier; the identifier is recorded as in use so
    /// a later duplicate is rejected.
    ///
    /// Errors (none of which reserve an identifier or forward a message):
    /// - [`GatewayError::InvalidJson`] — body is not valid JSON.
    /// - [`GatewayError::MissingField`] — a required field is absent.
    /// - [`GatewayError::InvalidField`] — `side`, `price`, `quantity`, or a
    ///   supplied `order_id` is malformed or out of range.
    /// - [`GatewayError::DuplicateOrderId`] — supplied id already in use.
    pub fn handle_new_order(&mut self, body: &[u8]) -> Result<ValidatedOrder, GatewayError> {
        let value: serde_json::Value = serde_json::from_slice(body)
            .map_err(|e| GatewayError::InvalidJson(e.to_string()))?;

        let obj = value
            .as_object()
            .ok_or_else(|| GatewayError::InvalidJson("expected a JSON object".to_string()))?;

        let side = parse_side(obj.get("side"))?;
        let price_ticks = parse_price(obj.get("price"))?;
        let quantity = parse_quantity(obj.get("quantity"))?;
        let order_id = self.resolve_order_id(obj.get("order_id"))?;

        // Reserve the identifier only once every field has validated, so a
        // rejected request never mutates Gateway state.
        self.in_use.insert(order_id);

        Ok(ValidatedOrder {
            order_id,
            side,
            price_ticks,
            quantity,
        })
    }

    /// Determine the order identifier for a request: preserve a supplied id
    /// after a duplicate check, or assign a fresh unique id when none is
    /// supplied.
    fn resolve_order_id(
        &mut self,
        field: Option<&serde_json::Value>,
    ) -> Result<u64, GatewayError> {
        match field {
            // Absent or explicit null → assign a unique identifier.
            None | Some(serde_json::Value::Null) => Ok(self.assign_unique_id()),
            Some(value) => {
                let supplied = value
                    .as_u64()
                    .ok_or_else(|| GatewayError::InvalidField("order_id".to_string()))?;
                if self.in_use.contains(&supplied) {
                    return Err(GatewayError::DuplicateOrderId(supplied));
                }
                Ok(supplied)
            }
        }
    }

    /// Allocate the next identifier not already in use.
    fn assign_unique_id(&mut self) -> u64 {
        while self.in_use.contains(&self.next_auto_id) {
            self.next_auto_id = self.next_auto_id.wrapping_add(1);
        }
        let id = self.next_auto_id;
        self.next_auto_id = self.next_auto_id.wrapping_add(1);
        id
    }

    /// Build the `NewOrder` message for an accepted order, stamping it with the
    /// next monotonic arrival-sequence number.
    pub fn new_order_message(&mut self, order: &ValidatedOrder) -> BinaryMessage {
        let seq = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1);
        BinaryMessage::NewOrder {
            order_id: order.order_id,
            side: order.side,
            price_ticks: order.price_ticks,
            quantity: order.quantity,
            seq,
        }
    }

    /// Encode an accepted order into `out` as a `NewOrder` message, returning
    /// the number of bytes written.
    ///
    /// Because `order` is already validated, the only possible failure is a
    /// caller-provided buffer that is too small ([`CodecError::InsufficientLength`]).
    pub fn encode_new_order(
        &mut self,
        order: &ValidatedOrder,
        out: &mut [u8],
    ) -> Result<usize, CodecError> {
        self.new_order_message(order).encode(out)
    }

    /// Validate a JSON order body and, on success, produce the `NewOrder`
    /// message to forward.
    ///
    /// This is the full request-side flow: an invalid request returns a
    /// [`GatewayError`] and produces no message, so nothing is forwarded to the
    /// matching engine; a valid request reserves its identifier and yields the
    /// message to enqueue on the ingress ring.
    pub fn accept_new_order(&mut self, body: &[u8]) -> Result<BinaryMessage, GatewayError> {
        let order = self.handle_new_order(body)?;
        Ok(self.new_order_message(&order))
    }

    // -- Response half: decode, convert, status mapping, timeout --

    /// Resolve a forwarded order's outcome from an optional engine response and
    /// the time elapsed since the `NewOrder` was forwarded.
    ///
    /// This is the timeout-aware entry point, modeled as a pure decision so it
    /// is deterministic and testable without a real clock:
    /// - if a response arrived within [`ENGINE_RESPONSE_CEILING`], it is decoded
    ///   and converted into a [`JsonResponse`];
    /// - otherwise — no response, or one that arrived only after the ceiling —
    ///   the engine is declared unavailable and HTTP 503 is returned via
    ///   [`GatewayError::EngineUnavailable`].
    pub fn resolve_engine_response(
        &self,
        response: Option<&[u8]>,
        elapsed: Duration,
    ) -> Result<JsonResponse, GatewayError> {
        match response {
            Some(bytes) if elapsed <= ENGINE_RESPONSE_CEILING => Self::handle_engine_response(bytes),
            // No response within the ceiling (or none at all): engine unavailable.
            _ => Err(GatewayError::EngineUnavailable),
        }
    }

    /// Decode a response message and convert it into a [`JsonResponse`].
    ///
    /// A response the codec cannot decode means the engine produced malformed
    /// output; the Gateway treats that as the engine being unavailable
    /// ([`GatewayError::EngineUnavailable`] → HTTP 503) rather than surfacing a
    /// codec error to the client.
    pub fn handle_engine_response(bytes: &[u8]) -> Result<JsonResponse, GatewayError> {
        let msg = Self::decode_engine_response(bytes)?;
        Self::response_to_json(&msg)
    }

    /// Decode a response message from the egress ring, mapping any codec failure
    /// to [`GatewayError::EngineUnavailable`].
    pub fn decode_engine_response(bytes: &[u8]) -> Result<BinaryMessage, GatewayError> {
        decode(bytes).map_err(|_: CodecError| GatewayError::EngineUnavailable)
    }

    /// Convert a decoded response message into a [`JsonResponse`].
    ///
    /// `Ack` and `Trade` are successful outcomes (HTTP 200); a `Reject` maps its
    /// reason to the appropriate HTTP status via [`reject_reason_status`]. A
    /// `NewOrder`/`CancelOrder` is a request, not a response — receiving one on
    /// the egress path indicates an engine fault and yields HTTP 503.
    pub fn response_to_json(msg: &BinaryMessage) -> Result<JsonResponse, GatewayError> {
        match *msg {
            BinaryMessage::Ack { order_id, kind } => Ok(JsonResponse {
                status: 200,
                body: serde_json::json!({
                    "order_id": order_id,
                    "status": kind,
                }),
            }),
            BinaryMessage::Trade {
                exec_seq,
                price_ticks,
                quantity,
                incoming_id,
                resting_id,
            } => Ok(JsonResponse {
                status: 200,
                body: serde_json::json!({
                    "status": "trade",
                    "exec_seq": exec_seq,
                    "price": ticks_to_price(price_ticks),
                    "quantity": quantity,
                    "incoming_id": incoming_id,
                    "resting_id": resting_id,
                }),
            }),
            BinaryMessage::Reject { order_id, reason } => Ok(JsonResponse {
                status: reject_reason_status(reason),
                body: serde_json::json!({
                    "order_id": order_id,
                    "status": "rejected",
                    "reason": reason,
                }),
            }),
            // Request messages are never valid engine responses.
            BinaryMessage::NewOrder { .. } | BinaryMessage::CancelOrder { .. } => {
                Err(GatewayError::EngineUnavailable)
            }
        }
    }
}

/// Convert a fixed-point `price_ticks` value (price × 100) back into a decimal
/// price for the JSON response body.
fn ticks_to_price(price_ticks: u64) -> f64 {
    price_ticks as f64 / 100.0
}

/// Map an engine [`RejectReason`] to the HTTP status the Gateway returns for it:
/// validation failures are client errors (400), references to absent or
/// no-longer-resting orders are conflicts (409), and an internal integrity
/// violation surfaces as engine-unavailable (503).
fn reject_reason_status(reason: RejectReason) -> u16 {
    match reason {
        RejectReason::InvalidPrice | RejectReason::InvalidQuantity => 400,
        RejectReason::OrderNotFound | RejectReason::NoLongerResting => 409,
        RejectReason::IntegrityViolation => 503,
    }
}

/// Validate the `side` field: present and equal to `"buy"` or `"sell"`.
fn parse_side(field: Option<&serde_json::Value>) -> Result<Side, GatewayError> {
    match field {
        None | Some(serde_json::Value::Null) => {
            Err(GatewayError::MissingField("side".to_string()))
        }
        Some(value) => match value.as_str() {
            Some("buy") => Ok(Side::Buy),
            Some("sell") => Ok(Side::Sell),
            _ => Err(GatewayError::InvalidField("side".to_string())),
        },
    }
}

/// Validate the `price` field and convert it to fixed-point ticks.
///
/// The raw price is range-checked against `[MIN_PRICE, MAX_PRICE]` before
/// rounding to ticks, so sub-tick values just below `0.01` are rejected rather
/// than rounded up into range.
fn parse_price(field: Option<&serde_json::Value>) -> Result<u64, GatewayError> {
    let value = match field {
        None | Some(serde_json::Value::Null) => {
            return Err(GatewayError::MissingField("price".to_string()))
        }
        Some(value) => value,
    };

    let price = value
        .as_f64()
        .filter(|p| value.is_number() && p.is_finite())
        .ok_or_else(|| GatewayError::InvalidField("price".to_string()))?;

    if price < MIN_PRICE || price > MAX_PRICE {
        return Err(GatewayError::InvalidField("price".to_string()));
    }

    let ticks = (price * 100.0).round() as u64;
    if ticks < limits::MIN_PRICE_TICKS || ticks > limits::MAX_PRICE_TICKS {
        return Err(GatewayError::InvalidField("price".to_string()));
    }
    Ok(ticks)
}

/// Validate the `quantity` field: a present integer within
/// `[MIN_GATEWAY_QUANTITY, MAX_GATEWAY_QUANTITY]`.
fn parse_quantity(field: Option<&serde_json::Value>) -> Result<u32, GatewayError> {
    let value = match field {
        None | Some(serde_json::Value::Null) => {
            return Err(GatewayError::MissingField("quantity".to_string()))
        }
        Some(value) => value,
    };

    // `as_u64` is `None` for floats (e.g. 1.5) and negative numbers, which
    // covers the "not an integer" rejection.
    let quantity = value
        .as_u64()
        .ok_or_else(|| GatewayError::InvalidField("quantity".to_string()))?;

    if quantity < limits::MIN_GATEWAY_QUANTITY as u64
        || quantity > limits::MAX_GATEWAY_QUANTITY as u64
    {
        return Err(GatewayError::InvalidField("quantity".to_string()));
    }
    Ok(quantity as u32)
}

#[cfg(test)]
mod tests {
    use super::*;
    use codec::{decode, layout};

    // --- Valid orders ------------------------------------------------------

    #[test]
    fn valid_order_with_supplied_id_is_preserved() {
        let mut gw = Gateway::new();
        let body = br#"{"order_id": 42, "side": "buy", "price": 123.45, "quantity": 10}"#;
        let order = gw.handle_new_order(body).expect("valid order");
        assert_eq!(
            order,
            ValidatedOrder {
                order_id: 42,
                side: Side::Buy,
                price_ticks: 12_345,
                quantity: 10,
            }
        );
        assert!(gw.is_in_use(42));
    }

    #[test]
    fn valid_sell_order_normalizes_side_and_price() {
        let mut gw = Gateway::new();
        let body = br#"{"side": "sell", "price": 0.01, "quantity": 1}"#;
        let order = gw.handle_new_order(body).expect("valid order");
        assert_eq!(order.side, Side::Sell);
        assert_eq!(order.price_ticks, limits::MIN_PRICE_TICKS);
        assert_eq!(order.quantity, 1);
    }

    #[test]
    fn order_without_id_gets_assigned_unique_id() {
        let mut gw = Gateway::new();
        let body = br#"{"side": "buy", "price": 1.00, "quantity": 5}"#;
        let first = gw.handle_new_order(body).expect("valid");
        let second = gw.handle_new_order(body).expect("valid");
        assert_ne!(first.order_id, second.order_id);
        assert!(gw.is_in_use(first.order_id));
        assert!(gw.is_in_use(second.order_id));
        assert_eq!(gw.live_order_count(), 2);
    }

    #[test]
    fn assigned_id_skips_ids_already_in_use() {
        let mut gw = Gateway::new();
        // Reserve the first auto-id candidate explicitly.
        gw.handle_new_order(br#"{"order_id": 1, "side": "buy", "price": 1.0, "quantity": 1}"#)
            .expect("valid");
        let auto = gw
            .handle_new_order(br#"{"side": "buy", "price": 1.0, "quantity": 1}"#)
            .expect("valid");
        assert_ne!(auto.order_id, 1);
    }

    #[test]
    fn boundary_prices_and_quantities_are_accepted() {
        let mut gw = Gateway::new();
        let max = gw
            .handle_new_order(
                br#"{"side": "buy", "price": 999999999.99, "quantity": 1000000000}"#,
            )
            .expect("max boundary");
        assert_eq!(max.price_ticks, limits::MAX_PRICE_TICKS);
        assert_eq!(max.quantity, limits::MAX_GATEWAY_QUANTITY);
    }

    // --- Duplicate detection -----------------------------------------------

    #[test]
    fn duplicate_supplied_id_is_rejected_with_409() {
        let mut gw = Gateway::new();
        let body = br#"{"order_id": 7, "side": "buy", "price": 1.0, "quantity": 1}"#;
        gw.handle_new_order(body).expect("first accepted");
        let err = gw.handle_new_order(body).expect_err("duplicate");
        assert_eq!(err, GatewayError::DuplicateOrderId(7));
        assert_eq!(err.http_status(), 409);
        // The duplicate must not have changed the live-order set.
        assert_eq!(gw.live_order_count(), 1);
    }

    // --- Identifier reservation (journal replay) ---------------------------

    #[test]
    fn reserve_records_id_and_blocks_auto_collision() {
        let mut gw = Gateway::new();
        assert!(gw.reserve(5));
        assert!(gw.is_in_use(5));
        assert_eq!(gw.live_order_count(), 1);

        // A re-reserve of the same id reports the conflict and changes nothing.
        assert!(!gw.reserve(5));
        assert_eq!(gw.live_order_count(), 1);

        // Auto-assignment must skip past the reserved id.
        let auto = gw
            .handle_new_order(br#"{"side": "buy", "price": 1.0, "quantity": 1}"#)
            .expect("valid");
        assert!(auto.order_id > 5);

        // A later supplied duplicate of the reserved id is still rejected.
        let err = gw
            .handle_new_order(br#"{"order_id": 5, "side": "buy", "price": 1.0, "quantity": 1}"#)
            .expect_err("duplicate");
        assert_eq!(err, GatewayError::DuplicateOrderId(5));
    }

    // --- Malformed / invalid requests --------------------------------------

    #[test]
    fn malformed_json_is_rejected_with_400() {
        let mut gw = Gateway::new();
        let err = gw.handle_new_order(b"{not json").expect_err("malformed");
        assert!(matches!(err, GatewayError::InvalidJson(_)));
        assert_eq!(err.http_status(), 400);
    }

    #[test]
    fn missing_field_is_rejected_naming_the_field() {
        let mut gw = Gateway::new();
        let err = gw
            .handle_new_order(br#"{"side": "buy", "quantity": 1}"#)
            .expect_err("missing price");
        assert_eq!(err, GatewayError::MissingField("price".to_string()));
        assert_eq!(err.http_status(), 400);
    }

    #[test]
    fn invalid_side_is_rejected() {
        let mut gw = Gateway::new();
        let err = gw
            .handle_new_order(br#"{"side": "hold", "price": 1.0, "quantity": 1}"#)
            .expect_err("invalid side");
        assert_eq!(err, GatewayError::InvalidField("side".to_string()));
    }

    #[test]
    fn price_below_minimum_is_rejected() {
        let mut gw = Gateway::new();
        let err = gw
            .handle_new_order(br#"{"side": "buy", "price": 0.009, "quantity": 1}"#)
            .expect_err("price too low");
        assert_eq!(err, GatewayError::InvalidField("price".to_string()));
    }

    #[test]
    fn price_above_maximum_is_rejected() {
        let mut gw = Gateway::new();
        let err = gw
            .handle_new_order(br#"{"side": "buy", "price": 1000000000.0, "quantity": 1}"#)
            .expect_err("price too high");
        assert_eq!(err, GatewayError::InvalidField("price".to_string()));
    }

    #[test]
    fn non_integer_quantity_is_rejected() {
        let mut gw = Gateway::new();
        let err = gw
            .handle_new_order(br#"{"side": "buy", "price": 1.0, "quantity": 1.5}"#)
            .expect_err("non-integer quantity");
        assert_eq!(err, GatewayError::InvalidField("quantity".to_string()));
    }

    #[test]
    fn out_of_range_quantity_is_rejected() {
        let mut gw = Gateway::new();
        let zero = gw
            .handle_new_order(br#"{"side": "buy", "price": 1.0, "quantity": 0}"#)
            .expect_err("zero quantity");
        assert_eq!(zero, GatewayError::InvalidField("quantity".to_string()));
        let too_big = gw
            .handle_new_order(br#"{"side": "buy", "price": 1.0, "quantity": 1000000001}"#)
            .expect_err("too-large quantity");
        assert_eq!(too_big, GatewayError::InvalidField("quantity".to_string()));
    }

    #[test]
    fn rejected_request_does_not_reserve_an_id_or_seq() {
        let mut gw = Gateway::new();
        gw.handle_new_order(br#"{"side": "buy"}"#).ok();
        assert_eq!(gw.live_order_count(), 0);
    }

    // --- Encoding accepted orders ------------------------------------------

    #[test]
    fn accepted_order_encodes_to_a_decodable_new_order() {
        let mut gw = Gateway::new();
        let order = gw
            .handle_new_order(br#"{"order_id": 5, "side": "sell", "price": 250.00, "quantity": 9}"#)
            .expect("valid");
        let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
        let written = gw.encode_new_order(&order, &mut buf).expect("encode");
        assert_eq!(written, layout::NEW_ORDER_LEN);

        match decode(&buf[..written]).expect("decode") {
            BinaryMessage::NewOrder {
                order_id,
                side,
                price_ticks,
                quantity,
                seq,
            } => {
                assert_eq!(order_id, 5);
                assert_eq!(side, Side::Sell);
                assert_eq!(price_ticks, 25_000);
                assert_eq!(quantity, 9);
                assert_eq!(seq, 0);
            }
            other => panic!("expected NewOrder, got {other:?}"),
        }
    }

    #[test]
    fn forwarded_orders_carry_monotonic_sequence_numbers() {
        let mut gw = Gateway::new();
        let body = br#"{"side": "buy", "price": 1.0, "quantity": 1}"#;
        let a = gw.handle_new_order(body).expect("valid");
        let b = gw.handle_new_order(body).expect("valid");
        let msg_a = gw.new_order_message(&a);
        let msg_b = gw.new_order_message(&b);
        let seq_a = match msg_a {
            BinaryMessage::NewOrder { seq, .. } => seq,
            _ => unreachable!(),
        };
        let seq_b = match msg_b {
            BinaryMessage::NewOrder { seq, .. } => seq,
            _ => unreachable!(),
        };
        assert!(seq_b > seq_a);
    }

    #[test]
    fn accept_new_order_combines_validation_and_encoding() {
        let mut gw = Gateway::new();
        let msg = gw
            .accept_new_order(br#"{"side": "buy", "price": 10.50, "quantity": 3}"#)
            .expect("accepted");
        assert!(matches!(
            msg,
            BinaryMessage::NewOrder {
                price_ticks: 1050,
                quantity: 3,
                side: Side::Buy,
                ..
            }
        ));
    }

    #[test]
    fn accept_new_order_rejects_before_forwarding() {
        let mut gw = Gateway::new();
        let err = gw
            .accept_new_order(br#"{"side": "buy", "price": -1.0, "quantity": 1}"#)
            .expect_err("invalid price");
        assert_eq!(err, GatewayError::InvalidField("price".to_string()));
        assert_eq!(gw.live_order_count(), 0);
    }
}
