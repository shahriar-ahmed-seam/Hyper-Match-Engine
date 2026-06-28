//! Unit test for the Gateway's engine-unavailable timeout.
//!
//! When the matching engine does not return a response message within the
//! 1000 ms engine-response ceiling, the Gateway must declare the engine
//! unavailable and respond HTTP 503. The decision is exercised through
//! `Gateway::resolve_engine_response`, which is a pure function over an optional
//! response and the elapsed wait, so the ceiling can be probed deterministically
//! without a real clock.
//!
//! This test lives in its own integration-test file to avoid touching
//! `gateway/src/lib.rs`.

use std::time::Duration;

use codec::{layout, AckKind, BinaryMessage};
use gateway::{Gateway, GatewayError, ENGINE_RESPONSE_CEILING};

/// Encode a successful `Ack` response message into a byte buffer, as the engine
/// would place on the egress ring. Used for the "arrives in time" case.
fn ack_response_bytes(order_id: u64) -> Vec<u8> {
    let mut buf = [0u8; layout::MAX_MESSAGE_LEN];
    let written = BinaryMessage::Ack {
        order_id,
        kind: AckKind::Accepted,
    }
    .encode(&mut buf)
    .expect("encode Ack response");
    buf[..written].to_vec()
}

#[test]
fn missing_engine_response_yields_503() {
    let gw = Gateway::new();
    // No response at all, regardless of how little time has elapsed: the engine
    // is unavailable.
    let result = gw.resolve_engine_response(None, Duration::from_millis(0));
    assert_eq!(result, Err(GatewayError::EngineUnavailable));
    assert_eq!(
        result.unwrap_err().http_status(),
        503,
        "a missing engine response must map to HTTP 503"
    );
}

#[test]
fn no_response_after_ceiling_yields_503() {
    let gw = Gateway::new();
    // The full ceiling elapsed with no response: still unavailable.
    let result = gw.resolve_engine_response(None, ENGINE_RESPONSE_CEILING);
    assert_eq!(result, Err(GatewayError::EngineUnavailable));
    assert_eq!(result.unwrap_err().http_status(), 503);
}

#[test]
fn response_arriving_after_ceiling_is_treated_as_unavailable() {
    let gw = Gateway::new();
    let response = ack_response_bytes(7);
    // A response exists, but it only arrived after the ceiling -- too late, so
    // the Gateway declares the engine unavailable.
    let late = ENGINE_RESPONSE_CEILING + Duration::from_millis(1);
    let result = gw.resolve_engine_response(Some(&response), late);
    assert_eq!(result, Err(GatewayError::EngineUnavailable));
    assert_eq!(result.unwrap_err().http_status(), 503);
}

#[test]
fn response_within_ceiling_yields_ok() {
    let gw = Gateway::new();
    let response = ack_response_bytes(7);

    // Comfortably within the ceiling.
    let early = gw
        .resolve_engine_response(Some(&response), Duration::from_millis(0))
        .expect("a response within the ceiling must be converted, not timed out");
    assert_eq!(early.status, 200);

    // Exactly at the ceiling is still in time (the boundary is inclusive).
    let at_ceiling = gw
        .resolve_engine_response(Some(&response), ENGINE_RESPONSE_CEILING)
        .expect("a response exactly at the ceiling must be accepted");
    assert_eq!(at_ceiling.status, 200);
}
