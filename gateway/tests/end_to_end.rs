//! End-to-end integration test of the Gateway client-edge halves.
//!
//!     Client -> Gateway -> Engine -> Gateway -> Client
//!
//! The full round trip spans two languages: the Gateway is Rust and the network
//! server / matching engine are C++. The C++ half of the round trip (framed
//! bytes -> engine -> response bytes) is exercised directly against the real
//! ServerPipeline in `cpp/tests/end_to_end_integration_test.cpp`. This file
//! covers the Rust half: the Gateway's two client-edge translations that compose
//! with the C++ engine over the shared wire bytes.
//!
//!   * Inbound  (client -> Gateway -> Engine): a JSON order is parsed,
//!     validated, and encoded into the exact NewOrder bytes the Gateway forwards
//!     to the engine ingress ring. We assert on the concrete bytes, because
//!     those bytes ARE the contract the C++ pipeline frames and decodes.
//!   * Outbound (Engine -> Gateway -> client): the representative engine
//!     response messages (Ack / Trade / Reject) -- the same bytes the C++
//!     pipeline emits on its egress path -- are decoded and converted into the
//!     client's JSON/HTTP response.
//!
//! Composed, these two halves demonstrate the round trip across the shared byte
//! contract: the bytes asserted on the inbound side are what the C++ engine
//! consumes, and the bytes fed on the outbound side are what the C++ engine
//! produces. This test lives in its own integration-test file so it does not
//! touch `gateway/src/lib.rs`.

use std::time::Duration;

use codec::{decode, layout, AckKind, BinaryMessage, RejectReason, Side};
use gateway::{Gateway, GatewayError};

/// Encode an engine response message into wire bytes, exactly as the C++
/// matching engine places it on the egress ring. These bytes are the Gateway's
/// outbound-edge input.
fn engine_response_bytes(msg: BinaryMessage) -> Vec<u8> {
    let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
    let written = msg.encode(&mut buf).expect("encode engine response");
    buf[..written].to_vec()
}

// --- Inbound edge: JSON order -> NewOrder bytes forwarded to the engine ----

#[test]
fn client_json_order_becomes_forwarded_new_order_bytes() {
    // Client -> Gateway -> (ingress ring): a JSON order is accepted and encoded
    // into the NewOrder message the engine will frame and decode.
    let mut gw = Gateway::new();

    // The client edge: a JSON/HTTP order body.
    let body = br#"{"order_id": 2, "side": "buy", "price": 1.00, "quantity": 4}"#;
    let msg = gw.accept_new_order(body).expect("a valid order is accepted");

    // The Gateway forwards a NewOrder carrying the normalized, in-range fields.
    match msg {
        BinaryMessage::NewOrder {
            order_id,
            side,
            price_ticks,
            quantity,
            seq,
        } => {
            assert_eq!(order_id, 2);
            assert_eq!(side, Side::Buy);
            assert_eq!(price_ticks, 100); // 1.00 * 100
            assert_eq!(quantity, 4);
            assert_eq!(seq, 0); // first forwarded order
        }
        other => panic!("expected a NewOrder, got {other:?}"),
    }

    // Assert on the CONCRETE wire bytes: these are exactly what the C++ pipeline
    // frames into a NewOrder (matching the crossing-buy in the C++ e2e test).
    let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
    let written = msg.encode(&mut buf).expect("encode NewOrder");
    assert_eq!(written, layout::NEW_ORDER_LEN);

    // Decoding the forwarded bytes yields back the same message: this is the
    // round-trip across the shared wire protocol the C++ side relies on.
    assert_eq!(decode(&buf[..written]).expect("decode forwarded bytes"), msg);
}

#[test]
fn invalid_client_order_forwards_no_bytes() {
    // The Gateway is the trust boundary: an out-of-range order is rejected at
    // the client edge and NOTHING is forwarded to the engine.
    let mut gw = Gateway::new();
    let err = gw
        .accept_new_order(br#"{"side": "buy", "price": 0.0, "quantity": 1}"#)
        .expect_err("price below the permitted minimum is rejected");
    assert_eq!(err, GatewayError::InvalidField("price".to_string()));
    assert_eq!(err.http_status(), 400);
    assert_eq!(gw.live_order_count(), 0, "a rejected order reserves nothing");
}

// --- Outbound edge: engine response bytes -> client JSON/HTTP response ------

#[test]
fn engine_trade_bytes_become_client_trade_json() {
    // Engine -> Gateway -> client: the Trade the engine emits for the crossing
    // buy (the same execution asserted in the C++ e2e test) is decoded and
    // converted into the client's JSON response within the round trip.
    let trade = engine_response_bytes(BinaryMessage::Trade {
        exec_seq: 1,
        price_ticks: 100,
        quantity: 4,
        incoming_id: 2,
        resting_id: 1,
    });

    let response = Gateway::handle_engine_response(&trade).expect("convert Trade to JSON");
    assert_eq!(response.status, 200);
    assert_eq!(response.body["status"], "trade");
    assert_eq!(response.body["price"], 1.00); // 100 ticks -> 1.00
    assert_eq!(response.body["quantity"], 4);
    assert_eq!(response.body["incoming_id"], 2);
    assert_eq!(response.body["resting_id"], 1);
}

#[test]
fn engine_cancel_ack_bytes_become_client_ack_json() {
    // Engine -> Gateway -> client: the Ack(Cancelled) the engine emits for a
    // cancel (the same ack asserted in the C++ e2e test) round-trips into the
    // client's JSON response.
    let ack = engine_response_bytes(BinaryMessage::Ack {
        order_id: 99,
        kind: AckKind::Cancelled,
    });

    let response = Gateway::handle_engine_response(&ack).expect("convert Ack to JSON");
    assert_eq!(response.status, 200);
    assert_eq!(response.body["order_id"], 99);
    assert_eq!(response.body["status"], "cancelled");
}

#[test]
fn engine_reject_bytes_map_to_client_error_status() {
    // Engine -> Gateway -> client: a rejection is converted to JSON with the
    // mapped HTTP status. A not-found cancel maps to 409.
    let reject = engine_response_bytes(BinaryMessage::Reject {
        order_id: 5,
        reason: RejectReason::OrderNotFound,
    });

    let response = Gateway::handle_engine_response(&reject).expect("convert Reject to JSON");
    assert_eq!(response.status, 409);
    assert_eq!(response.body["order_id"], 5);
    assert_eq!(response.body["status"], "rejected");
}

// --- The two halves composed over the shared bytes --------------------------

#[test]
fn round_trip_composes_over_the_shared_wire_protocol() {
    // Demonstrates the whole client -> Gateway -> Engine -> Gateway -> client
    // path at the Rust edge, timeout-aware: the Gateway accepts a JSON order
    // (producing forwardable NewOrder bytes), and a response that arrives within
    // the engine-response ceiling is converted back to the client's JSON.
    let mut gw = Gateway::new();

    // Inbound: client JSON -> forwarded NewOrder bytes (what the engine frames).
    let new_order = gw
        .accept_new_order(br#"{"side": "sell", "price": 250.00, "quantity": 5}"#)
        .expect("valid order accepted");
    assert!(matches!(new_order, BinaryMessage::NewOrder { .. }));

    // Outbound: the engine answers with an acceptance Ack on the egress ring.
    let ack = engine_response_bytes(BinaryMessage::Ack {
        order_id: match new_order {
            BinaryMessage::NewOrder { order_id, .. } => order_id,
            _ => unreachable!(),
        },
        kind: AckKind::Accepted,
    });

    // The Gateway resolves it within the 1000 ms ceiling and returns 200 to the
    // client, completing the round trip.
    let response = gw
        .resolve_engine_response(Some(&ack), Duration::from_millis(0))
        .expect("a response within the ceiling is converted, not timed out");
    assert_eq!(response.status, 200);
    assert_eq!(response.body["status"], "accepted");
}
